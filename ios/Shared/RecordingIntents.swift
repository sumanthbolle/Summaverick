// Start and stop a recording without opening the app (iOS 18+): from Shortcuts and their automations, Siri, the
// Action button (an App Shortcut is offered there by itself), the Live Activity's Stop button and the Control Center /
// Lock Screen record control.
//
// Compiled into both targets, because the widget extension's buttons and control name these intents. They are
// performed in the app's process, never in the extension: the system launches the app (without opening it) for a
// LiveActivityIntent / AudioRecordingIntent, and from iOS 27 `allowedExecutionTargets` says so explicitly. The
// extension's copy is built with SVE_WIDGET_EXTENSION and its perform never records; it only logs.
// The answers (what is said, when the app opens instead) are the pure RecordingIntentLogic.
import AppIntents
import Foundation
import WidgetKit
#if !SVE_WIDGET_EXTENSION
import UIKit
import UserNotifications
#endif

/// The record control's state, read by the widget extension and written by the app (App Group defaults), and the
/// control's kind.
enum RecordControlState {
    static let kind = "com.summaverick.everyday.record"
    static let appGroup = "group.com.summaverick.everyday"
    private static let key = "recording.isRecording"
    private static var defaults: UserDefaults? { UserDefaults(suiteName: appGroup) }

    static var isRecording: Bool { defaults?.bool(forKey: key) ?? false }

    /// From the app, whenever a recording starts or ends (and at launch, when nothing records yet).
    static func recordingChanged(_ on: Bool) {
        guard defaults?.object(forKey: key) as? Bool != on else { return }
        defaults?.set(on, forKey: key)
        if #available(iOS 18.0, *) { ControlCenter.shared.reloadControls(ofKind: kind) }
    }
}

@available(iOS 18.0, *)
struct StartRecordingIntent: AudioRecordingIntent, LiveActivityIntent {
    static let title: LocalizedStringResource = "Start recording"
    static let description = IntentDescription("Starts recording for notes, without opening the app.")

    @available(iOS 26.0, *)
    static var supportedModes: IntentModes { [.background, .foreground(.dynamic)] }

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        #if SVE_WIDGET_EXTENSION
        NSLog("[sve] intent: start reached the widget extension; nothing recorded")
        return .result(dialog: IntentDialog(RecordingIntentLogic.text(.couldNotStart)))
        #else
        switch await RecordingIntentActions.start(from: "shortcut") {
        case .say(let line): return .result(dialog: IntentDialog(RecordingIntentLogic.text(line)))
        case .openForConsent: throw openAppForConsent()
        }
        #endif
    }
}

@available(iOS 18.0, *)
struct StopRecordingIntent: LiveActivityIntent {
    static let title: LocalizedStringResource = "Stop recording"
    static let description = IntentDescription("Stops the recording; its notes are written on this phone.")

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        #if SVE_WIDGET_EXTENSION
        NSLog("[sve] intent: stop reached the widget extension; nothing stopped")
        return .result(dialog: IntentDialog(RecordingIntentLogic.text(.notRecording)))
        #else
        let stopped = RecordingIntentActions.stop(from: "shortcut or Live Activity")
        return .result(dialog: IntentDialog(RecordingIntentLogic.text(RecordingIntentLogic.reply(stopped: stopped))))
        #endif
    }
}

/// The Control Center / Lock Screen record control's action: on starts a recording, off stops it. Not offered in
/// Shortcuts (Start and Stop are).
@available(iOS 18.0, *)
struct RecordControlIntent: SetValueIntent, AudioRecordingIntent, LiveActivityIntent {
    static let title: LocalizedStringResource = "Record"
    static let isDiscoverable = false

