import Foundation
import Observation
import UserNotifications

/// Where a tapped reminder notification should take the person.
enum ReminderRoute: Equatable {
    case note(id: Int64, startSec: Int?)
    case profile
    case deletedRecording(reminderID: Int64)
    /// "Finish now" on a note's notes notice: the note opens, with the mode over it (FinishNowView).
    case finishNow(noteID: Int64)
}

/// One notification request, computed without touching UNUserNotificationCenter.
struct PlannedRequest: Equatable {
    var identifier: String
    var body: String
    var components: DateComponents
    var repeats: Bool
    var fireDate: Date
    var reminderID: Int64
    var noteID: Int64?
    var startSec: Int?
    var fromDeleted: Bool

    var userInfo: [String: Any] {
        var u: [String: Any] = ["reminder_id": reminderID, "from_deleted": fromDeleted]
        if let noteID { u["note_id"] = noteID }
        if let startSec { u["start_sec"] = startSec }
        return u
    }
}

/// Schedules local notifications for active reminders.
///
/// Open-ended reminders (no until_date, already started) use one repeating trigger per time.
/// Bounded ones (a future start_date or any until_date) get one-shot requests over a short horizon:
/// 7 days of daily times, or the next 4 weekly occurrences. Each resync (launch, `.assistantChanged`,
/// app becomes active) tops these up. LIMIT: a bounded course stops alerting if the app is not
/// opened for 7 days (daily) or about 4 weeks (weekly).
@MainActor
@Observable
final class ReminderScheduler: NSObject, UNUserNotificationCenterDelegate {
    static let shared = ReminderScheduler()
    nonisolated static let prefix = "sve.rem."
    /// iOS keeps at most 64 pending local notifications; stay under it.
    nonisolated static let maxRequests = 60

    /// Set when a notification is tapped. The Router reads and clears it.
    var pendingRoute: ReminderRoute?

    @ObservationIgnored private var syncing = false
    @ObservationIgnored private var dirty = false

    private override init() { super.init() }

