// Writes the transcript while the recording is still going, on the CPU, so it keeps going with the
// phone locked (the recording's audio session keeps the app running; iOS refuses background GPU work).
// After Stop it finishes the piece in flight and hands over to the pipeline, which continues from the
// same cursor (TranscriptSidecar) with is_final. Not used for Ask's voice questions.
// It comes first (LiveFirst): its pieces and the sections it writes never yield. The model is one actor, so a piece
// waits only for the call in flight (another note's piece, section or output); at its next boundary that run steps
// aside (ProcessingPipeline.stepsAside), so the pieces that follow get the model to themselves until Stop.
import Foundation
import Observation
import UIKit

/// A flag set on the main thread and read from the core's progress callback (any thread).
final class AtomicFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
    func set(_ v: Bool) { lock.lock(); value = v; lock.unlock() }
    /// Sets the flag; true only for the caller that set it first.
    func setOnce() -> Bool { lock.lock(); defer { lock.unlock() }; if value { return false }; value = true; return true }
}

/// The one transcription loop: from the sidecar's cursor, piece by piece, saving after every piece.
@MainActor
enum TranscriptRun {
    enum Stop: Equatable {
        /// Final: everything is transcribed.
        case end
        /// Still recording and not enough new audio yet.
        case wait
        /// `shouldContinue` said no before a piece.
        case yielded
    }

    /// shouldStop is asked during a model run (any thread) and cancels it: the run throws and the piece is
    /// lost, the cursor stays. shouldContinue is asked before every piece (main actor); shouldStart once
    /// the engine is free, right before the run (any thread). onPiece gets the progress after each piece.
    /// afterPiece then may write section summaries: the sections it returns are saved with the progress
    /// (nil: nothing new).
    static func run(audioPath: String, isFinal: Bool,
                    shouldStop: @escaping () -> Bool,
                    shouldContinue: () -> Bool = { true },
                    shouldStart: @escaping () -> Bool = { true },
                    beforePiece: () async -> Bool = { true },
                    onPiece: (TranscriptSidecar) -> Void = { _ in },
                    afterPiece: (TranscriptSidecar) async throws -> [NoteSection]? = { _ in nil }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        while true {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !shouldContinue() || !shouldStart() { return (progress, .yielded) }
            // The heat gate (rest at serious, stop at critical); false yields.
            if !(await beforePiece()) || !shouldContinue() || !shouldStart() { return (progress, .yielded) }
            // Loads the files if needed; the CPU itself is ensured inside the run (same actor turn).
            try await ModelManager.shared.loadIntoCore(for: .transcription)
            let next = try await CoreEngine.shared.transcribeNextPiece(path: audioPath, startSample: progress.cursor_sample,
                                                                       isFinal: isFinal, previousText: progress.previousText,
                                                                       backend: .cpu, shouldStart: shouldStart,
                                                                       shouldStop: shouldStop)
            switch next {
            case .end: return (progress, .end)
            case .wait: return (progress, .wait)
            case .skipped: return (progress, .yielded)
            case .piece(let startSec, let endSample, let text, let conf):
                progress.append(startSec: startSec, endSample: endSample, text: text, conf: conf)
                progress.save(audioPath: audioPath)   // skipped when the audio was deleted meanwhile
                onPiece(progress)
                if let sections = try await afterPiece(progress) {
                    progress.sections = sections
                    progress.save(audioPath: audioPath)
                }
            }
        }
    }

    /// The pieces as the note stores them.
    static func pieces(_ s: TranscriptSidecar) -> [TranscriptPiece] {
        s.pieces.map { TranscriptPiece(startSec: $0.start_sec, text: $0.text, conf: $0.conf) }
    }
}

/// One recording's live transcription. Its flags are its own, so a Stop or Cancel of one recording never
/// touches the next one.
@MainActor
final class LiveSession {
    let audioPath: String
    /// Stop or Cancel: no new piece starts (read on any thread).
    let finishing = AtomicFlag()
    /// Cancel: the piece in flight is dropped too.
    let abort = AtomicFlag()
    var task: Task<Void, Never>?
    init(audioPath: String) { self.audioPath = audioPath }
}

@MainActor
@Observable
final class LiveTranscriber {
    static let shared = LiveTranscriber()

    enum Status: Equatable {
        case idle
        /// The AI is not ready (or keeps failing for now): notes are written after Stop.
        case later
        case writing
        case behind(minutes: Int)
        case pausedHeat
        case pausedBattery
        /// Serious: resting between pieces.
        case cooling
    }