    @available(iOS 26.0, *)
    static var supportedModes: IntentModes { [.background, .foreground(.dynamic)] }

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    @Parameter(title: "Recording")
    var value: Bool

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        #if SVE_WIDGET_EXTENSION
        NSLog("[sve] intent: record control reached the widget extension; nothing changed")
        #else
        switch RecordingIntentLogic.toggle(on: value) {
        case .start:
            if await RecordingIntentActions.start(from: "control") == .openForConsent { throw openAppForConsent() }
        case .stop:
            RecordingIntentActions.stop(from: "control")
        }
        #endif
        return .result()
    }
}

#if !SVE_WIDGET_EXTENSION
/// The app's side of the intents: the one recording controller, as the Record and Stop buttons use it.
@available(iOS 18.0, *)
@MainActor
enum RecordingIntentActions {
    static func start(from source: String) async -> RecordingIntentLogic.Reply {
        let result = await RecordingController.shared.start()
        NSLog("[sve] intent: start from %@ (app %@): %@", source, appState, String(describing: result))
        return RecordingIntentLogic.reply(to: result.intentStart)
    }

    /// The same synchronous stop as the Stop button: the notes' work (and its continued-processing request) is
    /// claimed here. From the background iOS may refuse that request; the pipeline then finishes the notes in the
    /// overnight task and says so (ProcessingPipeline.backgroundTimeEnded).
    @discardableResult
    static func stop(from source: String) -> Bool {
        let stopped = RecordingController.shared.stop()
        NSLog("[sve] bg: intent: stop from %@ (app %@): %@", source, appState, stopped ? "stopped" : "nothing recording")
        return stopped
    }

    private static var appState: String {
        switch UIApplication.shared.applicationState {
        case .active: return "in front"
        case .inactive: return "inactive"
        case .background: return "in the background"
        @unknown default: return "unknown"
        }
    }
}

extension RecordingController.StartResult {
    var intentStart: RecordingIntentLogic.Start {
        switch self {
        case .started: return .started
        case .alreadyRecording: return .alreadyRecording
        case .needsConsent: return .needsConsent
        case .micDenied: return .micDenied
        case .failed: return .failed
        case .stoppedWhileStarting: return .stoppedWhileStarting
        }
    }
}

// iOS 18–25 continue in the app through ForegroundContinuableIntent; iOS 26+ through supportedModes.
@available(iOS 18.0, *)
extension StartRecordingIntent: ForegroundContinuableIntent {}
@available(iOS 18.0, *)
extension RecordControlIntent: ForegroundContinuableIntent {}

@available(iOS 18.0, *)
extension ForegroundContinuableIntent {
    /// The recording reminder was never acknowledged: the app opens and shows it (RootView). Nothing records until
    /// the person taps "I understand".
    @MainActor
    func openAppForConsent() -> Error {
        RecordingController.shared.requestConsentFromShortcut()
        let dialog = IntentDialog(RecordingIntentLogic.text(.consentFirst))
        if #available(iOS 26.0, *) {
            return needsToContinueInForegroundError(dialog, alwaysConfirm: false)
        }
        return needsToContinueInForegroundError(dialog, continuation: nil)
    }
}

// MARK: - assistant shortcuts (app target only)
//
// What's on today, Last note summary and Add a reminder read or write the person's own reminders and notes without
// the app's Face ID screen, so each needs the phone unlocked (authenticationPolicy). Ask Summaverick opens the app
// (the app lock applies as usual): answering loads the AI model, which cannot run inside a background intent.
// What they say is the pure AssistantIntentLogic (RecordingIntentLogic.swift).

