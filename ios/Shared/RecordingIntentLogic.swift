// Pure rules of the recording shortcuts (RecordingIntents.swift): what a start or stop says, and when the app opens
// instead. Foundation only and free of app state, so they can be checked with a standalone swiftc script.
import Foundation

enum RecordingIntentLogic {
    /// RecordingController.StartResult, one for one (the app maps it with an exhaustive switch).
    enum Start: CaseIterable, Equatable {
        case started, alreadyRecording, needsConsent, micDenied, failed
    }

    /// What a start does after the controller answered.
    enum Reply: Equatable {
        /// Finish with this line (spoken by Siri, shown by Shortcuts).
        case say(Line)
        /// The recording reminder was never acknowledged: open the app at it. Nothing records.
        case openForConsent
    }

    enum Line: CaseIterable, Equatable {
        case recording, alreadyRecording, allowMicrophone, couldNotStart, stopped, notRecording
        /// Shown when the app asks to be opened for the recording reminder.
        case consentFirst
    }

    /// The Control Center toggle: what the new value asks for.
    enum Toggle: Equatable { case start, stop }

    static func reply(to start: Start) -> Reply {
        switch start {
        case .started: return .say(.recording)
        case .alreadyRecording: return .say(.alreadyRecording)
        case .needsConsent: return .openForConsent
        case .micDenied: return .say(.allowMicrophone)
        case .failed: return .say(.couldNotStart)
        }
    }

    static func reply(stopped: Bool) -> Line { stopped ? .stopped : .notRecording }

    static func toggle(on: Bool) -> Toggle { on ? .start : .stop }

    static func text(_ line: Line) -> LocalizedStringResource {
        switch line {
        case .recording: return LocalizedStringResource("Recording")
        case .alreadyRecording: return LocalizedStringResource("Already recording")
        case .allowMicrophone: return LocalizedStringResource("Allow the microphone for Summaverick in Settings.")
        case .couldNotStart: return LocalizedStringResource("Could not start recording.")
        case .stopped: return LocalizedStringResource("Recording stopped. Your notes are being written.")
        case .notRecording: return LocalizedStringResource("Nothing is being recorded.")
        case .consentFirst: return LocalizedStringResource("Open Summaverick to read the recording reminder first.")
        }
    }

    /// How long a shortcut's request to show the recording reminder is kept: opening the app much later must not
    /// show it (its "I understand" opens the recorder).
    static let consentRequestWindow: TimeInterval = 120

    static func consentRequestIsFresh(requestedAt: Date?, now: Date) -> Bool {
        guard let requestedAt else { return false }
        let age = now.timeIntervalSince(requestedAt)
        return age >= 0 && age <= consentRequestWindow
    }
}
