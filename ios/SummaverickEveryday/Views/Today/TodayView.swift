// Today (spec §6.1): greeting, suggestions waiting for a Yes, reminders for the next few days,
// open to-dos, one insight, and a bar that jumps to Ask.
import SwiftUI
import UserNotifications

/// One time a reminder goes off, for the Reminders list.
struct ReminderOccurrence: Identifiable, Equatable {
    let reminder: Reminder
    let day: Date        // start of that day
    let date: Date       // the exact time
    let hhmm: String
    var id: String { "\(reminder.id).\(AssistantFormat.ymd(day)).\(hhmm)" }

    /// Occurrences from today through `days - 1` days ahead, soonest first. Mirrors ReminderScheduler's rules
    /// (start/until dates, once/daily/twice_daily/weekly) but includes today's earlier times too.
    static func upcoming(_ reminders: [Reminder], now: Date, calendar: Calendar, days: Int = 4) -> [ReminderOccurrence] {
        let today = calendar.startOfDay(for: now)
        var out: [ReminderOccurrence] = []
        for r in reminders where r.active && !r.times.isEmpty {
            let start = r.startDate.flatMap { ReminderScheduler.day($0, calendar) }
            if r.startDate != nil && start == nil { continue }
            let until = r.untilDate.flatMap { ReminderScheduler.day($0, calendar) }
            if r.untilDate != nil && until == nil { continue }
            let created = r.createdAt.map { calendar.startOfDay(for: Date(timeIntervalSince1970: TimeInterval($0))) }
            let onceDay = start ?? created ?? today
            let weekday = calendar.component(.weekday, from: start ?? created ?? today)
            for k in 0..<days {
                guard let d = calendar.date(byAdding: .day, value: k, to: today) else { continue }
                if let start, d < start { continue }
                if let until, d > until { continue }
                let on: Bool
                switch r.repeat ?? "once" {
                case "daily", "twice_daily": on = true
                case "weekly": on = calendar.component(.weekday, from: d) == weekday
                default: on = d == onceDay
                }
                guard on else { continue }
                for t in r.times {
                    guard let (h, m) = ReminderScheduler.clock(t),
                          let at = calendar.date(bySettingHour: h, minute: m, second: 0, of: d) else { continue }
                    out.append(ReminderOccurrence(reminder: r, day: d, date: at, hhmm: String(format: "%02d:%02d", h, m)))
                }
            }
        }
        return out.sorted { $0.date != $1.date ? $0.date < $1.date : $0.id < $1.id }
    }
}

struct TodayView: View {
    @Environment(Router.self) private var router
    @Environment(\.scenePhase) private var scenePhase
    @State private var pipeline = ProcessingPipeline.shared
    @State private var models = ModelManager.shared
    @State private var lock = AppLock.shared

