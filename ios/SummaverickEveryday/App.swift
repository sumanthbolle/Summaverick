import SwiftUI
import UserNotifications

@main
struct SummaverickEverydayApp: App {
    @State private var settings = AppSettings.shared
    @State private var lock = AppLock.shared
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Must be set before launch finishes so a tap that cold-starts the app is delivered.
        UNUserNotificationCenter.current().delegate = ReminderScheduler.shared
        AudioRecorder.sweepAskVoice()
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
            .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in
                Task { await ReminderScheduler.shared.resync() }
            }
            .onChange(of: scenePhase) { _, phase in
                switch phase {
                case .background: lock.lockIfNeeded()
                case .active:
                    lock.unlockIfNeeded()
                    Task { await ReminderScheduler.shared.resync() }
                    Task { await ProcessingPipeline.shared.resumeInterrupted() }
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

    var languageID: String { didSet { d.set(languageID, forKey: "lang") } }
    var retentionDays: Int { didSet { d.set(retentionDays, forKey: "retentionDays") } }
    var requireUnlock: Bool { didSet { d.set(requireUnlock, forKey: "requireUnlock") } }
    var consentAcknowledged: Bool { didSet { d.set(consentAcknowledged, forKey: "consentAck") } }
    /// Language the summary, notes and questions are written in when an audio-capable model is used.
    var notesLanguage: String { didSet { d.set(notesLanguage, forKey: "notesLang") } }

    private init() {
        languageID = d.string(forKey: "lang") ?? ""
        retentionDays = d.object(forKey: "retentionDays") as? Int ?? 7
        requireUnlock = d.bool(forKey: "requireUnlock")
        consentAcknowledged = d.bool(forKey: "consentAck")
        notesLanguage = d.string(forKey: "notesLang") ?? "en"
    }

    /// Resolves the chosen language against what this device can do on-device.
    var language: SpeechLanguage {
        let langs = SpeechTranscriber.onDeviceLanguages()
        if let l = langs.first(where: { $0.id == languageID }) { return l }
        if let l = langs.first { return l }
        return SpeechLanguage(locale: Locale(identifier: "en-US"))
    }
}

/// Deletes raw audio older than the retention window. Transcripts and notes stay.
enum Retention {
    static func applyPolicy(maxAgeDays: Int) async {
        guard maxAgeDays > 0 else { return }
        let engine = CoreEngine.shared
        for row in await engine.audioOlderThan(days: Int32(maxAgeDays)) {
            if let p = AudioRecorder.existingAudioPath(row.audio_path) { try? FileManager.default.removeItem(atPath: p) }
            await engine.clearAudio(row.id)
        }
    }
}