    func requestAuthorization() async -> Bool {
        (try? await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge])) ?? false
    }

    /// At a Stop, in the tap (the app is in front): asks once, only while the person has not answered yet, so
    /// "Notes ready" can be told. Same options as reminders.
    func requestAuthorizationIfUndetermined() {
        Task {
            guard await UNUserNotificationCenter.current().notificationSettings().authorizationStatus == .notDetermined else { return }
            let granted = await requestAuthorization()
            NSLog("[sve] bg: notification permission asked at Stop: %@", granted ? "granted" : "not granted")
        }
    }

    /// Rebuilds all `sve.rem.` requests from the active reminders. Safe to call often.
    func resync() async {
        // Night quiet for notes notices follows the profile's bedtime and wake-up (QuietHours).
        if let r = await CoreEngine.shared.profile()?.routine { QuietHours.remember(bed: r.bed, wake: r.wake) }
        if syncing { dirty = true; return }
        syncing = true
        defer { syncing = false }
        repeat {
            dirty = false
            let reminders: [Reminder]
            do {
                reminders = try await CoreEngine.shared.remindersStrict(activeOnly: true)
            } catch {
                // Never wipe a working schedule because the read failed.
                NSLog("ReminderScheduler: reminder read failed, schedule left untouched: %@", "\(error)")
                continue
            }
            let planned = Self.plan(reminders, now: Date(), calendar: .current)
            let center = UNUserNotificationCenter.current()
            // Add or replace first (same identifier replaces), so there is never a gap.
            for p in planned {
                let content = UNMutableNotificationContent()
                content.title = "Summaverick"
                content.body = p.body
                content.sound = .default
                content.userInfo = p.userInfo
                let trigger = UNCalendarNotificationTrigger(dateMatching: p.components, repeats: p.repeats)
                do {
                    try await center.add(UNNotificationRequest(identifier: p.identifier, content: content, trigger: trigger))
                } catch {
                    NSLog("ReminderScheduler: could not add %@: %@", p.identifier, "\(error)")
                }
            }
            let keep = Set(planned.map(\.identifier))
            let pending = await center.pendingNotificationRequests()
            let stale = pending.map(\.identifier).filter { $0.hasPrefix(Self.prefix) && !keep.contains($0) }
            center.removePendingNotificationRequests(withIdentifiers: stale)
        } while dirty
    }

    // MARK: planning (pure)

    /// Horizon for bounded schedules: this many days of daily times, or this many weekly occurrences.
    nonisolated static let dailyHorizonDays = 7
    nonisolated static let weeklyOccurrences = 4

    nonisolated static func plan(_ reminders: [Reminder], now: Date, calendar: Calendar) -> [PlannedRequest] {
        let today = calendar.startOfDay(for: now)
        var repeating: [PlannedRequest] = []
        var oneShots: [PlannedRequest] = []
        var seen = Set<String>()

        for r in reminders where r.active {
            guard !r.times.isEmpty else { NSLog("ReminderScheduler: reminder %lld has no times, skipped", r.id); continue }
            var start = today
            if let s = r.startDate {
                guard let d = day(s, calendar) else { NSLog("ReminderScheduler: reminder %lld bad start_date '%@', skipped", r.id, s); continue }
                start = d
            }
            var untilEnd: Date?
            if let u = r.untilDate {
                guard let d = day(u, calendar) else { NSLog("ReminderScheduler: reminder %lld bad until_date '%@', skipped", r.id, u); continue }
                if d < today { continue }
                untilEnd = calendar.date(byAdding: .day, value: 1, to: d)
            }
            let kind = r.repeat ?? "once"
            let base = max(start, today)
            let bounded = start > today || untilEnd != nil
            // A "once" reminder fires only on its own date: start_date, else the day it was created
            // (created_at is epoch seconds), else today. Never max(start, today): the core does not
            // deactivate fired once-reminders, so a past date must stay silent.
            let onceDay: Date = r.startDate != nil ? start
                : (r.createdAt.map { calendar.startOfDay(for: Date(timeIntervalSince1970: TimeInterval($0))) } ?? today)
            // Weekly reminders repeat on their anchor day's weekday: start_date, else the day they were created
            // (same anchor as "once"). Never today's weekday, or the day would move with every resync.
            let weekday = calendar.component(.weekday, from: onceDay)

            func make(_ id: String, _ c: DateComponents, _ repeats: Bool, _ fire: Date) -> PlannedRequest {
                PlannedRequest(identifier: id, body: r.text, components: c, repeats: repeats, fireDate: fire,
                               reminderID: r.id, noteID: r.noteID, startSec: r.startSec, fromDeleted: r.fromDeleted)
            }
            func shot(_ d: Date, _ h: Int, _ m: Int, _ hhmm: String) -> PlannedRequest? {
                var c = calendar.dateComponents([.year, .month, .day], from: d)
                c.hour = h; c.minute = m
                guard let f = calendar.date(from: c), f > now else { return nil }
                if let end = untilEnd, f >= end { return nil }
                let ymd = String(format: "%04d%02d%02d", c.year ?? 0, c.month ?? 0, c.day ?? 0)
                return make("\(prefix)\(r.id).\(ymd).\(hhmm)", c, false, f)
            }

            for t in r.times {
                guard let (h, m) = clock(t) else { NSLog("ReminderScheduler: reminder %lld bad time '%@', skipped", r.id, t); continue }
                let hhmm = String(format: "%02d:%02d", h, m)
                var comps = DateComponents(); comps.hour = h; comps.minute = m

                switch kind {
                case "daily", "twice_daily":
                    if bounded {
                        for k in 0..<dailyHorizonDays {
                            guard let d = calendar.date(byAdding: .day, value: k, to: base),
                                  let p = shot(d, h, m, hhmm), seen.insert(p.identifier).inserted else { continue }
                            oneShots.append(p)
                        }
                    } else {
                        let id = "\(prefix)\(r.id).\(hhmm)"
                        if seen.insert(id).inserted,
                           let f = calendar.nextDate(after: now, matching: comps, matchingPolicy: .nextTime) {
                            repeating.append(make(id, comps, true, f))
                        }
                    }
                case "weekly":
                    if bounded {
                        var found = 0, k = 0
                        while found < weeklyOccurrences && k < 7 * (weeklyOccurrences + 2) {
                            defer { k += 1 }
                            guard let d = calendar.date(byAdding: .day, value: k, to: base),
                                  calendar.component(.weekday, from: d) == weekday else { continue }
                            if let end = untilEnd, d >= end { break }
                            // Occurrences already past today don't count toward the horizon.
                            guard let p = shot(d, h, m, hhmm) else { continue }
                            found += 1
                            if seen.insert(p.identifier).inserted { oneShots.append(p) }
                        }
                    } else {
                        let id = "\(prefix)\(r.id).\(hhmm)"
                        comps.weekday = weekday
                        if seen.insert(id).inserted,
                           let f = calendar.nextDate(after: now, matching: comps, matchingPolicy: .nextTime) {
                            repeating.append(make(id, comps, true, f))
                        }
                    }
                default: // "once" and anything unknown
                    if let p = shot(onceDay, h, m, hhmm), seen.insert(p.identifier).inserted { oneShots.append(p) }
                }
            }
        }
        let byDate: (PlannedRequest, PlannedRequest) -> Bool = {
            $0.fireDate != $1.fireDate ? $0.fireDate < $1.fireDate : $0.identifier < $1.identifier
        }
        // Repeating triggers are kept first, then the soonest one-shots.
        let all = repeating.sorted(by: byDate) + oneShots.sorted(by: byDate)
        if all.count > maxRequests {
            NSLog("ReminderScheduler: %d requests planned, cap dropped %d", all.count, all.count - maxRequests)
        }
        return Array(all.prefix(maxRequests))
    }

    nonisolated static func clock(_ s: String) -> (Int, Int)? {
        let p = s.split(separator: ":", omittingEmptySubsequences: false)
        guard p.count == 2, let h = Int(p[0]), let m = Int(p[1]), (0...23).contains(h), (0...59).contains(m),
              p[1].count == 2 else { return nil }
        return (h, m)
    }

    nonisolated static func day(_ s: String, _ calendar: Calendar) -> Date? {
        let p = s.split(separator: "-", omittingEmptySubsequences: false)
        guard p.count == 3, p[0].count == 4, let y = Int(p[0]), let m = Int(p[1]), let d = Int(p[2]) else { return nil }
        let c = DateComponents(year: y, month: m, day: d)
        guard let date = calendar.date(from: c), calendar.dateComponents([.year, .month, .day], from: date) == c else { return nil }
        return calendar.startOfDay(for: date)
    }

    nonisolated static func route(from info: [AnyHashable: Any]) -> ReminderRoute {
        func int64(_ k: String) -> Int64? { (info[k] as? NSNumber)?.int64Value }
        if (info["from_deleted"] as? Bool) == true, let rid = int64("reminder_id") { return .deletedRecording(reminderID: rid) }
        if let nid = int64("note_id") { return .note(id: nid, startSec: (info["start_sec"] as? NSNumber)?.intValue) }
        return .profile
    }

    // MARK: UNUserNotificationCenterDelegate

    // Completion-handler versions, answered on the main thread: with the async versions the system's completion runs
    // on whatever thread the method returned on, and UIKit ends the app ("Call must be made on main thread"), seen
    // on the owner's iPhone when a "paused" notification was tapped.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        // A note's notes notification arriving in front is not shown: the app already shows the note.
        let options: UNNotificationPresentationOptions =
            notification.request.identifier.hasPrefix(NotesNotifier.prefix) ? [] : [.banner, .list, .sound]
        DispatchQueue.main.async { completionHandler(options) }
    }

    /// Reminders, and a note's notes notifications (NotesNotifier: their userInfo has the note id), open their note.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                            withCompletionHandler completionHandler: @escaping () -> Void) {
        let identifier = response.notification.request.identifier
        let action = response.actionIdentifier
        let route = Self.route(from: response.notification.request.content.userInfo)
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                // Stop when quiet's warning: Keep recording carries on (a tap only opens the app).
                if identifier == QuietNotifier.identifier {
                    if action == QuietNotifier.keepAction { RecordingController.shared.keepRecording() }
                } else if action == NotesNotifier.finishNowAction, identifier.hasPrefix(NotesNotifier.prefix),
                          case .note(let id, _) = route {
                    ReminderScheduler.shared.pendingRoute = .finishNow(noteID: id)
                } else if action == UNNotificationDefaultActionIdentifier,
                          identifier.hasPrefix(Self.prefix) || identifier.hasPrefix(NotesNotifier.prefix) {
                    ReminderScheduler.shared.pendingRoute = route
                }
            }
            completionHandler()
        }
    }
}
