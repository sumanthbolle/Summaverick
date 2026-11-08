// The one recording for notes, without any screen: RecordView shows it, and shortcuts (R3) start and stop it.
// Start checks consent and the microphone, records, starts live transcription and the Live Activity. Stop hands
// the audio to ProcessingPipeline.recordingStopped synchronously, exactly as the Stop button always did. Cancel
// drops the piece in flight and deletes the audio. A recording that was going when the app was closed is turned into
// a note at the next launch (UnfinishedRecordings, ProcessingPipeline.recoverUnfinishedRecordings).
import ActivityKit
import Foundation
import Observation

@MainActor
@Observable
final class RecordingController {
    static let shared = RecordingController()

    enum StartResult: Equatable {
        case started
        /// A recording is already going (or starting): nothing new starts.
        case alreadyRecording
        /// The recording reminder has not been acknowledged: the app must show it first.
        case needsConsent
        case micDenied
        /// The audio session or the recorder refused (the reason is in `lastError`).
        case failed
    }

    /// The recorder itself (level and error, for the record screen).
    let recorder = AudioRecorder()
    var isRecording: Bool { recorder.isRecording }
    var elapsed: TimeInterval { recorder.elapsed }
    /// Between start() and the recorder running (the microphone question): a second start is a no-op.
    private(set) var isStarting = false

    private init() {}

    func start() async -> StartResult {
        guard !recorder.isRecording, !isStarting else { return .alreadyRecording }
        guard AppSettings.shared.consentAcknowledged else { return .needsConsent }
        isStarting = true
        defer { isStarting = false }
        recorder.lastError = nil
        guard await recorder.requestPermission() else { return .micDenied }
        guard !recorder.isRecording else { return .alreadyRecording }
        // The record screen closed while the microphone question was up: nothing records without it.
        guard !Task.isCancelled else { return .failed }
        recorder.start()
        guard recorder.isRecording, let url = recorder.currentURL else { return .failed }
        UnfinishedRecordings.add(url.path)
        let recorder = self.recorder
        LiveTranscriber.shared.recordingStarted(audio: url) { recorder.elapsed }
        RecordingActivities.shared.recordingStarted(audioPath: url.path, startedAt: Date().addingTimeInterval(-recorder.elapsed))
        NSLog("[sve] recording started")
        return .started
    }

    /// Stop and make notes. Synchronous, before anything awaits: claims the work and the background time right in
    /// the tap (or the shortcut). False when nothing was recording.
    @discardableResult
    func stop() -> Bool {
        guard let (url, duration) = recorder.stop() else { return false }
        ProcessingPipeline.shared.recordingStopped(audio: url, duration: duration)
        RecordingActivities.shared.update(audioPath: url.path, .transcribing(0),
                                          line: ProcessingPipeline.Stage.transcribing(0).label, percent: 0)
        NSLog("[sve] recording stopped (%lld s)", duration)
        return true
    }

    /// Cancel: the recording and its transcription so far are deleted.
    func cancel() {
        guard let (url, _) = recorder.stop() else { return }
        RecordingActivities.shared.update(audioPath: url.path, .cancelled, line: "", percent: 0)
        UnfinishedRecordings.remove(url.path)
        // Drop the piece in flight first, so nothing is written next to the deleted recording.
        Task {
            await LiveTranscriber.shared.cancel(audioPath: url.path)
            AudioRecorder.removeAudio(atPath: url.path)
        }
        NSLog("[sve] recording cancelled")
    }

    /// The recording going now, if this is its file (the launch recovery leaves it alone).
    func isRecording(audioPath: String) -> Bool {
        recorder.isRecording && recorder.currentURL?.path == audioPath
    }
}

/// Recordings started whose note does not exist yet, kept across launches: added at start, removed once the note is
/// saved (or the recording is cancelled). One left at launch was going when the app was closed.
enum UnfinishedRecordings {
    private static let key = "recording.unfinished"

    static var paths: [String] { UserDefaults.standard.stringArray(forKey: key) ?? [] }

    /// File names (the container folder can change between launches).
    static var names: Set<String> { Set(paths.map { ($0 as NSString).lastPathComponent }) }

    static func add(_ path: String) {
        var p = paths
        guard !p.contains(path) else { return }
        p.append(path)
        UserDefaults.standard.set(p, forKey: key)
    }

    static func remove(_ path: String) {
        let name = (path as NSString).lastPathComponent
        let p = paths.filter { ($0 as NSString).lastPathComponent != name }
        UserDefaults.standard.set(p, forKey: key)
    }

    /// Length of a 16 kHz mono 16-bit WAV from its size, so a file the recorder never finished (its header still
    /// says 0 bytes) is measured too.
    static func seconds(ofWAV path: String) -> Int64 {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? Int64 else { return 0 }
        return max(0, size - dataOffset(path)) / 32_000
    }