    /// A recording for notes is going (not an Ask voice question). Model runs use the CPU meanwhile.
    var isRecording: Bool { current != nil }
    /// Same as isRecording, readable on any thread (a model run's cancel check).
    nonisolated let recordingNow = AtomicFlag()
    private(set) var status: Status = .idle
    /// Transcript so far of the current recording, one entry per piece, oldest first.
    private(set) var lines: [String] = []

    /// The recording going now; Stop or Cancel clears it at once.
    private var current: LiveSession?
    /// Sessions whose loop has not ended yet (the current one, and stopped ones finishing their piece).
    @ObservationIgnored private var running: [String: LiveSession] = [:]
    @ObservationIgnored private var elapsed: () -> TimeInterval = { 0 }
    @ObservationIgnored private var transcribedSeconds: Double = 0

    /// Wait after this many failures in a row before trying again (the recording goes on meanwhile).
    static let failureBackoff: Double = 60

    private init() {}

    var statusLine: String {
        switch status {
        case .idle, .writing: return String(localized: "Writing as you talk")
        case .behind(let m): return String(localized: "About \(m) min behind")
        case .pausedHeat: return String(localized: "Paused while the phone cools down")
        case .pausedBattery: return String(localized: "Paused: battery low — notes will be written later")
        case .later: return String(localized: "Notes will be written after you stop")
        case .cooling: return String(localized: "Going slower to keep the phone cool")
        }
    }

    /// True while a session of this recording still has a loop (Stop waits for it in finish).
    func isTranscribing(audioPath: String) -> Bool { running[audioPath] != nil }

    /// A recording started (its WAV is being written at `audio`). Transcribes along if the AI is ready.
    func recordingStarted(audio: URL, elapsed: @escaping () -> TimeInterval) {
        if let old = current { stop(audioPath: old.audioPath) }   // never two current recordings
        let session = LiveSession(audioPath: audio.path)
        current = session
        recordingNow.set(true)
        lines = []
        transcribedSeconds = 0
        self.elapsed = elapsed
        heatPaused = false
        TranscriptSidecar.remove(audioPath: audio.path)   // a new recording always starts at 0
        // isReady also means the phone is supported.
        guard ModelManager.shared.isReady else { status = .later; return }
        status = .writing
        UIDevice.current.isBatteryMonitoringEnabled = true
        running[audio.path] = session
        // Earlier sessions still finishing their piece go first: one model run at a time.
        let earlier = running.values.filter { $0 !== session }.compactMap(\.task)
        session.task = Task {
            for t in earlier { await t.value }
            await self.loop(session)
            self.ended(session)
        }
    }

    /// Stop, synchronously: no new piece starts; the piece in flight is finished and saved.
    func stop(audioPath: String) {
        guard let s = current, s.audioPath == audioPath else { running[audioPath]?.finishing.set(true); return }
        s.finishing.set(true)
        current = nil
        recordingNow.set(false)
        status = .idle
    }

    /// Stop, then wait until that recording's loop has ended, so the pipeline can take over from the cursor.
    func finish(audioPath: String) async {
        stop(audioPath: audioPath)
        await running[audioPath]?.task?.value
    }

    /// Cancel: the piece in flight is dropped as well, so deleting the recording is not held up.
    func cancel(audioPath: String) async {
        running[audioPath]?.abort.set(true)
        await finish(audioPath: audioPath)
    }

    private func ended(_ s: LiveSession) {
        if running[s.audioPath] === s { running[s.audioPath] = nil }
        if running.isEmpty && current == nil { UIDevice.current.isBatteryMonitoringEnabled = false }
    }

    /// Paused at critical: stays paused until nominal or fair.
    @ObservationIgnored private var heatPaused = false

    private func pauseReason() -> LivePause? {
        let d = UIDevice.current
        let r = LivePause.reason(thermal: ProcessInfo.processInfo.thermalState, batteryLevel: d.batteryLevel,
                                 charging: d.batteryState == .charging || d.batteryState == .full, heatPaused: heatPaused)
        heatPaused = r == .heat
        return r
    }

    /// Before each piece: at serious rest 30 s (Stop/Cancel cut it short), then go on; false at critical.
    private func heatGate(_ s: LiveSession) async -> Bool {
        switch Heat.beforeRun(ProcessInfo.processInfo.thermalState) {
        case .go: return true
        case .pause: return false
        case .rest:
            show(s, .cooling)
            await nap(Heat.restSeconds, s)
            if Heat.beforeRun(ProcessInfo.processInfo.thermalState) == .pause { return false }
            refresh(s)
            return true
        }
    }

