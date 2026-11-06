import Foundation
import Observation
import UserNotifications

/// Where a tapped reminder notification should take the person.
enum ReminderRoute: Equatable {
    case note(id: Int64, startSec: Int?)
    case profile
    case deletedRecording(reminderID: Int64)
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

    /// Rebuilds all `sve.rem.` requests from the active reminders. Safe to call often.
    func resync() async {
        if syncing { dirty = true; return }
        syncing = true
        defer { syncing = false }
        repeat {
            dirty = false
            let reminders = await CoreEngine.shared.reminders(activeOnly: true)
            let planned = Self.plan(reminders, now: Date(), calendar: .current)
            let center = UNUserNotificationCenter.current()
            let pending = await center.pendingNotificationRequests()
            let old = pending.map(\.identifier).filter { $0.hasPrefix(Self.prefix) }
            center.removePendingNotificationRequests(withIdentifiers: old)
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
        } while dirty
    }

    // MARK: planning (pure)

    nonisolated static func plan(_ reminders: [Reminder], now: Date, calendar: Calendar) -> [PlannedRequest] {
        let today = calendar.startOfDay(for: now)
        var out: [PlannedRequest] = []
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
            let weekday = calendar.component(.weekday, from: start)

            for t in r.times {
                guard let (h, m) = clock(t) else { NSLog("ReminderScheduler: reminder %lld bad time '%@', skipped", r.id, t); continue }
                let id = "\(prefix)\(r.id).\(t)"
                guard seen.insert(id).inserted else { continue }

                var comps = DateComponents(); comps.hour = h; comps.minute = m
                var repeats = false
                var fire: Date?

                func oneShot(_ day: Date) -> Date? {
                    var c = calendar.dateComponents([.year, .month, .day], from: day)
                    c.hour = h; c.minute = m
                    guard let d = calendar.date(from: c), d > now else { return nil }
                    comps = c
                    return d
                }

                switch kind {
                case "daily", "twice_daily":
                    if start > today {
                        fire = oneShot(start)   // cannot express "daily from a future date"; re-synced when it arrives
                    } else {
                        repeats = true
                        fire = calendar.nextDate(after: now, matching: comps, matchingPolicy: .nextTime)
                    }
                case "weekly":
                    if start > today {
                        fire = oneShot(start)
                    } else {
                        comps.weekday = weekday
                        repeats = true
                        fire = calendar.nextDate(after: now, matching: comps, matchingPolicy: .nextTime)
                    }
                default: // "once" and anything unknown
                    fire = oneShot(start)
                }
                guard let f = fire else { continue }
                if let end = untilEnd, f >= end { continue }
                out.append(PlannedRequest(identifier: id, body: r.text, components: comps, repeats: repeats, fireDate: f,
                                          reminderID: r.id, noteID: r.noteID, startSec: r.startSec, fromDeleted: r.fromDeleted))
            }
        }
        out.sort { $0.fireDate != $1.fireDate ? $0.fireDate < $1.fireDate : $0.identifier < $1.identifier }
        return Array(out.prefix(maxRequests))
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

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification) async -> UNNotificationPresentationOptions {
        [.banner, .list, .sound]
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        guard response.actionIdentifier == UNNotificationDefaultActionIdentifier,
              response.notification.request.identifier.hasPrefix(Self.prefix) else { return }
        let route = Self.route(from: response.notification.request.content.userInfo)
        await MainActor.run { ReminderScheduler.shared.pendingRoute = route }
    }
}
