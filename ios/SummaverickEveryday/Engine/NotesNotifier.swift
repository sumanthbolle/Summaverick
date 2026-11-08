// Local notifications about a recording's notes while the app is not in front: ready, paused (the background work
// was cut off) and failed. One identifier per note (notes.<id>), so a later state replaces the earlier one; at most
// one per state change, never one per percent. Tapping opens the note through ReminderScheduler.pendingRoute. Uses
// the same notification permission as reminders. Nothing is posted while the app is active (it shows the note).
import UIKit
import UserNotifications

@MainActor
enum NotesNotifier {
    /// Identifiers of these notifications start with this (NotifyRule.identifier).
    nonisolated static let prefix = "notes."

    /// The note's notes are written (the memories may still follow).
    static func notesReady(noteID: Int64, title: String) {
        post(noteID, state: "ready", body: String(localized: "Notes ready: \(title)"))
    }

    /// The background work was cut off; the note finishes later by itself (the overnight task) or when tapped.
    static func paused(noteID: Int64, title: String, percent: Int) {
        post(noteID, state: "paused",
             body: String(localized: "\(title): paused at \(percent)%. It will finish while charging, or tap to finish now.",
                          comment: "Note title, percent of the work done"))
    }

    /// The notes run failed for real (not a cut-off); `line` is the failure the note shows.
    static func failed(noteID: Int64, title: String, line: String) {
        post(noteID, state: "failed", body: String(localized: "\(title): notes could not be written") + "\n" + line)
    }

    private static func post(_ noteID: Int64, state: String, body: String) {
        guard NotifyRule.shouldNotify(appActive: UIApplication.shared.applicationState == .active) else {
            NSLog("[sve] bg: note %lld %@ with the app in front: no notification", noteID, state)
            return
        }
        let content = UNMutableNotificationContent()
        content.title = "Summaverick"
        content.body = body
        content.sound = .default
        content.userInfo = ["note_id": NSNumber(value: noteID)]
        let request = UNNotificationRequest(identifier: NotifyRule.identifier(noteID: noteID), content: content, trigger: nil)
        // Right after a cut-off iOS may suspend the app at any moment: hold on long enough to hand the request over.
        let hold = BackgroundHold()
        hold.begin()
        Task {
            do {
                try await UNUserNotificationCenter.current().add(request)
                NSLog("[sve] bg: note %lld %@: notification posted", noteID, state)
            } catch {
                NSLog("[sve] bg: note %lld %@: notification not posted: %@", noteID, state, String(describing: error))
            }
            hold.end()
        }
    }

    /// A short background-time assertion, ended exactly once (by the caller or when iOS takes the time back).
    @MainActor
    private final class BackgroundHold {
        private var id: UIBackgroundTaskIdentifier = .invalid
        func begin() {
            // iOS calls the expiration handler on the main thread.
            id = UIApplication.shared.beginBackgroundTask(withName: "sve.notify") { [self] in
                MainActor.assumeIsolated { end() }
            }
        }
        func end() {
            guard id != .invalid else { return }
            UIApplication.shared.endBackgroundTask(id)
            id = .invalid
        }
    }
}
