import SwiftUI
import UserNotifications

@main
struct SummaverickEverydayApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var settings = AppSettings.shared
    @State private var lock = AppLock.shared
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Must be set before launch finishes so a tap that cold-starts the app is delivered.
        UNUserNotificationCenter.current().delegate = ReminderScheduler.shared
        AudioRecorder.sweepAskVoice()
        // Live Activities left by a closed app go before this launch can start its own.
        RecordingActivities.endStale()
        // Nothing records at launch: a record control left on by a closed app shows off again.
        RecordControlState.recordingChanged(false)
        // Lets notes keep being written with the phone locked after Stop (iOS 26+).
        ContinuedWork.register()
        // Finishes long recordings overnight while charging: a fixed identifier, registered before launch ends.
        OvernightWork.register()
        // Reconnect to (or start) the AI download as early as possible.
        ModelManager.shared.activate()
    }

    var body: some Scene {
        WindowGroup {
            ZStack {
                RootView()
                    .environment(settings)
                if lock.isLocked {
                    LockScreen()
                        .transition(.opacity)
                }
            }
            .task {
                await Retention.applyPolicy(maxAgeDays: settings.retentionDays)
            }
            .task { await ReminderScheduler.shared.resync() }
            .task { await PiecesBackfill.runOnce() }   // older notes become searchable by Ask (one time)
            .task {
                // A recording the app was closed in the middle of becomes a note first, so it is queued with the rest.
                await ProcessingPipeline.shared.recoverUnfinishedRecordings()
                await ProcessingPipeline.shared.processWaitingNotes()
            }
            .task { await ProcessingPipeline.shared.sweepOrphanSidecars() }
            .task { await ProcessingPipeline.shared.sweepCompression() }   // finished notes: WAV → AAC; stray .part files go
            // The AI finished downloading: write the notes for recordings made while it downloaded.
            .onReceive(NotificationCenter.default.publisher(for: .aiReady)) { _ in
                Task { await ProcessingPipeline.shared.processWaitingNotes() }
            }
            .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in
                Task { await ReminderScheduler.shared.resync() }
            }
            .onChange(of: scenePhase) { _, phase in
                switch phase {
                case .background:
                    lock.lockIfNeeded()
                    ProcessingPipeline.shared.appWentToBackground()   // unfinished notes can finish while charging
                case .active:
                    lock.unlockIfNeeded()
                    Task { await ReminderScheduler.shared.resync() }
                    ModelManager.shared.startIfNeeded()   // space freed, or a retry left for the next launch
                    Task { await ProcessingPipeline.shared.processWaitingNotes() }
                default: break
                }
            }
        }
    }
}

/// User preferences. Small and boring on purpose.
@MainActor
@Observable
final class AppSettings {
    static let shared = AppSettings()
    private let d = UserDefaults.standard

    var retentionDays: Int { didSet { d.set(retentionDays, forKey: "retentionDays") } }
    var requireUnlock: Bool { didSet { d.set(requireUnlock, forKey: "requireUnlock") } }
    var consentAcknowledged: Bool { didSet { d.set(consentAcknowledged, forKey: "consentAck") } }
    /// Language the summary, notes and questions are written in (the model hears any spoken language).
    var notesLanguage: String { didSet { d.set(notesLanguage, forKey: "notesLang") } }

    private init() {
        retentionDays = d.object(forKey: "retentionDays") as? Int ?? 7
        requireUnlock = d.bool(forKey: "requireUnlock")
        consentAcknowledged = d.bool(forKey: "consentAck")
        notesLanguage = d.string(forKey: "notesLang") ?? "en"
    }
}

/// Deletes raw audio older than the retention window. Transcripts and notes stay.
enum Retention {
    static func applyPolicy(maxAgeDays: Int) async {
        guard maxAgeDays > 0 else { return }
        let engine = CoreEngine.shared
        for row in await engine.audioOlderThan(days: Int32(maxAgeDays)) {
            if let p = AudioRecorder.existingAudioPath(row.audio_path) { AudioRecorder.removeAudio(atPath: p) }
            await engine.clearAudio(row.id)
        }
    }
}
