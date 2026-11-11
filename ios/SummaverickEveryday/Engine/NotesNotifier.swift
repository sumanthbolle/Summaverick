// Local notifications about a recording's notes while the app is not in front: each part as it is ready (transcript,
// summary, notes), paused (the background work was cut off; it continues by itself) and failed (a real failure only).
// A notes run of more than one part says which part this is ("1 of 3 ready: what was said. Writing your summary…",
// StepCount) in the notification's subtitle. One identifier per note (notes.<id>), so a later state replaces the
// earlier one; at most one per stage per run (NotifyRule.next), never one per percent. Tapping opens the note through ReminderScheduler.pendingRoute. Uses
// the same notification permission as reminders. Nothing is posted while the app is active (it shows the note).
import UIKit
import UserNotifications

@MainActor
enum NotesNotifier {
    /// Identifiers of these notifications start with this (NotifyRule.identifier).
    nonisolated static let prefix = "notes."
    /// The notices of a note still being written carry "Finish now": it opens the app into the mode (FinishNowView),
    /// where the notes are written on the GPU. Answered in ReminderScheduler.
    nonisolated static let workingCategory = "sve.notes.working"
    nonisolated static let finishNowAction = "sve.notes.finishnow"

    nonisolated static var workingCategoryDefinition: UNNotificationCategory {
        let finish = UNNotificationAction(identifier: finishNowAction, title: String(localized: "Finish now"), options: [.foreground])
        return UNNotificationCategory(identifier: workingCategory, actions: [finish], intentIdentifiers: [], options: [])
    }

    /// The transcript is stored after Stop (the summary and notes follow).
    static func transcriptReady(noteID: Int64, title: String, step: String? = nil, hold: Hold? = nil) {
        // With the placeholder title ("Recording 10 Nov, 14:05") the body does not name the note: the subtitle says
        // which part is ready and what comes next.
        let body = NotifyRule.transcriptBodyNamesNote(title: title)
            ? String(localized: "\"\(title)\": writing your notes now.")
            : String(localized: "I'll let you know as each part is ready.")
        post(noteID, state: "transcript ready", title: String(localized: "Heard it all"), subtitle: step, body: body,
             finishNow: true, hold: hold)
    }

    /// The title and the short summary are stored (the detailed notes and the questions follow).
    static func summaryReady(noteID: Int64, title: String, step: String? = nil, hold: Hold? = nil) {
        let heading: String, body: String
        switch NotifyCopy.variant(noteID: noteID, stage: NotifyRule.summary, count: 2) {
        case 0:
            heading = NotifyCopy.storedFirstName().map { String(localized: "\($0), the short version is here ☕") }
                ?? String(localized: "The short version is here ☕")
            body = String(localized: "\"\(title)\": read it in a minute.")
        default:
            heading = String(localized: "Got the gist!")
            body = String(localized: "\"\(title)\": the full notes are on the way.")
        }
        post(noteID, state: "summary ready", title: heading, subtitle: step, body: body, finishNow: true, hold: hold)
    }

    /// The note's detailed notes and questions are written (the memories may still follow).
    static func notesReady(noteID: Int64, title: String, step: String? = nil, hold: Hold? = nil) {
        let heading: String, body: String
        switch NotifyCopy.variant(noteID: noteID, stage: NotifyRule.notes, count: 3) {
        case 0:
            heading = NotifyCopy.storedFirstName().map { String(localized: "All done, \($0) 🎉") }
                ?? String(localized: "All done 🎉")
            body = String(localized: "\"\(title)\": notes and quiz questions are ready.")
        case 1:
            heading = String(localized: "Fresh notes, just served 🍽️")
            body = String(localized: "\"\(title)\" is ready when you are.")
        default:
            heading = String(localized: "You can relax now")
            body = String(localized: "I wrote it all down: \"\(title)\".")
        }
        post(noteID, state: "ready", title: heading, subtitle: step, body: body, hold: hold)
    }

    /// The background work was cut off; the note continues by itself (the overnight task, or when the app is opened).
    static func paused(noteID: Int64, title: String, percent: Int, hold: Hold? = nil) {
        post(noteID, state: "paused at \(percent)%", title: String(localized: "Taking a short break"),
             body: String(localized: "\"\(title)\" is safe. I'll carry on by myself soon."), finishNow: true, hold: hold)
    }

