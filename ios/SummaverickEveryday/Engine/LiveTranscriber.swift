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
    /// afterPiece then may write section summaries and their notes and questions: the sections and parts of the
    /// progress it returns are saved (nil: nothing new).
    static func run(audioPath: String, isFinal: Bool,
                    shouldStop: @escaping () -> Bool,
                    shouldContinue: () -> Bool = { true },
                    shouldStart: @escaping () -> Bool = { true },
                    beforePiece: () async -> Bool = { true },
                    onPiece: (TranscriptSidecar) -> Void = { _ in },
                    afterPiece: (TranscriptSidecar) async throws -> TranscriptSidecar? = { _ in nil }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        while true {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !shouldContinue() || !shouldStart() { return (progress, .yielded) }
            // The heat gate (the heat ladder's rest; paused stops); false yields.
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
                if let updated = try await afterPiece(progress) {
                    progress.sections = updated.sections
                    progress.parts = updated.parts
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
    /// Live parts (LiveParts): stopped tries per section, and sections left for after Stop (a try failed).
    var partCancels: [Int: Int] = [:]
    var partsLeft: Set<Int> = []
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
        /// The heat ladder at eased or minimal: resting between pieces.
        case cooling
    }

    /// A recording for notes is going (not an Ask voice question). Model runs use the CPU meanwhile.
    var isRecording: Bool { current != nil }
    /// Same as isRecording, readable on any thread (a model run's cancel check).
    nonisolated let recordingNow = AtomicFlag()
    /// Live transcription can run now: a recording is going, its loop runs, and it is not waiting for the AI
    /// (.later, also its failure backoff) nor paused for heat or battery. Only then do other runs step aside for
    /// it (LiveFirst). Main actor.
    var liveRunning: Bool {
        guard let c = current, running[c.audioPath] === c else { return false }
        switch status {
        case .later, .pausedHeat, .pausedBattery: return false
        case .idle, .writing, .behind, .cooling: return true
        }
    }
    /// Same as liveRunning, readable on any thread (a model run's cancel check).
    nonisolated let liveRunningNow = AtomicFlag()
    private func liveChanged() { liveRunningNow.set(liveRunning) }
    private(set) var status: Status = .idle { didSet { liveChanged() } }
    /// Transcript so far of the current recording, one entry per piece, oldest first.
    private(set) var lines: [String] = []

    /// The recording going now; Stop or Cancel clears it at once.
    private var current: LiveSession? { didSet { liveChanged() } }
    /// Sessions whose loop has not ended yet (the current one, and stopped ones finishing their piece).
    @ObservationIgnored private var running: [String: LiveSession] = [:] { didSet { liveChanged() } }
    @ObservationIgnored private var elapsed: () -> TimeInterval = { 0 }
    @ObservationIgnored private var transcribedSeconds: Double = 0

    /// Wait after this many failures in a row before trying again (the recording goes on meanwhile).
    static let failureBackoff: Double = 60

    /// The app is in front (active), readable on any thread: a live part stops at once when it leaves.
    nonisolated let inFront = AtomicFlag()
    @ObservationIgnored private var frontObservers: [NSObjectProtocol] = []

    private init() {
        let flag = inFront
        for (name, active) in [(UIApplication.didBecomeActiveNotification, true),
                               (UIApplication.willResignActiveNotification, false),
                               (UIApplication.didEnterBackgroundNotification, false)] {
            frontObservers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                flag.set(active)
            })
        }
    }

    var statusLine: String {
        switch status {
        case .idle, .writing: return String(localized: "Writing as you talk")
        case .behind(let m): return String(localized: "Running about \(m) min behind")
        case .pausedHeat: return String(localized: "Waiting for the phone to cool down")
        case .pausedBattery: return String(localized: "Battery's low, so I'll write your notes later")
        case .later: return String(localized: "I'll write your notes after you stop")
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
        if running.isEmpty && current == nil && !HeatGovernor.shared.holdsBatteryMonitoring {
            UIDevice.current.isBatteryMonitoringEnabled = false   // the heat ladder may still be reading it
        }
    }

    /// Heat (the shared ladder at paused, which lasts a minute of calm past critical) or battery below 20%.
    private func pauseReason() -> LivePause? {
        let d = UIDevice.current
        return LivePause.reason(heat: HeatGovernor.shared.level, batteryLevel: d.batteryLevel,
                                charging: d.batteryState == .charging || d.batteryState == .full)
    }

    /// Before each piece and section: the shared heat ladder (HeatGovernor), as the pipeline's gate. Eased or
    /// minimal: rest 5 or 20 s (Stop/Cancel or a cooler phone cut it short), then go on; the CPU run takes the
    /// ladder's threads (CoreEngine). False when paused.
    private func heatGate(_ s: LiveSession) async -> Bool {
        let heat = HeatGovernor.shared
        let level = heat.level
        if level == .paused { return false }
        guard HeatLadder.showsCooling(level) else { return true }
        show(s, .cooling)
        let start = HeatGovernor.now
        while !s.finishing.isSet, !HeatLadder.restOver(level: heat.level, started: start, now: HeatGovernor.now) {
            do { try await Task.sleep(nanoseconds: 250_000_000) } catch { break }
        }
        if heat.level == .paused { return false }
        refresh(s)
        return true
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
                    afterPiece: { p in
                        var next = p
                        if let sections = try await self.writeSections(s, p) { next.sections = sections }
                        next = await self.writeLiveParts(s, next)
                        return next == p ? nil : next
                    })
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

    /// After a piece (and its section, if one was due): the notes and questions of the oldest section that lacks
    /// them, written while recording so only the last minutes are left after Stop (LiveParts). Only with the app in
    /// front, the heat full or eased and no piece waiting; on the GPU (free while the CPU transcribes; the next piece
    /// switches back). Stopped at once when a piece is waiting, the app leaves the front, the phone gets hot, or on
    /// Stop or Cancel; a kind finished before that is kept. Returns the progress with the parts kept.
    private func writeLiveParts(_ s: LiveSession, _ p: TranscriptSidecar) async -> TranscriptSidecar {
        let finishing = s.finishing, abort = s.abort, inFront = self.inFront
        let lang = AppSettings.shared.notesLanguage
        let skip = s.partsLeft.union(s.partCancels.filter { $0.value >= LiveParts.maxCancels }.map(\.key))
        guard current === s, !finishing.isSet, let due = p.partsDue(lang: lang, skip: skip) else { return p }
        inFront.set(UIApplication.shared.applicationState == .active)
        let recorded = elapsed(), transcribed = p.transcribedSeconds
        let sec = due.section
        guard LiveParts.mayWrite(appActive: inFront.isSet, heat: HeatGovernor.shared.level,
                                 piecesWaiting: LiveParts.piecesWaiting(recordedSeconds: recorded, transcribedSeconds: transcribed),
                                 cancels: s.partCancels[sec.idx] ?? 0) else { return p }
        let started = Date()
        let mustStop: () -> Bool = {
            if finishing.isSet || abort.isSet { return true }
            let waiting = LiveParts.piecesWaiting(recordedSeconds: recorded + Date().timeIntervalSince(started),
                                                  transcribedSeconds: transcribed)
            return LiveParts.mustStop(appActive: inFront.isSet, piecesWaiting: waiting,
                                      thermal: ProcessInfo.processInfo.thermalState)
        }
        let names = due.kinds.map { $0 == LivePart.notes ? "notes" : "questions" }.joined(separator: " and ")
        NSLog("[sve] live part %ld: writing %@ (%.0f s to the next piece)", sec.idx + 1, names,
              LiveParts.pieceSeconds - (recorded - transcribed))
        var out = p
        do {
            try await ModelManager.shared.loadIntoCore(for: .transcription)   // the files; the GPU is switched to inside
            let pieces = Array(TranscriptRun.pieces(p)[sec.firstPiece...sec.lastPiece])
            guard let w = try await CoreEngine.shared.writeLiveParts(
                pieces: pieces, summary: sec.summary, idx: sec.idx, kinds: due.kinds, lang: lang, backend: .gpu,
                shouldStart: { !mustStop() }, shouldStop: mustStop) else { return p }
            for r in w.parts {
                out.keep(LivePart(idx: sec.idx, kind: r.kind, lang: lang, first: sec.firstPiece, last: sec.lastPiece,
                                  text: r.text, cut: r.cut))
            }
            let kept = w.parts.map { $0.kind == LivePart.notes ? "notes" : "questions" }.joined(separator: " and ")
            let secs = Date().timeIntervalSince(started)
            if w.cancelled {
                let n = (s.partCancels[sec.idx] ?? 0) + 1
                s.partCancels[sec.idx] = n
                NSLog("[sve] live part %ld: stopped after %.0f s (%@; try %ld of %ld)%@", sec.idx + 1, secs,
                      inFront.isSet ? "a piece is waiting or the phone is warm" : "the app left the front", n,
                      LiveParts.maxCancels, kept.isEmpty ? "" : ", kept \(kept)")
            } else {
                if out.partsDue(lang: lang, skip: skip)?.section.idx == sec.idx { s.partsLeft.insert(sec.idx) }
                NSLog("[sve] live part %ld: kept %@ in %.0f s", sec.idx + 1, kept.isEmpty ? "nothing" : kept, secs)
            }
        } catch {
            // A GPU refused (the app left the front) or a failed load: tried again only while tries are left.
            s.partCancels[sec.idx] = (s.partCancels[sec.idx] ?? 0) + 1
            if !finishing.isSet { NSLog("[sve] live part %ld failed: %@", sec.idx + 1, String(describing: error)) }
        }
        return out
    }

    /// Sleeps up to `seconds`, waking early on Stop, Cancel or task cancellation.
    private func nap(_ seconds: Double, _ s: LiveSession) async {
        let end = Date().addingTimeInterval(seconds)
        while !s.finishing.isSet, Date() < end {
            do { try await Task.sleep(nanoseconds: 250_000_000) } catch { return }
        }
    }
}