@available(iOS 18.0, *)
struct WhatsOnTodayIntent: AppIntent {
    static let title: LocalizedStringResource = "What's on today"
    static let description = IntentDescription("Tells you the reminders still to come today and your open to-dos.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ReturnsValue<String> & ProvidesDialog {
        let now = Date()
        let reminders: [Reminder]
        do {
            reminders = try await CoreEngine.shared.remindersStrict(activeOnly: true)
        } catch {
            NSLog("[sve] intent: today could not read reminders: %@", String(describing: error))
            let line = String(localized: "I couldn't read your reminders. Please try again.")
            return .result(value: line, dialog: IntentDialog(stringLiteral: line))
        }
        let tasks = await CoreEngine.shared.tasks(openOnly: true)
        // Today's times as Today lists them (ReminderOccurrence), only those still to come.
        let today = ReminderOccurrence.upcoming(reminders, now: now, calendar: .current, days: 1)
            .filter { $0.date >= now }
            .map { AssistantIntentLogic.TodayReminder(time: AssistantFormat.time($0.hhmm), text: $0.reminder.text) }
        let text = AssistantIntentLogic.todayText(reminders: today, todos: tasks.map(\.text))
        NSLog("[sve] intent: today: %d reminder times, %d to-dos", today.count, tasks.count)
        return .result(value: text, dialog: IntentDialog(stringLiteral: text))
    }
}

@available(iOS 18.0, *)
struct LastNoteSummaryIntent: AppIntent {
    static let title: LocalizedStringResource = "Last note summary"
    static let description = IntentDescription("Tells you the title and summary of your most recent note.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ReturnsValue<String> & ProvidesDialog {
        let list = await CoreEngine.shared.notes()
        guard let id = AssistantIntentLogic.newestNote(list.map { (id: $0.id, createdAt: $0.created_at) }),
              let note = await CoreEngine.shared.note(id) else {
            let line = AssistantIntentLogic.noNotesText()
            return .result(value: line, dialog: IntentDialog(stringLiteral: line))
        }
        // Writing now: as FinishNowView decides it (memories, after the notes, do not count).
        let pipeline = ProcessingPipeline.shared
        var writingNow = pipeline.activeNoteID == id && pipeline.isWorking
        if case .memories = pipeline.stage { writingNow = false }
        let state = AssistantIntentLogic.noteState(summary: note.short_summary, notes: note.detailed_notes,
                                                   questions: note.revision_questions, writingNow: writingNow)
        NSLog("[sve] intent: last note %lld: %@", id, String(describing: state))
        let text = AssistantIntentLogic.lastNoteText(title: note.title, summary: note.short_summary, state: state)
        return .result(value: text, dialog: IntentDialog(stringLiteral: text))
    }
}

/// How often a reminder from the shortcut rings (AssistantIntentLogic.RepeatRule).
@available(iOS 18.0, *)
enum ReminderRepeatOption: String, AppEnum {
    case once, daily, weekly
    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Repeat"
    static let caseDisplayRepresentations: [ReminderRepeatOption: DisplayRepresentation] = [
        .once: "Once", .daily: "Every day", .weekly: "Every week",
    ]
    var rule: AssistantIntentLogic.RepeatRule { AssistantIntentLogic.RepeatRule(rawValue: rawValue) ?? .once }
}

@available(iOS 18.0, *)
struct AddReminderIntent: AppIntent {
    static let title: LocalizedStringResource = "Add a reminder"
    static let description = IntentDescription("Saves a reminder in Summaverick, once, every day or every week. It rings at the time you choose.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    @Parameter(title: "Reminder", requestValueDialog: "What should I remind you about?")
    var text: String

    @Parameter(title: "Time", requestValueDialog: "When should I remind you?")
    var time: Date

    @Parameter(title: "Repeat", default: .once)
    var repeats: ReminderRepeatOption

    static var parameterSummary: some ParameterSummary {
        Summary("Remind me about \(\.$text) at \(\.$time), \(\.$repeats)")
    }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        guard let words = AssistantIntentLogic.cleanedReminderText(text) else {
            throw $text.needsValueError("What should I remind you about?")
        }
        let rule = repeats.rule
        guard let at = AssistantIntentLogic.reminderStart(at: time, rule: rule, now: Date(), calendar: .current) else {
            return .result(dialog: "That time has already passed. Please choose a later time.")
        }
        do {
            // The same core path as a reminder saved from Ask or Today: the person's own row, applied directly.
            _ = try await CoreEngine.shared.createReminder(
                NewReminder(text: words, times: [at.time], repeat: rule.rawValue, startDate: at.day, untilDate: nil,
                            noteID: nil, startSec: nil, medicineID: nil, sourceText: nil))
        } catch {
            NSLog("[sve] intent: add reminder failed: %@", String(describing: error))
            return .result(dialog: "I couldn't save the reminder. Please try again.")
        }
        await ReminderScheduler.shared.resync()   // so it rings, even if the app is not opened
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        let status = await UNUserNotificationCenter.current().notificationSettings().authorizationStatus
        let canRing = status == .authorized || status == .provisional || status == .ephemeral
        let when = rule == .once ? time.formatted(.dateTime.weekday(.wide).day().month(.wide).hour().minute())
            : AssistantIntentLogic.repeatWhen(rule: rule, time: time.formatted(date: .omitted, time: .shortened),
                                              weekday: time.formatted(.dateTime.weekday(.wide)))
        let line = AssistantIntentLogic.reminderSavedText(text: words, when: when, canRing: canRing)
        NSLog("[sve] intent: reminder saved for %@ %@ %@ (notifications %@)", at.day, at.time, rule.rawValue, canRing ? "on" : "off")
        return .result(dialog: IntentDialog(stringLiteral: line))
    }
}

