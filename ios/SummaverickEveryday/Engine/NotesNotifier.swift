// Local notifications about a recording's notes while the app is not in front: each part as it is ready (transcript,
// summary, notes), paused (the background work was cut off; it continues by itself) and failed (a real failure only).
// One identifier per note (notes.<id>), so a later state replaces the earlier one; at most one per stage per run
// (NotifyRule.next), never one per percent. Tapping opens the note through ReminderScheduler.pendingRoute. Uses
// the same notification permission as reminders. Nothing is posted while the app is active (it shows the note).
import UIKit
import UserNotifications

@MainActor
enum NotesNotifier {
    /// Identifiers of these notifications start with this (NotifyRule.identifier).
    nonisolated static let prefix = "notes."

    /// The transcript is stored after Stop (the summary and notes follow).
    static func transcriptReady(noteID: Int64, title: String, hold: Hold? = nil) {
        post(noteID, state: "transcript ready", body: String(localized: "Transcript ready: \(title)"), hold: hold)
    }

    /// The title and the short summary are stored (the detailed notes and the questions follow).
    static func summaryReady(noteID: Int64, title: String, hold: Hold? = nil) {
        post(noteID, state: "summary ready", body: String(localized: "Summary ready: \(title)"), hold: hold)
    }

    /// The note's detailed notes and questions are written (the memories may still follow).
    static func notesReady(noteID: Int64, title: String, hold: Hold? = nil) {
        post(noteID, state: "ready", body: String(localized: "Notes ready: \(title)"), hold: hold)
    }

    /// The background work was cut off; the note continues by itself (the overnight task, or when the app is opened).
    static func paused(noteID: Int64, title: String, percent: Int, hold: Hold? = nil) {
        post(noteID, state: "paused",
             body: String(localized: "\(title): paused at \(percent)%. It continues by itself.",
                          comment: "Note title, percent of the work done"),
             hold: hold)
    }

    /// The notes run failed for real (not a cut-off); `line` is the failure the note shows.
    static func failed(noteID: Int64, title: String, line: String, hold: Hold? = nil) {
        post(noteID, state: "failed", body: String(localized: "\(title): notes could not be written") + "\n" + line, hold: hold)
    }

    /// hold: background time the caller took synchronously, before any await (right after a cut-off the app may be
    /// suspended at any moment); ended once the request is handed over. Without one, it is taken here.
    private static func post(_ noteID: Int64, state: String, body: String, hold given: Hold?) {
        let hold = given ?? Hold()
        guard NotifyRule.shouldNotify(appActive: UIApplication.shared.applicationState == .active) else {
            NSLog("[sve] bg: note %lld %@ with the app in front: no notification", noteID, state)
            hold.end()
            return
        }
        let content = UNMutableNotificationContent()
        content.title = "Summaverick"
        content.body = body
        content.sound = .default
        content.userInfo = ["note_id": NSNumber(value: noteID)]
        let request = UNNotificationRequest(identifier: NotifyRule.identifier(noteID: noteID), content: content, trigger: nil)
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

    /// A short background-time assertion, taken when made and ended exactly once (after the post, or when iOS takes
    /// the time back).
    @MainActor
    final class Hold {
        private var id: UIBackgroundTaskIdentifier = .invalid
        init() {
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

/// Stop when quiet (ruling B11): the one warning before a recording stops by itself ("No voice for a while — stopping
/// in 30 seconds", with Keep recording), and then the line that says it stopped. One identifier, so the second replaces
/// the first, and voice coming back or Keep recording withdraws it. The action is answered in ReminderScheduler (the
/// notification delegate); the app is running anyway, since it records. Same notification permission as reminders.
@MainActor
enum QuietNotifier {
    nonisolated static let identifier = "quiet.recording"
    nonisolated static let category = "sve.quiet"
    nonisolated static let keepAction = "sve.quiet.keep"

    /// At launch, before a notification can be answered. The app's only notification category.
    nonisolated static func registerCategory() {
        let keep = UNNotificationAction(identifier: keepAction, title: String(localized: "Keep recording"), options: [])
        let c = UNNotificationCategory(identifier: category, actions: [keep], intentIdentifiers: [], options: [])
        UNUserNotificationCenter.current().setNotificationCategories([c])
    }

    static func warn(_ line: String) { post(line, category: category) }

    static func stopped(_ line: String) { post(line, category: nil) }

    static func withdraw() {
        let c = UNUserNotificationCenter.current()
        c.removePendingNotificationRequests(withIdentifiers: [identifier])
        c.removeDeliveredNotifications(withIdentifiers: [identifier])
    }

    private static func post(_ body: String, category: String?) {
        let hold = NotesNotifier.Hold()
        let content = UNMutableNotificationContent()
        content.title = "Summaverick"
        content.body = body
        content.sound = .default
        if let category { content.categoryIdentifier = category }
        let request = UNNotificationRequest(identifier: identifier, content: content, trigger: nil)
        Task {
            do {
                try await UNUserNotificationCenter.current().add(request)
                NSLog("[sve] quiet: notification posted")
            } catch {
                NSLog("[sve] quiet: notification not posted: %@", String(describing: error))
            }
            hold.end()
        }
    }
}
