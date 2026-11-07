// Writes the transcript while the recording is still going, on the CPU, so it keeps going with the
// phone locked (the recording's audio session keeps the app running; iOS refuses background GPU work).
// After Stop it finishes the piece in flight and hands over to the pipeline, which continues from the
// same cursor (TranscriptSidecar) with is_final. Not used for Ask's voice questions.
import Foundation
import Observation
import UIKit

/// A flag set on the main thread and read from the core's progress callback (any thread).
final class AtomicFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
    func set(_ v: Bool) { lock.lock(); value = v; lock.unlock() }
}

/// The one transcription loop: from the sidecar's cursor, piece by piece, saving after every piece.
@MainActor
enum TranscriptRun {
    enum Stop: Equatable {
        /// Final: everything is transcribed.
        case end
        /// Still recording and not enough new audio yet.
        case wait
        /// `shouldContinue` said no between two pieces.
        case yielded
    }

    /// shouldStop is asked during a model run (any thread) and cancels it: the run throws and the piece is
    /// lost, the cursor stays. shouldContinue is asked between pieces. onPiece gets the progress after each piece.
    static func run(audioPath: String, isFinal: Bool,
                    shouldStop: @escaping () -> Bool,
                    shouldContinue: () -> Bool = { true },
                    onPiece: (TranscriptSidecar) -> Void = { _ in }) async throws -> (TranscriptSidecar, Stop) {
        var progress = TranscriptSidecar.load(audioPath: audioPath)
        while true {
            if shouldStop() { throw CancellationError() }
            try await ModelManager.shared.loadIntoCore(for: .transcription)
            let next = try await CoreEngine.shared.transcribeNextPiece(path: audioPath, startSample: progress.cursor_sample,
                                                                       isFinal: isFinal, previousText: progress.previousText,
                                                                       shouldStop: shouldStop)
            switch next {
            case .end: return (progress, .end)
            case .wait: return (progress, .wait)
            case .piece(let startSec, let endSample, let text):
                progress.append(startSec: startSec, endSample: endSample, text: text)
                progress.save(audioPath: audioPath)   // skipped when the audio was deleted meanwhile
                onPiece(progress)
                if !shouldContinue() { return (progress, .yielded) }
            }
        }
    }

    /// The pieces as the note stores them.
    static func pieces(_ s: TranscriptSidecar) -> [TranscriptPiece] {
        s.pieces.map { TranscriptPiece(startSec: $0.start_sec, text: $0.text) }
    }
}

@MainActor
@Observable
final class LiveTranscriber {
    static let shared = LiveTranscriber()

    enum Status: Equatable {
        case idle
        /// The AI is not ready (or gave up): notes are written after Stop.
        case later
        case writing
        case behind(minutes: Int)
        case pausedHeat
        case pausedBattery
    }

    /// A recording for notes is going (not an Ask voice question). Model runs use the CPU meanwhile.
    private(set) var isRecording = false
    private(set) var status: Status = .idle
    /// Transcript so far, one entry per piece, oldest first.
    private(set) var lines: [String] = []

    @ObservationIgnored private var task: Task<Void, Never>?
    @ObservationIgnored private var finishing = false
    @ObservationIgnored private let abort = AtomicFlag()
    @ObservationIgnored private var elapsed: () -> TimeInterval = { 0 }
    @ObservationIgnored private var transcribedSeconds: Double = 0

    private init() {}

    var statusLine: String {
        switch status {
        case .idle, .writing: return String(localized: "Writing as you talk")
        case .behind(let m): return String(localized: "About \(m) min behind")
        case .pausedHeat: return String(localized: "Paused while the phone cools down")
        case .pausedBattery: return String(localized: "Paused: battery low — notes will be written later")
        case .later: return String(localized: "Notes will be written after you stop")
        }
    }

    /// A recording started (its WAV is being written at `audio`). Transcribes along if the AI is ready.
    func recordingStarted(audio: URL, elapsed: @escaping () -> TimeInterval) {
        task?.cancel()
        isRecording = true
        finishing = false
        abort.set(false)
        lines = []
        transcribedSeconds = 0
        self.elapsed = elapsed
        // isReady also means the phone is supported.
        guard ModelManager.shared.isReady else { status = .later; return }
        status = .writing
        UIDevice.current.isBatteryMonitoringEnabled = true
        let path = audio.path
        task = Task { await self.loop(path) }
    }

    /// Stop: the piece in flight is finished and saved; returns once the loop has ended, so the pipeline
    /// can take over from the cursor.
    func finish() async {
        isRecording = false
        finishing = true
        await task?.value
        task = nil
        status = .idle
        UIDevice.current.isBatteryMonitoringEnabled = false
    }

    /// Cancel: the piece in flight is dropped so deleting the recording is not delayed.
    func cancel() async {
        abort.set(true)
        await finish()
    }

    private func pauseReason() -> LivePause? {
        let d = UIDevice.current
        return LivePause.reason(thermal: ProcessInfo.processInfo.thermalState, batteryLevel: d.batteryLevel,
                                charging: d.batteryState == .charging || d.batteryState == .full)
    }

    private func refresh() {
        if let m = LiveLag.minutesBehind(recordedSeconds: elapsed(), transcribedSeconds: transcribedSeconds) {
            status = .behind(minutes: m)
        } else {
            status = .writing
        }
    }

    private func loop(_ path: String) async {
        var failures = 0
        let abort = self.abort
        while !finishing {
            if let p = pauseReason() {
                status = p == .heat ? .pausedHeat : .pausedBattery
                await nap(3)
                continue
            }
            refresh()
            do {
                let (_, stop) = try await TranscriptRun.run(
                    audioPath: path, isFinal: false,
                    shouldStop: { abort.isSet || Heat.isTooHot(ProcessInfo.processInfo.thermalState) },
                    shouldContinue: { !self.finishing && self.pauseReason() == nil },
                    onPiece: { s in
                        self.lines = s.pieces.map(\.text)
                        self.transcribedSeconds = s.transcribedSeconds
                        self.refresh()
                    })
                failures = 0
                if stop == .wait { await nap(3) }
            } catch {
                if abort.isSet || finishing { break }
                if Heat.isTooHot(ProcessInfo.processInfo.thermalState) { continue }   // paused at the top
                failures += 1
                NSLog("[sve] live transcription failed (%ld): %@", failures, String(describing: error))
                if failures >= 3 { status = .later; return }   // the pipeline does it after Stop
                await nap(3)
            }
        }
    }

    /// Sleeps up to `seconds`, waking early on Stop or Cancel.
    private func nap(_ seconds: Double) async {
        let end = Date().addingTimeInterval(seconds)
        while !finishing, Date() < end {
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
    }
}