    @State private var loaded = false
    @State private var suggestions: [Suggestion] = []
    @State private var reminders: [Reminder] = []
    @State private var tasks: [TaskItem] = []
    @State private var insight: Insight?
    @State private var profile: Profile?
    @State private var titles: [Int64: String] = [:]
    @State private var staleIDs: Set<Int64> = []
    @State private var busyIDs: Set<Int64> = []
    @State private var errorMessage: String?
    @State private var notificationsOff = false
    @State private var now = Date()
    @State private var showSetup = false
    @AppStorage("ai.unsupportedShown") private var unsupportedShown = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    greeting
                    if pipeline.isBusy || isFailed || isPaused {
                        StatusBanner(stage: pipeline.stage).clipShape(RoundedRectangle(cornerRadius: 10))
                    }
                    if models.status == .unsupported {
                        if !unsupportedShown { AIUnsupportedCard { unsupportedShown = true } }
                    } else if !models.isReady {
                        AIDownloadCard()
                    }
                    if loaded && needsProfile { setupProfileCard }
                    waitingSection
                    remindersSection
                    tasksSection
                    if let insight { insightCard(insight) }
                    if models.isReady { askBar }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 32)
            }
            .alert("Could not save this. Please try again.", isPresented: errorShown) {
                Button("OK", role: .cancel) {}
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .principal) { BrandTitleInline() }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { router.showSettings = true } label: {
                        Image(systemName: "gearshape").frame(minWidth: 44, minHeight: 44)
                    }
                    .accessibilityLabel(Text("Settings"))
                }
            }
            .refreshable { await reload() }
            .task { await reload() }
            .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
            .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
            .onChange(of: scenePhase) { _, phase in if phase == .active { Task { await reload() } } }
            .onChange(of: notificationsOff || errorMessage != nil) { _, open in PresentationGate.shared.set("today", open) }
            .sheet(isPresented: $showSetup, onDismiss: { Task { await reload() } }) { ProfileSetupView() }
            .onChange(of: lock.isLocked) { _, _ in offerSetupIfNeeded() }
            .alert("Notifications are off", isPresented: $notificationsOff) {
                Button("Open Settings") {
                    if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }
                }
                Button("Not now", role: .cancel) {}
            } message: {
                Text("The reminder is saved, but it cannot ring until notifications are on for Summaverick.")
            }
        }
    }

    private var isFailed: Bool { if case .failed = pipeline.stage { return true } else { return false } }
    private var isPaused: Bool { if case .paused = pipeline.stage { return true } else { return false } }
    /// No profile saved yet (setup was closed with "Later", or never finished the routine step).
    private var needsProfile: Bool { profile == nil }
    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }

    // MARK: greeting

    private var greeting: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(greetingLine).font(.largeTitle.bold()).fixedSize(horizontal: false, vertical: true)
            Text(subtitle).font(.title3).foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private var greetingLine: String {
        let hour = Calendar.current.component(.hour, from: now)
        let name = profile?.name?.trimmingCharacters(in: .whitespaces) ?? ""
        switch (hour, name.isEmpty) {
        case (..<12, true): return String(localized: "Good morning")
        case (..<12, false): return String(localized: "Good morning, \(name)")
        case (..<17, true): return String(localized: "Good afternoon")
        case (..<17, false): return String(localized: "Good afternoon, \(name)")
        case (_, true): return String(localized: "Good evening")
        case (_, false): return String(localized: "Good evening, \(name)")
        }
    }

    private var subtitle: String {
        let weekday = now.formatted(.dateTime.weekday(.wide))
        let n = needCount
        let need = n == 0 ? String(localized: "Nothing needs you today") : String(localized: "\(n) things need you today")
        return "\(weekday) · \(need)"
    }

    /// Pending suggestions + reminders still to come today + open tasks due today or earlier.
    private var needCount: Int {
        let cal = Calendar.current
        let todayYMD = AssistantFormat.ymd(now)
        let rem = occurrences.filter { cal.isDateInToday($0.day) && $0.date >= now }.count
        let due = tasks.filter { t in t.dueDate.map { $0 <= todayYMD } ?? false }.count
        return suggestions.count + rem + due
    }

    private var occurrences: [ReminderOccurrence] {
        ReminderOccurrence.upcoming(reminders, now: now, calendar: .current)
    }

    // MARK: sections

    private func sectionTitle(_ title: LocalizedStringKey, count: Int? = nil) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(.title2.bold())
            if let count, count > 0 {
                Text(verbatim: "\(count)").font(.headline).foregroundStyle(.white)
                    .padding(.horizontal, 8).padding(.vertical, 2)
                    .background(Brand.record, in: Capsule())
            }
            Spacer()
        }
        .accessibilityAddTraits(.isHeader)
    }

    private func emptyLine(_ text: LocalizedStringKey) -> some View {
        Text(text).font(.body).foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
    }

    private var waitingSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("Waiting for you", count: suggestions.count)
            if suggestions.isEmpty {
                emptyLine("Nothing is waiting. New suggestions from your recordings appear here.")
            }
            ForEach(suggestions) { s in
                SuggestionCard(suggestion: s,
                               noteTitle: s.noteID.flatMap { titles[$0] },
                               isStale: staleIDs.contains(s.id),
                               isBusy: busyIDs.contains(s.id),
                               onAccept: { edited in beginAccept(s, edited: edited) },
                               onReject: { beginReject(s) })
            }
        }
    }

    private var remindersSection: some View {
        let items = occurrences
        let days = Dictionary(grouping: items, by: \.day).keys.sorted()
        return VStack(alignment: .leading, spacing: 12) {
            sectionTitle("Reminders")
            if items.isEmpty { emptyLine("No reminders today or in the next 3 days.") }
            ForEach(days, id: \.self) { day in
                VStack(alignment: .leading, spacing: 0) {
                    Text(dayLabel(day)).font(.headline).foregroundStyle(.secondary)
                        .padding(.horizontal, 14).padding(.top, 10).padding(.bottom, 4)
                    ForEach(items.filter { $0.day == day }) { o in reminderRow(o) }
                }
                .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func dayLabel(_ day: Date) -> String {
        let cal = Calendar.current
        if cal.isDateInToday(day) { return String(localized: "Today") }
        if cal.isDateInTomorrow(day) { return String(localized: "Tomorrow") }
        return day.formatted(.dateTime.weekday(.wide).day().month(.wide))
    }

    private func reminderRow(_ o: ReminderOccurrence) -> some View {
        let r = o.reminder
        let past = o.date < now
        let canOpen = r.noteID != nil && !r.fromDeleted
        return Button {
            if let id = r.noteID, !r.fromDeleted { router.openNote = (id, r.startSec) }
        } label: {
            HStack(alignment: .firstTextBaseline, spacing: 12) {
                Text(AssistantFormat.time(o.hhmm)).font(.headline.monospacedDigit())
                    .frame(minWidth: 72, alignment: .leading)
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        if r.medicineID != nil { Image(systemName: "pills.fill").foregroundStyle(.red).accessibilityHidden(true) }
                        Text(r.text).font(.body).multilineTextAlignment(.leading)
                    }
                    if r.fromDeleted {
                        Text("From a deleted recording").font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
                if canOpen { Image(systemName: "chevron.right").font(.footnote).foregroundStyle(.tertiary) }
            }
            .padding(.horizontal, 14).padding(.vertical, 10)
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .contentShape(Rectangle())
            .opacity(past ? 0.5 : 1)
        }
        .buttonStyle(.plain)
        .disabled(!canOpen)
    }

    private var tasksSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("To do")
            if tasks.isEmpty { emptyLine("No open to-dos.") }
            if !tasks.isEmpty {
                VStack(spacing: 0) {
                    ForEach(tasks) { t in
                        taskRow(t)
                        if t.id != tasks.last?.id { Divider().padding(.leading, 58) }
                    }
                }
                .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func taskRow(_ t: TaskItem) -> some View {
        let todayYMD = AssistantFormat.ymd(now)
        let overdue = t.dueDate.map { $0 < todayYMD } ?? false
        return HStack(alignment: .center, spacing: 6) {
            Button { Task { await setDone(t) } } label: {
                Image(systemName: "circle").font(.title2).foregroundStyle(Color.accentColor)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Mark as done: \(t.text)"))
            VStack(alignment: .leading, spacing: 2) {
                Text(t.text).font(.body)
                HStack(spacing: 6) {
                    if let d = t.dueDate {
                        Text("Due \(AssistantFormat.day(d))").foregroundStyle(overdue ? Color.red : Color.secondary)
                    }
                    if let who = t.who, !who.isEmpty { Text(verbatim: "· \(who)").foregroundStyle(.secondary) }
                }
                .font(.footnote)
            }
            Spacer(minLength: 0)
            if let id = t.noteID, !t.fromDeleted {
                Button { router.openNote = (id, t.startSec) } label: {
                    Image(systemName: "play.circle").font(.title3).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.accentColor)
                .accessibilityLabel(Text("Open the recording"))
            }
        }
        .padding(.horizontal, 8).padding(.vertical, 4)
    }

    private func insightCard(_ i: Insight) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("Insight", systemImage: "sparkles").font(.headline).foregroundStyle(Color.accentColor)
                Spacer()
                Button { Task { await dismissInsight(i) } } label: {
                    Image(systemName: "xmark").font(.headline).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .accessibilityLabel(Text("Dismiss"))
            }
            Text(i.text).font(.body).fixedSize(horizontal: false, vertical: true)
            if let id = i.noteID {
                Button { router.openNote = (id, nil) } label: {
                    Text("Open the recording").font(.subheadline.weight(.semibold)).frame(minHeight: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.accentColor)
            }
        }
        .padding(.leading, 16).padding(.trailing, 4).padding(.bottom, 8)
        .background(Color.accentColor.opacity(0.10), in: RoundedRectangle(cornerRadius: 16))
    }

    private var askBar: some View {
        Button { router.tab = .ask } label: {
            HStack(spacing: 10) {
                Image(systemName: "bubble.left.and.bubble.right").accessibilityHidden(true)
                Text("Ask Summaverick…")
                Spacer()
            }
            .font(.body)
            .foregroundStyle(.secondary)
            .padding(.horizontal, 16)
            .frame(maxWidth: .infinity, minHeight: 52)
            .background(Color(.secondarySystemBackground), in: Capsule())
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
    }

    private var setupProfileCard: some View {
        Button { showSetup = true } label: {
            HStack(spacing: 12) {
                Image(systemName: "person.crop.circle.badge.plus").font(.largeTitle).foregroundStyle(Color.accentColor)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Set up your profile").font(.headline).foregroundStyle(.primary)
                    Text("Your name, your day and your medicines help Summaverick time reminders for you.")
                        .font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.leading)
                }
                Spacer(minLength: 0)
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
            }
            .padding(16)
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    // MARK: data

    private func reload() async {
        let e = CoreEngine.shared
        let s = await e.suggestions(status: "pending")
        let r = await e.reminders(activeOnly: true)
        let t = await e.tasks(openOnly: true)
        let i = await e.latestInsight()
        let p = await e.profile()
        let notes = await e.notes()
        suggestions = s
        reminders = r
        tasks = t
        insight = i.flatMap { $0.dismissed ? nil : $0 }
        profile = p
        titles = Dictionary(notes.map { ($0.id, $0.title) }, uniquingKeysWith: { a, _ in a })
        staleIDs.formIntersection(s.map(\.id))
        now = Date()
        loaded = true
        offerSetupIfNeeded()
    }

    /// Opens the setup sheet once per launch while there is no profile, never over the lock screen or
    /// another sheet. The card stays on Today as the way back in.
    private func offerSetupIfNeeded() {
        guard loaded, profile == nil, !ProfileSetupView.shownThisLaunch, !lock.isLocked, !router.showSettings,
              router.openNote == nil, PresentationGate.shared.open.isEmpty else { return }
        ProfileSetupView.shownThisLaunch = true
        showSetup = true
    }

    /// After any change: reschedule notifications, then tell the rest of the app (which also reloads this view).
    private func changed() async {
        await ReminderScheduler.shared.resync()
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
    }

    /// Marks the card busy synchronously, so a second tap cannot queue a second accept.
    private func beginAccept(_ s: Suggestion, edited: SuggestionPayload?) {
        guard !busyIDs.contains(s.id) else { return }
        busyIDs.insert(s.id)
        Task { await accept(s, edited: edited) }
    }

    private func beginReject(_ s: Suggestion) {
        guard !busyIDs.contains(s.id) else { return }
        busyIDs.insert(s.id)
        Task { await reject(s) }
    }

    /// Only called through beginAccept, which has already marked the card busy.
    private func accept(_ s: Suggestion, edited: SuggestionPayload?) async {
        defer { busyIDs.remove(s.id) }
        let makesReminder = s.kind == .reminder || (s.kind == .profile && s.payload.medicine != nil)
        do {
            switch try await CoreEngine.shared.accept(s.id, edited: edited) {
            case .staleReflagged:
                staleIDs.insert(s.id)
            case .created, .applied:
                staleIDs.remove(s.id)
                if makesReminder { await ensureNotificationsAllowed() }
            }
        } catch {
            errorMessage = error.localizedDescription
        }
        await changed()
    }

    private func reject(_ s: Suggestion) async {
        defer { busyIDs.remove(s.id) }
        await CoreEngine.shared.reject(s.id)
        staleIDs.remove(s.id)
        await changed()
    }

    private func setDone(_ t: TaskItem) async {
        await CoreEngine.shared.setTaskDone(t.id, true)
        await changed()
    }

    private func dismissInsight(_ i: Insight) async {
        await CoreEngine.shared.dismissInsight(i.id)
        insight = nil
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
    }

    /// Asks once for notification permission; if it was turned off, says so, since a medicine
    /// reminder that cannot ring is worse than none.
    private func ensureNotificationsAllowed() async {
        let center = UNUserNotificationCenter.current()
        let status = await center.notificationSettings().authorizationStatus
        switch status {
        case .notDetermined:
            if !(await ReminderScheduler.shared.requestAuthorization()) { notificationsOff = true }
        case .denied:
            notificationsOff = true
        default:
            break
        }
    }
}
