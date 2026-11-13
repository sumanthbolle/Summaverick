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
        /// Whisper only: it may not run the next group (the app went to the background); Gemma goes on from the cursor.
        case handedOff
    }

    /// shouldStop is asked during a model run (any thread) and cancels it: the run throws and the piece is
    /// lost, the cursor stays. shouldContinue is asked before every piece (main actor); shouldStart once
    /// the engine is free, right before the run (any thread). onPiece gets the progress after each piece.
    /// afterPiece then may write section summaries and their notes and questions: the sections and parts of the
    /// progress it returns are saved (nil: nothing new). backend: asked before every piece, never during one (live:
    /// the CPU; after Stop the pipeline's StepBackend). A GPU piece that fails (the app left the front) is tried once
    /// more on the CPU, unless shouldStop says the run is over.
    static func run(audioPath: String, isFinal: Bool,
                    shouldStop: @escaping () -> Bool,
                    shouldContinue: () -> Bool = { true },
                    shouldStart: @escaping () -> Bool = { true },
                    backend: () async -> ModelBackend = { .cpu },
                    beforePiece: () async -> Bool = { true },
                    onPiece: (TranscriptSidecar) -> Void = { _ in },
                    afterPiece: (TranscriptSidecar) async throws -> TranscriptSidecar? = { _ in nil }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        while true {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !shouldContinue() || !shouldStart() { return (progress, .yielded) }
            // The heat gate (the heat ladder's rest; paused stops); false yields.
            if !(await beforePiece()) || !shouldContinue() || !shouldStart() { return (progress, .yielded) }
            // Loads the files on the backend for this piece; it is ensured again inside the run (same actor turn).
            let b = await backend()
            try await ModelManager.shared.loadIntoCore(backend: b)
            let start = progress.cursor_sample, previous = progress.previousText
            let next: CoreEngine.NextPiece
            do {
                next = try await CoreEngine.shared.transcribeNextPiece(path: audioPath, startSample: start, isFinal: isFinal,
                                                                       previousText: previous, backend: b,
                                                                       shouldStart: shouldStart, shouldStop: shouldStop)
            } catch {
                guard b == .gpu, !shouldStop(), !Task.isCancelled else { throw error }
                NSLog("[sve] a GPU piece stopped (%@); this piece again on the CPU", String(describing: error))
                next = try await CoreEngine.shared.transcribeNextPiece(path: audioPath, startSample: start, isFinal: isFinal,
                                                                       previousText: previous, backend: .cpu,
                                                                       shouldStart: shouldStart, shouldStop: shouldStop)
            }
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

    /// After Stop, Whisper from the sidecar's cursor to the end (the recording is final), the same loop as `run`: the
    /// heat gate before each group, the cursor saved after each. A group that isn't clearly Telugu is kept pending for
    /// Gemma (fillPending). backend: asked before every group, after the heat gate and right before any model call
    /// (WhisperChoice); nil (the app went to the background) ends the run with `.handedOff`, Whisper's GPU never
    /// started in the background, and Gemma goes on from the cursor. Whisper is loaded before every group, a no-op
    /// unless Ask or a recording freed it meanwhile. cutShort (any thread; the app went to the background during the
    /// load or a group) cancels the group in flight and also hands off. A failed group otherwise throws, with no
    /// second try on the CPU: the caller goes on with Gemma from the saved cursor.
    static func runWhisper(audioPath: String,
                           shouldStop: @escaping () -> Bool,
                           backend: () async -> ModelBackend? = { .gpu },
                           cutShort: @escaping () -> Bool = { false },
                           beforePiece: () async -> Bool = { true },
                           onPiece: (TranscriptSidecar) -> Void = { _ in }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        while true {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !(await beforePiece()) { return (progress, .yielded) }
            guard let b = await backend() else { return (progress, .handedOff) }
            let model = ModelManager.shared.whisperPath
            let next: CoreEngine.WhisperPiece
            do {
                try await CoreEngine.shared.loadWhisper(path: model, backend: b)
                next = try await CoreEngine.shared.whisperNextPiece(path: audioPath, startSample: progress.cursor_sample,
                                                                    isFinal: true, shouldStop: { shouldStop() || cutShort() })
            } catch {
                guard cutShort(), !shouldStop(), !Task.isCancelled else { throw error }
                NSLog("[sve] whisper group cut short: the app went to the background; Gemma goes on from the cursor")
                return (progress, .handedOff)
            }
            switch next {
            case .end: return (progress, .end)
            case .wait: return (progress, .wait)
            case .piece(let startSec, let endSample, let text, let conf, let needsGemma):
                if needsGemma {
                    progress.appendPending(startSec: startSec, endSample: endSample)
                } else {
                    progress.append(startSec: startSec, endSample: endSample, text: text, conf: conf)
                }
                progress.save(audioPath: audioPath)
                onPiece(progress)
            }
        }
    }

    /// After Stop, Shruti from the sidecar's cursor, the `runWhisper` loop shape: the heat gate before each piece, the
    /// cursor saved after each. Pieces are planned like Gemma's (sve_plan_next_piece, at most 28 s). A piece Shruti
    /// heard nothing in though it was not silent (ShrutiPiece.needsGemma) is kept pending for Gemma (fillPending).
    /// threads: asked before every piece (the heat ladder's count); loading is a no-op at the same count. The read and
    /// the decode run off the main actor; a decode is not cut short, shouldStop is asked between pieces. A failed
    /// read or load throws: the caller goes on with Gemma from the saved cursor.
    static func runShruti(audioPath: String, isFinal: Bool,
                          shouldStop: @escaping () -> Bool,
                          threads: () -> Int,
                          beforePiece: () async -> Bool = { true },
                          onPiece: (TranscriptSidecar) -> Void = { _ in }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        var n = 0
        while true {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !(await beforePiece()) { return (progress, .yielded) }
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            let start = progress.cursor_sample, count = threads()
            let began = Date()
            let heard = try await Task.detached(priority: .userInitiated) {
                try ShrutiPieceRun.next(audioPath: audioPath, start: start, isFinal: isFinal, threads: count)
            }.value
            switch heard {
            case .end: return (progress, .end)
            case .wait: return (progress, .wait)
            case .piece(let end, let text, let peak):
                n += 1
                let startSec = Int(start / Int64(ShrutiRecognizer.sampleRate))
                progress.noteShrutiPeak(peak)   // ShrutiPiece.retryWithGemma, when the note ends with no text
                if ShrutiPiece.needsGemma(text: text, peak: peak) {
                    progress.appendPending(startSec: startSec, endSample: end)
                } else {
                    progress.append(startSec: startSec, endSample: end, text: text, conf: nil)
                }
                NSLog("[sve] shruti piece %d: %.1f s audio in %.0f ms, %d chars", n,
                      Double(end - start) / Double(ShrutiRecognizer.sampleRate),
                      Date().timeIntervalSince(began) * 1000, text.count)
                progress.save(audioPath: audioPath)   // skipped when the audio was deleted meanwhile
                onPiece(progress)
            }
        }
    }

    /// Gemma fills the pieces Whisper left pending, oldest first, saving after each. backend: asked before every
    /// piece (loading Gemma there frees Whisper); a GPU piece that fails is tried once more on the CPU, unless
    /// shouldStop says the run is over. beforePiece false (the heat gate) throws CancellationError; the pieces
    /// filled so far are kept. onPiece gets the progress after each fill (the bar counts a pending piece once filled).
    static func fillPending(audioPath: String,
                            shouldStop: @escaping () -> Bool,
                            backend: () async -> ModelBackend = { .cpu },
                            beforePiece: () async -> Bool = { true },
                            onPiece: (TranscriptSidecar) -> Void = { _ in }) async throws -> TranscriptSidecar {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        // Re-read after each fill: an empty text removes its piece and moves the later ones down.
        while let i = progress.pendingIndices.first {
            if shouldStop() || Task.isCancelled { throw CancellationError() }
            if !(await beforePiece()) { throw CancellationError() }
            let p = progress.pieces[i]
            guard let start = p.start_sample, let end = p.end_sample, end > start else {
                progress.fill(i, text: "", conf: nil)   // no span to hear (should not happen): dropped
                progress.save(audioPath: audioPath)
                onPiece(progress)
                continue
            }
            let previous = progress.pieces[..<i].last(where: { !$0.text.isEmpty && $0.pending != true })?.text
            let b = await backend()
            try await ModelManager.shared.loadIntoCore(backend: b)
            let heard: (String, Double?)
            do {
                heard = try await CoreEngine.shared.transcribeSpan(path: audioPath, start: start, end: end,
                                                                   previousText: previous, backend: b, shouldStop: shouldStop)
            } catch {
                guard b == .gpu, !shouldStop(), !Task.isCancelled else { throw error }
                NSLog("[sve] a GPU span stopped (%@); this span again on the CPU", String(describing: error))
                heard = try await CoreEngine.shared.transcribeSpan(path: audioPath, start: start, end: end,
                                                                   previousText: previous, backend: .cpu, shouldStop: shouldStop)
            }
            progress.fill(i, text: heard.0, conf: heard.1)
            progress.save(audioPath: audioPath)
            onPiece(progress)
        }
        return progress
    }

    /// The pieces as the note stores them.
    static func pieces(_ s: TranscriptSidecar) -> [TranscriptPiece] {
        s.pieces.filter { $0.pending != true }.map { TranscriptPiece(startSec: $0.start_sec, text: $0.text, conf: $0.conf) }
    }
}

/// One Shruti piece, off the main actor: plan it, read its samples, decode them.
enum ShrutiPieceRun {
    enum Failure: Error { case unreadableAudio, planDidNotMove }
    enum Heard { case wait, end, piece(end: Int64, text: String, peak: Float) }

    static func next(audioPath: String, start: Int64, isFinal: Bool, threads: Int) throws -> Heard {
        var end: Int64 = 0
        switch sve_plan_next_piece(audioPath, start, isFinal ? 1 : 0, &end) {
        case 0: break
        case 1: return .wait
        case 2: return .end
        default: throw Failure.unreadableAudio
        }
        guard end > start else { throw Failure.planDidNotMove }   // should not happen; Gemma goes on
        var raw: UnsafeMutablePointer<Float>?
        let count = sve_read_samples(audioPath, start, end, &raw)
        defer { if let raw { sve_free_samples(raw) } }
        guard count >= 0 else { throw Failure.unreadableAudio }
        let samples = raw.map { Array(UnsafeBufferPointer(start: $0, count: Int(count))) } ?? []
        let peak = samples.reduce(Float(0)) { max($0, abs($1)) }
        let text = try ShrutiRecognizer.shared.decode(samples, threads: threads)   // loads in the same turn
        return .piece(end: end, text: text, peak: peak)
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
    /// LiveShruti: Shruti writes the pieces while recording (Gemma is not used by the live path).
    var usesShruti = false
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
    /// The Record screen shows the "the phone is warm" tip (LivePace.showsWarmTip): read at the start, at Stop and at
    /// every thermal change.
    private(set) var phoneWarm = false
    private func readWarm() {
        let warm = LivePace.showsWarmTip(recording: current != nil, thermal: ProcessInfo.processInfo.thermalState)
        if warm != phoneWarm { phoneWarm = warm }
    }

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
        frontObservers.append(NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { LiveTranscriber.shared.readWarm() }
        })
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
        HeatGovernor.threads.setRecording(true)   // 2 and 2 CPU threads from eased up (LivePace.threads)
        readWarm()
        lines = []
        transcribedSeconds = 0
        self.elapsed = elapsed
        TranscriptSidecar.remove(audioPath: audio.path)   // a new recording always starts at 0
        // Shruti while recording (LiveShruti, off by default): its pieces are final; Gemma is never loaded here.
        session.usesShruti = LiveShruti.isOn(shrutiReady: ShrutiRecognizer.shared.ready)
        // isReady also means the phone is supported.
        guard session.usesShruti || ModelManager.shared.isReady else { status = .later; return }
        // Off by default (LiveTranscribe): the recording stays cool and the transcript is written after Stop.
        guard session.usesShruti || LiveTranscribe.isOn() else {
            NSLog("[sve] live: off, every piece is written after Stop")
            status = .later
            return
        }
        status = .writing
        UIDevice.current.isBatteryMonitoringEnabled = true
        running[audio.path] = session
        // Earlier sessions still finishing their piece go first: one model run at a time.
        let earlier = running.values.filter { $0 !== session }.compactMap(\.task)
        session.task = Task {
            for t in earlier { await t.value }
            if session.usesShruti { await self.loopShruti(session) } else { await self.loop(session) }
            self.ended(session)
        }
    }

    /// Stop, synchronously: no new piece starts; the piece in flight is finished and saved.
    func stop(audioPath: String) {
        guard let s = current, s.audioPath == audioPath else { running[audioPath]?.finishing.set(true); return }
        s.finishing.set(true)
        current = nil
        recordingNow.set(false)
        HeatGovernor.threads.setRecording(false)
        readWarm()
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

    /// Before each piece and section: the shared heat ladder (HeatGovernor), paced to keep the phone cool while
    /// recording (LivePace). Eased or minimal: rest 20 or 60 s (eased only 5 s past about 15 minutes of lag); Stop,
    /// Cancel or a cooler phone cut it short, a warmer one lengthens it; then go on. The audio keeps recording
    /// meanwhile and the lag is caught up later. The CPU run takes the threads (2 and 2 while recording warm:
    /// CoreEngine). False when paused.
    private func heatGate(_ s: LiveSession) async -> Bool {
        let heat = HeatGovernor.shared
        let level = heat.level
        if level == .paused { return false }
        guard HeatLadder.showsCooling(level) else { return true }
        show(s, .cooling)
        let lag = { LiveParts.piecesWaiting(recordedSeconds: self.elapsed(), transcribedSeconds: self.transcribedSeconds) }
        let lagNow = current === s ? lag() : 0
        NSLog("[sve] live: heat %@, resting %d s (lag %d pieces)", level.name,
              Int32(LivePace.restSeconds(level: level, lagPieces: lagNow)), Int32(clamping: lagNow))
        let start = HeatGovernor.now
        while !s.finishing.isSet,
              !LivePace.restOver(level: heat.level, lagPieces: current === s ? lag() : 0, started: start,
                                 now: HeatGovernor.now) {
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

    /// Before each live Shruti piece: rest as long as the last piece worked (HeatLadder.lightRestSeconds, no floor);
    /// Stop, Cancel or a paused ladder cut it short. False when paused (the loop pauses instead).
    private func shrutiGate(_ s: LiveSession, lastWork: Double) async -> Bool {
        let heat = HeatGovernor.shared
        let rest = HeatLadder.lightRestSeconds(heat.level, lastWork: lastWork)
        if rest == .infinity { return false }
        if rest > 0 {
            show(s, .cooling)
            let start = HeatGovernor.now
            while !s.finishing.isSet, heat.level != .paused, HeatGovernor.now - start < rest {
                do { try await Task.sleep(nanoseconds: 250_000_000) } catch { break }
            }
            if heat.level == .paused { return false }
            refresh(s)
        }
        return true
    }

    /// The live loop with Shruti: the pieces are final (the cursor they leave is where the pipeline goes on after
    /// Stop). No sections or live parts here: transcription only.
    private func loopShruti(_ s: LiveSession) async {
        var failures = 0
        var lastWork: Double = 0
        var pieces = 0
        var lastLog = 0
        var pieceStart = Date()
        let finishing = s.finishing, abort = s.abort
        while !finishing.isSet, !Task.isCancelled {
            if let p = pauseReason() {
                show(s, p == .heat ? .pausedHeat : .pausedBattery)
                await nap(3, s)
                continue
            }
            refresh(s)
            do {
                let (_, stop) = try await TranscriptRun.runShruti(
                    audioPath: s.audioPath, isFinal: false,
                    shouldStop: { abort.isSet || finishing.isSet || Heat.cancelsRun(ProcessInfo.processInfo.thermalState) },
                    threads: { min(2, ShrutiThreads.count(HeatGovernor.shared.level)) },
                    beforePiece: {
                        guard self.pauseReason() == nil else { return false }
                        let go = await self.shrutiGate(s, lastWork: lastWork)
                        pieceStart = Date()
                        return go
                    },
                    onPiece: { p in
                        pieces += 1
                        lastWork = Date().timeIntervalSince(pieceStart)
                        guard self.current === s else { return }
                        self.lines = p.pieces.map(\.text)
                        self.transcribedSeconds = p.transcribedSeconds
                        self.refresh(s)
                        let minutes = Int(self.elapsed() / 300)
                        if minutes > lastLog {
                            lastLog = minutes
                            NSLog("[sve] live shruti: thermal %ld, %d pieces, lag %.0f s",
                                  ProcessInfo.processInfo.thermalState.rawValue, Int32(pieces),
                                  max(0, self.elapsed() - p.transcribedSeconds))
                        }
                    })
                failures = 0
                if stop == .wait { await nap(3, s) }
            } catch {
                if abort.isSet || finishing.isSet || Task.isCancelled { break }
                if Heat.cancelsRun(ProcessInfo.processInfo.thermalState) { continue }
                failures += 1
                NSLog("[sve] live shruti failed (%ld): %@", failures, String(describing: error))
                if failures >= 3 {
                    show(s, .later)
                    await nap(Self.failureBackoff, s)
                    failures = 0
                } else {
                    await nap(3, s)
                }
            }
        }
        if abort.isSet { await Task.detached { ShrutiRecognizer.shared.unload() }.value }
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
    /// front, the heat full or eased and at most LiveParts.startMaxWaiting pieces waiting; on the GPU (free while the
    /// CPU transcribes; the next piece switches back). Stopped at once when LiveParts.stopWaiting pieces wait, the app
    /// leaves the front, the heat goes above eased or the phone gets hot, or on Stop or Cancel; a kind finished
    /// before that is kept, and a stop for Stop or Cancel is not counted as a stopped try. Returns the progress with
    /// the parts kept.
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
                                      heat: HeatGovernor.threads.currentLevel,
                                      thermal: ProcessInfo.processInfo.thermalState)
        }
        let names = due.kinds.map { $0 == LivePart.notes ? "notes" : "questions" }.joined(separator: " and ")
        NSLog("[sve] live part %ld: writing %@ (lag %ld pieces)", sec.idx + 1, names,
              LiveParts.piecesWaiting(recordedSeconds: recorded, transcribedSeconds: transcribed))
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
            let lag = LiveParts.piecesWaiting(recordedSeconds: recorded + secs, transcribedSeconds: transcribed)
            if w.cancelled && (finishing.isSet || abort.isSet) {
                // Stop or Cancel: not the part's fault, so not a stopped try (the pipeline writes it after Stop).
                NSLog("[sve] live part %ld: stopped (Stop) after %.0f s%@", sec.idx + 1, secs,
                      kept.isEmpty ? "" : ", kept \(kept)")
            } else if w.cancelled {
                let n = (s.partCancels[sec.idx] ?? 0) + 1
                s.partCancels[sec.idx] = n
                NSLog("[sve] live part %ld: stopped after %.0f s (%@; try %ld of %ld)%@", sec.idx + 1, secs,
                      inFront.isSet ? "lag \(lag) pieces, or the phone is warm" : "the app left the front", n,
                      LiveParts.maxCancels, kept.isEmpty ? "" : ", kept \(kept)")
            } else {
                if out.partsDue(lang: lang, skip: skip)?.section.idx == sec.idx { s.partsLeft.insert(sec.idx) }
                NSLog("[sve] live part %ld: kept (lag %ld pieces): %@ in %.0f s", sec.idx + 1, lag,
                      kept.isEmpty ? "nothing" : kept, secs)
            }
        } catch {
            // A GPU refused (the app left the front) or a failed load: tried again only while tries are left.
            // Stop or Cancel is not counted.
            if !finishing.isSet && !abort.isSet { s.partCancels[sec.idx] = (s.partCancels[sec.idx] ?? 0) + 1 }
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
