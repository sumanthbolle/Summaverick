// The one recording for notes, without any screen: RecordView shows it, and shortcuts (R3) start and stop it.
// Shortcuts, Siri, the Action button and the record control reach it through RecordingIntents.swift.
// Start checks consent and the microphone, records, starts live transcription and the Live Activity. Stop hands
// the audio to ProcessingPipeline.recordingStopped synchronously, exactly as the Stop button always did. Cancel
// drops the piece in flight and deletes the audio. A recording that was going when the app was closed is turned into
// a note at the next launch (UnfinishedRecordings, ProcessingPipeline.recoverUnfinishedRecordings).
// Stop when quiet (ruling B11): every recording, however it was started, is watched for voice through the recorder's
// level (QuietStop). 30 s before the quiet limit it warns once (the Live Activity, and a notification with Keep recording,
// or RecordView's line while that screen is in front); voice or Keep recording takes the warning away. At the limit it
// goes through stop() like the Stop button, after cutting the trailing quiet (2 s after the last voice are kept; with no
// voice ever heard nothing is cut, ruling B12).
import ActivityKit
import Foundation
import Observation
import UIKit

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
        /// A stop (or cancel) came while it was starting (the microphone question): nothing records.
        case stoppedWhileStarting
    }

    /// The recorder itself (level and error, for the record screen).
    let recorder = AudioRecorder()
    var isRecording: Bool { recorder.isRecording }
    var elapsed: TimeInterval { recorder.elapsed }
    /// Between start() and the recorder running (the microphone question): a second start is a no-op.
    private(set) var isStarting = false
    /// Stop or cancel came while starting: the start ends without recording (RecordingIntentLogic.startMayRecord).
    private var stopRequestedWhileStarting = false
    /// A shortcut asked to record before the recording reminder was acknowledged: the app shows it when it opens
    /// (RootView), shortly after only (RecordingIntentLogic.consentRequestWindow).
    private(set) var consentRequestedAt: Date?
    /// Stop when quiet: the warning is up (RecordView shows its line with Keep recording).
    private(set) var quietWarning = false
    /// RecordView is on screen: with the app in front, its line takes the place of the warning notification.
    var recordScreenShown = false
    /// This recording's voice detector (the limit is the setting when it started).
    @ObservationIgnored private var quiet = QuietStop.Detector(minutes: 0)

    private init() {}

    func requestConsentFromShortcut() {
        consentRequestedAt = Date()
        NSLog("[sve] intent: recording reminder not acknowledged yet; opening the app")
    }

    /// True once for a fresh request (it is cleared either way).
    func takeConsentRequest(now: Date = Date()) -> Bool {
        defer { consentRequestedAt = nil }
        return RecordingIntentLogic.consentRequestIsFresh(requestedAt: consentRequestedAt, now: now)
    }

    func start() async -> StartResult {
        guard !recorder.isRecording, !isStarting else { return .alreadyRecording }
        guard AppSettings.shared.consentAcknowledged else { return .needsConsent }
        isStarting = true
        stopRequestedWhileStarting = false
        defer { isStarting = false; stopRequestedWhileStarting = false }
        recorder.lastError = nil
        guard await recorder.requestPermission() else { return .micDenied }
        guard !recorder.isRecording else { return .alreadyRecording }
        // The record screen closed while the microphone question was up: nothing records without it.
        guard !Task.isCancelled else { return .failed }
        guard RecordingIntentLogic.startMayRecord(stopRequested: stopRequestedWhileStarting) else {
            NSLog("[sve] recording not started: stopped while starting")
            return .stoppedWhileStarting
        }
        recorder.start()
        guard recorder.isRecording, let url = recorder.currentURL else { return .failed }
        UnfinishedRecordings.add(url.path)
        watchForQuiet()
        let recorder = self.recorder
        LiveTranscriber.shared.recordingStarted(audio: url) { recorder.elapsed }
        RecordingActivities.shared.recordingStarted(audioPath: url.path, startedAt: Date().addingTimeInterval(-recorder.elapsed))
        RecordControlState.recordingChanged(true)
        NSLog("[sve] recording started")
        return .started
    }

    /// Stop and make notes. Synchronous, before anything awaits: claims the work and the background time right in
    /// the tap (or the shortcut). False when nothing was recording.
    @discardableResult
    func stop() -> Bool { stop(quietStop: nil) }

    /// quietStop: stopped by Stop when quiet; the audio after keepUntil (seconds) is cut before the notes are made
    /// (nil: no voice was ever heard, nothing is cut), and line says why it stopped (the Live Activity, and a
    /// notification when the app is not in front).
    @discardableResult
    private func stop(quietStop: (keepUntil: TimeInterval?, line: String)?) -> Bool {
        if isStarting { stopRequestedWhileStarting = true }
        guard let (url, recorded) = recorder.stop() else { return false }
        recorder.onLevel = nil
        var duration = recorded
        if let q = quietStop {
            if let keep = q.keepUntil, let trimmed = Self.trimWAV(atPath: url.path, keepSeconds: keep) {
                NSLog("[sve] quiet: kept %lld of %lld s", trimmed, recorded)
                duration = trimmed
            }
            // Sent with the stage update below, and kept through the note's stages.
            RecordingActivities.shared.setQuietLine(audioPath: url.path, q.line, send: false)
            if showsWarningsOutside { QuietNotifier.stopped(q.line) } else { QuietNotifier.withdraw() }
        } else if quietWarning {
            RecordingActivities.shared.setQuietLine(audioPath: url.path, nil, send: false)
            QuietNotifier.withdraw()
        }
        quietWarning = false
        RecordControlState.recordingChanged(false)
        ProcessingPipeline.shared.recordingStopped(audio: url, duration: duration)
        RecordingActivities.shared.update(audioPath: url.path, .transcribing(0),
                                          line: ProcessingPipeline.Stage.transcribing(0).label, percent: 0)
        NSLog("[sve] recording stopped (%lld s)", duration)
        return true
    }

    /// Cancel: the recording and its transcription so far are deleted.
    func cancel() {
        if isStarting { stopRequestedWhileStarting = true }
        guard let (url, _) = recorder.stop() else { return }
        recorder.onLevel = nil
        if quietWarning { QuietNotifier.withdraw() }
        quietWarning = false
        RecordControlState.recordingChanged(false)
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

    // MARK: Stop when quiet

    /// "Keep recording" (RecordView's line or the notification's action): the warning goes and the quiet timer
    /// starts again.
    func keepRecording() {
        guard recorder.isRecording else { return }
        quiet.keepRecording(at: recorder.recordedSeconds)
        NSLog("[sve] quiet: keep recording")
        clearQuietWarning()
    }

    private func watchForQuiet() {
        let minutes = AppSettings.shared.quietStopMinutes
        quiet = QuietStop.Detector(minutes: minutes)
        quietWarning = false
        if minutes > 0 {
            recorder.onLevel = { [weak self] t, db in self?.heard(t, db) }
        } else {
            recorder.onLevel = nil
        }
    }

    private func heard(_ t: TimeInterval, _ db: Float) {
        switch quiet.feed(time: t, level: Double(db)) {
        case .none: break
        case .warn:
            let line = String(localized: "No voice for a while — stopping in 30 seconds")
            NSLog("[sve] quiet: warning at %.0f s", t)
            quietWarning = true
            if let url = recorder.currentURL { RecordingActivities.shared.setQuietLine(audioPath: url.path, line) }
            if showsWarningsOutside { QuietNotifier.warn(line) }
        case .warningCancelled:
            NSLog("[sve] quiet: voice again at %.0f s", t)
            clearQuietWarning()
        case .stop(let keepUntil):
            let minutes = Int((quiet.limit / 60).rounded())
            NSLog("[sve] quiet: stopping at %.0f s, keeping %.0f s", t, keepUntil ?? -1)
            stop(quietStop: (keepUntil, String(localized: "Stopped after \(minutes) min of quiet")))
        }
    }

    private func clearQuietWarning() {
        guard quietWarning else { return }
        quietWarning = false
        if let url = recorder.currentURL { RecordingActivities.shared.setQuietLine(audioPath: url.path, nil) }
        QuietNotifier.withdraw()
    }

    /// The app is not in front with RecordView showing: the warning and the stop are told by notification.
    private var showsWarningsOutside: Bool {
        !(UIApplication.shared.applicationState == .active && recordScreenShown)
    }

    /// Cuts a finished recording after keepSeconds of audio and writes its header to match (QuietStop.trim). The live
    /// transcriber reads each piece into memory at its start, and a short read is planned with what was read, so a
    /// piece in flight is not harmed. Returns the new length in seconds; nil when nothing was cut (not a WAV the
    /// repair understands, or no audio after that point).
    private static func trimWAV(atPath path: String, keepSeconds: TimeInterval) -> Int64? {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? Int64,
              let h = FileHandle(forUpdatingAtPath: path) else { return nil }
        defer { try? h.close() }
        let head = [UInt8](h.readData(ofLength: 8192))
        guard let t = QuietStop.trim(head: head, fileSize: size, keepSeconds: keepSeconds), t.fileSize < size else { return nil }
        do {
            try h.truncate(atOffset: UInt64(t.fileSize))
            for p in t.patches {
                try h.seek(toOffset: p.offset)
                var v = p.value.littleEndian
                try h.write(contentsOf: Data(bytes: &v, count: 4))
            }
            try h.synchronize()
        } catch {
            // The core still reads it (it trusts the file's size over a larger size field).
            NSLog("[sve] quiet: recording not cut: %@", String(describing: error))
            return nil
        }
        return Int64((Double(t.dataBytes) / 32_000).rounded())
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

    /// A WAV the recorder never finished still says 0 (or an old size) in its header: the RIFF and data sizes are
    /// written to match the file (WAVHeaderRepair), so it plays, compresses and keeps its tail. Returns its length in
    /// seconds (16 kHz mono 16-bit), or nil when it is not a WAV the repair understands (left as it is).
    static func finishWAV(atPath path: String) -> Int64? {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? Int64,
              let h = FileHandle(forUpdatingAtPath: path) else { return nil }
        defer { try? h.close() }
        let head = [UInt8](h.readData(ofLength: 8192))
        guard let r = WAVHeaderRepair.repair(head: head, fileSize: size) else { return nil }
        do {
            for p in r.patches {
                try h.seek(toOffset: p.offset)
                var v = p.value.littleEndian
                try h.write(contentsOf: Data(bytes: &v, count: 4))
            }
            if !r.patches.isEmpty { try h.synchronize() }
        } catch {
            NSLog("[sve] unfinished recording: header not written: %@", String(describing: error))
        }
        return r.dataBytes / 32_000
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
        // Only those still showing as live: an ended "Notes ready" card keeps its own dismissal time.
        let stale = Activity<RecordingActivity>.activities.filter { $0.activityState == .active || $0.activityState == .stale }
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

    /// Stop when quiet: the warning while recording, or why it stopped (nil: none). send: false leaves it for the next
    /// stage update, which carries it.
    func setQuietLine(audioPath: String, _ line: String?, send: Bool = true) {
        guard let t = tracked.first(where: { $0.audioPath == audioPath }), t.state.quietLine != line else { return }
        t.state.quietLine = line
        guard send else { return }
        t.lastSentAt = Date()
        let content = ActivityContent(state: t.state, staleDate: nil)
        let a = t.activity, previous = t.chain
        t.chain = Task {
            await previous?.value
            await a.update(content)
        }
    }

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