    /// Only the current recording drives the screen.
    private func show(_ s: LiveSession, _ st: Status) { if current === s { status = st } }

    private func refresh(_ s: LiveSession) {
        if let m = LiveLag.minutesBehind(recordedSeconds: elapsed(), transcribedSeconds: transcribedSeconds) {
            show(s, .behind(minutes: m))
        } else {
            show(s, .writing)
        }
    }

    private func loop(_ s: LiveSession) async {
        var failures = 0
        let finishing = s.finishing, abort = s.abort
        while !finishing.isSet, !Task.isCancelled {
            if let p = pauseReason() {
                show(s, p == .heat ? .pausedHeat : .pausedBattery)
                await nap(3, s)
                continue
            }
            refresh(s)
            do {
                let (_, stop) = try await TranscriptRun.run(
                    audioPath: s.audioPath, isFinal: false,
                    shouldStop: { abort.isSet || Heat.cancelsRun(ProcessInfo.processInfo.thermalState) },
                    shouldContinue: { !finishing.isSet && self.pauseReason() == nil },
                    shouldStart: { !finishing.isSet },
                    beforePiece: { await self.heatGate(s) },
                    onPiece: { p in
                        guard self.current === s else { return }
                        self.lines = p.pieces.map(\.text)
                        self.transcribedSeconds = p.transcribedSeconds
                        self.refresh(s)
                    },
                    afterPiece: { p in try await self.writeSections(s, p) })
                failures = 0
                if stop == .wait { await nap(3, s) }
                else if stop == .yielded, pauseReason() != nil { continue }
            } catch {
                if abort.isSet || finishing.isSet || Task.isCancelled { break }
                if Heat.cancelsRun(ProcessInfo.processInfo.thermalState) { continue }   // paused at the top
                failures += 1
                NSLog("[sve] live transcription failed (%ld): %@", failures, String(describing: error))
                if failures >= 3 {
                    // Back off instead of giving up for the whole recording; after Stop the pipeline does the rest.
                    show(s, .later)
                    await nap(Self.failureBackoff, s)
                    failures = 0
                } else {
                    await nap(3, s)
                }
            }
        }
    }

    /// After a piece: the sections that are due (at most one per piece, so transcription keeps the lead), only
    /// while this is the current recording and fewer than a section's worth of pieces waits to be transcribed.
    /// On the CPU, after the heat gate. Stop, Cancel or critical heat cancel the section in flight: a section on
    /// the CPU can take a couple of minutes, too long to hold up Stop, and the pipeline writes whatever is
    /// missing after Stop (on the GPU when the app is in front). Other failures skip sections for this piece.
    /// nil: nothing new.
    private func writeSections(_ s: LiveSession, _ p: TranscriptSidecar) async throws -> [NoteSection]? {
        let finishing = s.finishing, abort = s.abort
        guard current === s, !finishing.isSet,
              LiveSections.shouldWrite(recordedSeconds: elapsed(), transcribedSeconds: p.transcribedSeconds) else { return nil }
        do {
            let done = try await SectionWriter.writeDue(
                pieces: TranscriptRun.pieces(p), existing: p.sections, isFinal: false,
                lang: AppSettings.shared.notesLanguage, endSec: p.cursor_sample / Int64(TranscriptSidecar.sampleRate), limit: 1,
                beforeEach: {
                    guard !finishing.isSet, self.pauseReason() == nil, await self.heatGate(s) else { return false }
                    return !finishing.isSet
                },
                summarize: { pieces, lang in
                    try await ModelManager.shared.loadIntoCore(for: .transcription)   // the CPU, as transcription
                    return try await CoreEngine.shared.sectionSummarize(
                        pieces: pieces, lang: lang, backend: .cpu, shouldStart: { !finishing.isSet },
                        shouldStop: { finishing.isSet || abort.isSet || Heat.cancelsRun(ProcessInfo.processInfo.thermalState) })
                })
            return done == p.sections ? nil : done
        } catch {
            if abort.isSet || Task.isCancelled || Heat.cancelsRun(ProcessInfo.processInfo.thermalState) { throw error }
            if !finishing.isSet { NSLog("[sve] live section summary failed: %@", String(describing: error)) }
            return nil   // Stop: the loop ends at the next check; the pipeline takes over
        }
    }

    /// Sleeps up to `seconds`, waking early on Stop, Cancel or task cancellation.
    private func nap(_ seconds: Double, _ s: LiveSession) async {
        let end = Date().addingTimeInterval(seconds)
        while !s.finishing.isSet, Date() < end {
            do { try await Task.sleep(nanoseconds: 250_000_000) } catch { return }
        }
    }
}