    /// Where the samples start: after the "data" chunk header (AVAudioRecorder writes a padding chunk first).
    private static func dataOffset(_ path: String) -> Int64 {
        guard let h = FileHandle(forReadingAtPath: path) else { return 44 }
        defer { try? h.close() }
        let head = [UInt8](h.readData(ofLength: 8192))
        var i = 12   // "RIFF" size "WAVE"
        while i + 8 <= head.count {
            let id = String(bytes: head[i..<i + 4], encoding: .ascii)
            let len = Int(head[i + 4]) | Int(head[i + 5]) << 8 | Int(head[i + 6]) << 16 | Int(head[i + 7]) << 24
            if id == "data" { return Int64(i + 8) }
            i += 8 + len + (len & 1)
        }
        return 44
    }
}

/// The recordings' Live Activities (iOS 18+, where the widget extension runs): one per recording, from its start
/// until its note's notes are ready, paused or failed, or it is cancelled or deleted. Progress is sent on every stage
/// change and otherwise about every 5 s (LiveActivityLogic). Every request goes through one chain per activity, so
/// they reach the system in order.
@MainActor
final class RecordingActivities {
    static let shared = RecordingActivities()

    private final class Tracked {
        let activity: Activity<RecordingActivity>
        let audioPath: String
        var noteID: Int64?
        var state: RecordingActivity.ContentState
        var lastStage: LiveActivityLogic.Stage?
        var lastSentAt: Date?
        var chain: Task<Void, Never>?
        init(_ activity: Activity<RecordingActivity>, audioPath: String, state: RecordingActivity.ContentState) {
            self.activity = activity
            self.audioPath = audioPath
            self.state = state
        }
    }
    private var tracked: [Tracked] = []

    private init() {}

    /// At launch, before anything records: activities left by an app that was closed (a recording or a note's work
    /// that is not going any more) go at once. The note itself carries on as usual. Called from App init, so the list
    /// is read before this launch can start one of its own.
    static func endStale() {
        guard #available(iOS 18, *) else { return }
        let stale = Activity<RecordingActivity>.activities
        guard !stale.isEmpty else { return }
        NSLog("[sve] live activity: ending %ld left from before", stale.count)
        Task {
            for a in stale { await a.end(nil, dismissalPolicy: .immediate) }
        }
    }

    func recordingStarted(audioPath: String, startedAt: Date) {
        guard #available(iOS 18, *), ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        let state = RecordingActivity.ContentState(phase: .recording, startedAt: startedAt, stageLine: "", percent: 0, noteID: nil)
        do {
            let a = try Activity.request(attributes: RecordingActivity(), content: .init(state: state, staleDate: nil), pushType: nil)
            let t = Tracked(a, audioPath: audioPath, state: state)
            t.lastStage = .recording
            t.lastSentAt = Date()
            tracked.append(t)
            NSLog("[sve] live activity: started")
        } catch {
            NSLog("[sve] live activity: not started: %@", String(describing: error))
        }
    }

    /// The note of this recording exists now: its stages are followed from here.
    func attach(audioPath: String, noteID: Int64) {
        guard let t = tracked.first(where: { $0.audioPath == audioPath }) else { return }
        t.noteID = noteID
        t.state.noteID = noteID
    }

    func isFollowing(noteID: Int64) -> Bool { tracked.contains { $0.noteID == noteID } }

    func update(audioPath: String, _ stage: LiveActivityLogic.Stage, line: String, percent: Int) {
        guard let t = tracked.first(where: { $0.audioPath == audioPath }) else { return }
        apply(t, stage, line: line, percent: percent)
    }

    func update(noteID: Int64, _ stage: LiveActivityLogic.Stage, line: String, percent: Int) {
        guard let t = tracked.first(where: { $0.noteID == noteID }) else { return }
        apply(t, stage, line: line, percent: percent)
    }

    private func apply(_ t: Tracked, _ stage: LiveActivityLogic.Stage, line: String, percent: Int) {
        let now = Date()
        t.state = LiveActivityLogic.state(for: stage, line: line, percent: percent, after: t.state)
        if let ending = LiveActivityLogic.ending(for: stage) {
            tracked.removeAll { $0 === t }
            end(t, ending, stage: stage)
            return
        }
        guard LiveActivityLogic.updateDue(last: t.lastStage, lastSentAt: t.lastSentAt, next: stage, now: now) else { return }
        t.lastStage = stage
        t.lastSentAt = now
        let content = ActivityContent(state: t.state, staleDate: nil)
        let a = t.activity, previous = t.chain
        t.chain = Task {
            await previous?.value
            await a.update(content)
        }
    }

    /// Ends it once (it is no longer tracked). Background time is taken now, so the end reaches the system even
    /// when the app is about to be suspended (a cut-off).
    private func end(_ t: Tracked, _ ending: LiveActivityLogic.Ending, stage: LiveActivityLogic.Stage) {
        let policy: ActivityUIDismissalPolicy
        switch ending {
        case .immediately: policy = .immediate
        case .standard: policy = .default
        case .after(let s): policy = .after(Date().addingTimeInterval(s))
        }
        NSLog("[sve] live activity: ended (%@)", String(describing: stage))
        let hold = NotesNotifier.Hold()
        let content = ActivityContent(state: t.state, staleDate: nil)
        let a = t.activity, previous = t.chain
        t.chain = Task {
            await previous?.value
            await a.end(content, dismissalPolicy: policy)
            hold.end()
        }
    }
}
