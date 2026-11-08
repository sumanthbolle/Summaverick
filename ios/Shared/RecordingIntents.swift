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
    }
}
#endif