    /// Right after a Stop from the background with no background time (TaskAtRecord.postsOpenAppNotice): the
    /// recording is kept and nothing is written until the app is opened. Tapping opens the note.
    /// False when nothing was posted (the app is in front).
    @discardableResult
    static func recordingSaved(noteID: Int64, hold: Hold? = nil) -> Bool {
        post(noteID, state: "saved, open the app", title: String(localized: "Your recording is saved"),
             body: String(localized: "Open Summaverick and I'll write your notes. It takes a few minutes."), finishNow: true,
             hold: hold)
    }

    /// The notes run failed for real (not a cut-off); `line` is the failure the note shows.
    static func failed(noteID: Int64, title: String, line: String, hold: Hold? = nil) {
        post(noteID, state: "failed", title: String(localized: "Sorry, I couldn't write this one"),
             body: String(localized: "\"\(title)\": \(line)"), hold: hold)
    }

    /// hold: background time the caller took synchronously, before any await (right after a cut-off the app may be
    /// suspended at any moment); ended once the request is handed over. Without one, it is taken here.
    /// finishNow: the notes are still being written, so the notice carries the "Finish now" action.
    /// False when nothing is posted (the app is in front).
    @discardableResult
    private static func post(_ noteID: Int64, state: String, title: String, subtitle: String? = nil, body: String,
                             finishNow: Bool = false, hold given: Hold?) -> Bool {
        let hold = given ?? Hold()
        guard NotifyRule.shouldNotify(appActive: UIApplication.shared.applicationState == .active) else {
            NSLog("[sve] bg: note %lld %@ with the app in front: no notification", noteID, state)
            hold.end()
            return false
        }
        let content = UNMutableNotificationContent()
        content.title = title
        if let subtitle { content.subtitle = subtitle }
        content.body = body
        if QuietHours.isQuietNow() {   // night: arrives silently, still on the lock screen and in the list
            content.sound = nil
            content.interruptionLevel = .passive
        } else {
            content.sound = .default
        }
        content.userInfo = ["note_id": NSNumber(value: noteID)]
        if finishNow { content.categoryIdentifier = workingCategory }
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
        return true
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

/// Stop when quiet (ruling B11): the one warning before a recording stops by itself ("It's gone quiet. I'll stop recording
/// in 30 seconds.", with Keep recording), and then the line that says it stopped. One identifier, so the second replaces
/// the first, and voice coming back or Keep recording withdraws it. The action is answered in ReminderScheduler (the
/// notification delegate); the app is running anyway, since it records. Same notification permission as reminders.
@MainActor
enum QuietNotifier {
    nonisolated static let identifier = "quiet.recording"
    nonisolated static let category = "sve.quiet"
    nonisolated static let keepAction = "sve.quiet.keep"

    /// At launch, before a notification can be answered. Registers the app's notification categories together (each
    /// call replaces them all): this one, and the notes notices' "Finish now" (NotesNotifier.workingCategory).
    nonisolated static func registerCategory() {
        let keep = UNNotificationAction(identifier: keepAction, title: String(localized: "Keep recording"), options: [])
        let c = UNNotificationCategory(identifier: category, actions: [keep], intentIdentifiers: [], options: [])
        UNUserNotificationCenter.current().setNotificationCategories([c, NotesNotifier.workingCategoryDefinition])
    }

    static func warn(_ line: String) { post(title: String(localized: "Still there? 👀"), line, category: category) }

    static func stopped(_ line: String) { post(title: "Summaverick", line, category: nil) }

    static func withdraw() {
        let c = UNUserNotificationCenter.current()
        c.removePendingNotificationRequests(withIdentifiers: [identifier])
        c.removeDeliveredNotifications(withIdentifiers: [identifier])
    }

    private static func post(title: String, _ body: String, category: String?) {
        let hold = NotesNotifier.Hold()
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        if QuietHours.isQuietNow() {   // night: arrives silently, still on the lock screen and in the list
            content.sound = nil
            content.interruptionLevel = .passive
        } else {
            content.sound = .default
        }
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