@available(iOS 18.0, *)
struct AskSummaverickIntent: AppIntent {
    static let title: LocalizedStringResource = "Ask Summaverick"
    static let description = IntentDescription("Opens Summaverick and asks your question about your notes.")
    /// The answer needs the AI model, which only runs with the app in front: the app opens (iOS 18–25).
    static let openAppWhenRun = true

    @available(iOS 26.0, *)
    static var supportedModes: IntentModes { .foreground }

    @available(iOS 27.0, *)
    static var allowedExecutionTargets: IntentExecutionTargets { .main }

    @Parameter(title: "Question", requestValueDialog: "What would you like to ask?")
    var question: String

    static var parameterSummary: some ParameterSummary {
        Summary("Ask \(\.$question)")
    }

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult {
        let q = question.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !q.isEmpty else { throw $question.needsValueError("What would you like to ask?") }
        // RootView routes it once the app is unlocked and nothing is open over it; Ask asks it.
        ReminderScheduler.shared.pendingRoute = .ask(question: q)
        NSLog("[sve] intent: ask opened the app with a question")
        return .result()
    }
}

/// Siri phrases and the Shortcuts app's actions (also offered for the Action button). App target only.
@available(iOS 18.0, *)
struct SummaverickShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: StartRecordingIntent(),
                    phrases: ["Start recording in \(.applicationName)"],
                    shortTitle: "Start recording", systemImageName: "record.circle")
        AppShortcut(intent: StopRecordingIntent(),
                    phrases: ["Stop recording in \(.applicationName)"],
                    shortTitle: "Stop recording", systemImageName: "stop.circle")
        AppShortcut(intent: WhatsOnTodayIntent(),
                    phrases: ["What's on today in \(.applicationName)",
                              "What's on my day in \(.applicationName)",
                              "What do I have today in \(.applicationName)"],
                    shortTitle: "What's on today", systemImageName: "calendar")
        AppShortcut(intent: LastNoteSummaryIntent(),
                    phrases: ["Last note summary in \(.applicationName)",
                              "Read my last note in \(.applicationName)",
                              "What was my last note in \(.applicationName)"],
                    shortTitle: "Last note", systemImageName: "doc.text")
        AppShortcut(intent: AddReminderIntent(),
                    phrases: ["Add a reminder in \(.applicationName)",
                              "Remind me with \(.applicationName)"],
                    shortTitle: "Add a reminder", systemImageName: "bell.badge")
        AppShortcut(intent: AskSummaverickIntent(),
                    phrases: ["Ask \(.applicationName)",
                              "Ask \(.applicationName) a question"],
                    shortTitle: "Ask", systemImageName: "bubble.left.and.text.bubble.right")
    }
}
#endif
