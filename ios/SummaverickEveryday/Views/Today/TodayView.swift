// Today (spec §6.1): greeting, suggestions waiting for a Yes, memories to check, reminders for the next few days,
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
    /// Pending suggestions, without those shown inside a memory's card in "To check".
    @State private var suggestions: [Suggestion] = []
    /// Recordings whose people, decisions and open questions wait to be checked before they reach the map.
    @State private var relationReviews: [RelationReview] = []
    /// Memories waiting for the person to check, newest recording first.
    @State private var queue: [ReviewItem] = []
    @State private var showAllToCheck = false
    /// The last read of the queue failed: the cards from before stay, with a line to try again.
    @State private var queueFailed = false
    @State private var reminders: [Reminder] = []
    @State private var tasks: [TaskItem] = []
    /// Coming meetings from recordings, with what is open from them (MeetingBrief).
    @State private var briefs: [BriefData] = []
    @State private var insight: Insight?
    @State private var profile: Profile?
    @State private var titles: [Int64: String] = [:]
    @State private var staleIDs: Set<Int64> = []
    @State private var busyIDs: Set<Int64> = []
    @State private var errorMessage: String?
    @State private var notificationsOff = false
    @State private var now = Date()
    @State private var showSetup = false
    @State private var medicineCount = 0
    @AppStorage(SetupFlow.dayCardDismissedKey) private var dayCardDismissed = false
    @AppStorage("ai.unsupportedShown") private var unsupportedShown = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 24) {
                    greeting
                    if pipeline.isBusy || isFailed || isPaused {
                        StatusBanner(stage: pipeline.stage, line: pipeline.progressLine).clipShape(RoundedRectangle(cornerRadius: 10))
                    }
                    if models.status == .unsupported {
                        if !unsupportedShown { AIUnsupportedCard { unsupportedShown = true } }
                    } else if !models.isReady {
                        AIDownloadCard()
                    }
                    if loaded && needsProfile { setupProfileCard }
                    ForEach(briefs) { b in
                        BeforeMeetingCard(brief: b, selfName: profile?.name, onDone: { t in Task { await setDone(t) } })
                    }
                    waitingSection
                    if !queue.isEmpty || queueFailed { toCheckSection }
                    remindersSection
                    tasksSection
                    if let insight { insightCard(insight) }
                    if models.isReady { askBar }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 32)
            }
            .themedPage()
            .alert("I couldn't save this. Please try again.", isPresented: errorShown) {
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
            .sheet(isPresented: $showSetup, onDismiss: { Task { await reload() } }) {
                // With a profile (setup saved the name and language), it starts at the day's times.
                ProfileSetupView(startAtDay: profile != nil)
            }
            .onChange(of: lock.isLocked) { _, _ in offerSetupIfNeeded() }
            .onChange(of: SetupState.shared.progress) { _, _ in offerSetupIfNeeded() }
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
    /// No profile saved yet, or one without a routine or medicines (setup saves only the name and language) that the
    /// person has not put off with Not now.
    private var needsProfile: Bool {
        SetupFlow.showsDayCard(hasProfile: profile != nil, hasRoutine: profile?.routine != nil,
                               medicineCount: medicineCount, dismissed: dayCardDismissed)
    }
    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }

    // MARK: greeting

    private var greeting: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(greetingLine).font(.largeTitle.bold()).fixedSize(horizontal: false, vertical: true)
            Text(subtitle).font(.title3).foregroundStyle(Theme.secondary)
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

    /// Pending suggestions + memories to check + reminders still to come today + open tasks due today or earlier.
    private var needCount: Int {
        let cal = Calendar.current
        let todayYMD = AssistantFormat.ymd(now)
        let rem = occurrences.filter { cal.isDateInToday($0.day) && $0.date >= now }.count
        let due = tasks.filter { t in t.dueDate.map { $0 <= todayYMD } ?? false }.count
        return suggestions.count + relationReviews.count + queue.count + rem + due
    }

    private var occurrences: [ReminderOccurrence] {
        ReminderOccurrence.upcoming(reminders, now: now, calendar: .current)
    }

    // MARK: sections

    private func sectionTitle(_ title: LocalizedStringKey, count: Int? = nil) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(.title2.bold())
            if let count, count > 0 {
                Text(verbatim: "\(count)").font(.headline).foregroundStyle(Theme.badgeLabel)
                    .padding(.horizontal, 8).padding(.vertical, 2)
                    .background(Theme.badgeFill, in: Capsule())
            }
            Spacer()
        }
        .accessibilityAddTraits(.isHeader)
    }

    private func emptyLine(_ text: LocalizedStringKey) -> some View {
        Text(text).font(.body).foregroundStyle(Theme.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14)
            .background(Theme.card, in: RoundedRectangle(cornerRadius: 12))
    }

    private var waitingSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            sectionTitle("Waiting for you", count: suggestions.count + relationReviews.count)
            if suggestions.isEmpty && relationReviews.isEmpty {
                emptyLine("All clear. When I spot something in your recordings, it shows up here.")
            }
            ForEach(suggestions) { s in
                SuggestionCard(suggestion: s,
                               noteTitle: s.noteID.flatMap { titles[$0] },
                               isStale: staleIDs.contains(s.id),
                               isBusy: busyIDs.contains(s.id),
                               onAccept: { edited in beginAccept(s, edited: edited) },
                               onReject: { beginReject(s) })
            }
            ForEach(relationReviews) { RelationsReviewCard(review: $0).id($0.noteID) }
        }
    }

    /// Cards shown before "Show all": the next one moves up as each is checked.
    private static let toCheckCollapsed = 3

    private var toCheckSection: some View {
        let shown = showAllToCheck ? queue : Array(queue.prefix(Self.toCheckCollapsed))
        return VStack(alignment: .leading, spacing: 12) {
            sectionTitle("To check", count: queue.count)
            if queueFailed {
                HStack(spacing: 12) {
                    Label("I couldn't open this.", systemImage: "exclamationmark.triangle.fill")
                        .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
                    Spacer(minLength: 0)
                    Button("Try again") { Task { await reload() } }
                        .buttonStyle(.pill).frame(minHeight: 44)
                }
            }
            ForEach(shown) { item in
                MemoryReviewCard(item: item,
                                 suggestionBusy: item.suggestion.map { busyIDs.contains($0.id) } ?? false,
                                 suggestionStale: item.suggestion.map { staleIDs.contains($0.id) } ?? false,
                                 onAcceptSuggestion: { s, edited in await acceptLinked(s, edited: edited) },
                                 onRejectSuggestion: { s in await rejectLinked(s) },
                                 onPlay: { id, sec in router.openNote = (id, sec) })
                    .transition(.asymmetric(insertion: .opacity, removal: .move(edge: .leading).combined(with: .opacity)))
            }
            if queue.count > Self.toCheckCollapsed {
                Button { withAnimation { showAllToCheck.toggle() } } label: {
                    Text(showAllToCheck ? "Show fewer" : "Show all \(queue.count)")
                        .font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pill)
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
                    Text(dayLabel(day)).font(.headline).foregroundStyle(Theme.secondary)
                        .padding(.horizontal, 14).padding(.top, 10).padding(.bottom, 4)
                    ForEach(items.filter { $0.day == day }) { o in reminderRow(o) }
                }
                .background(Theme.card, in: RoundedRectangle(cornerRadius: 12))
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
                        if r.medicineID != nil { Image(systemName: "pills.fill").foregroundStyle(Theme.medicineIcon).accessibilityHidden(true) }
                        Text(r.text).font(.body).multilineTextAlignment(.leading)
                    }
                    if r.fromDeleted {
                        Text("From a deleted recording").font(.footnote).foregroundStyle(Theme.secondary)
                    }
                }
                Spacer(minLength: 0)
                if canOpen { Image(systemName: "chevron.right").font(.footnote).foregroundStyle(Theme.secondary) }
            }
            .padding(.horizontal, 14).padding(.vertical, 10)
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .contentShape(Rectangle())
            .foregroundStyle(past ? Theme.secondary : Theme.text)
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
                .background(Theme.card, in: RoundedRectangle(cornerRadius: 12))
            }
        }
    }

    private func taskRow(_ t: TaskItem) -> some View {
        let todayYMD = AssistantFormat.ymd(now)
        let overdue = t.dueDate.map { $0 < todayYMD } ?? false
        return HStack(alignment: .center, spacing: 6) {
            Button { Task { await setDone(t) } } label: {
                Image(systemName: "circle").font(.title2).foregroundStyle(Theme.accent)
                    .frame(width: 44, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Mark as done: \(t.text)"))
            VStack(alignment: .leading, spacing: 2) {
                Text(t.text).font(.body)
                HStack(spacing: 6) {
                    if let d = t.dueDate {
                        Text("Due \(AssistantFormat.day(d))").foregroundStyle(overdue ? Theme.warning : Theme.secondary)
                    }
                    if let who = t.who, !who.isEmpty { Text(verbatim: "· \(who)").foregroundStyle(Theme.secondary) }
                }
                .font(.footnote)
            }
            Spacer(minLength: 0)
            if let id = t.noteID, !t.fromDeleted {
                Button { router.openNote = (id, t.startSec) } label: {
                    Image(systemName: "play.circle").font(.title3).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.accent)
                .accessibilityLabel(Text("Open the recording"))
            }
        }
        .padding(.horizontal, 8).padding(.vertical, 4)
    }

    private func insightCard(_ i: Insight) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("Something I noticed", systemImage: "sparkles").font(.headline).foregroundStyle(Theme.accent)
                Spacer()
                Button { Task { await dismissInsight(i) } } label: {
                    Image(systemName: "xmark").font(.headline).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.secondary)
                .accessibilityLabel(Text("Dismiss"))
            }
            Text(i.text).font(.body).fixedSize(horizontal: false, vertical: true)
            if let id = i.noteID {
                Button { router.openNote = (id, nil) } label: {
                    Text("Open the recording").font(.subheadline.weight(.semibold)).underline().frame(minHeight: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.accent)
            }
        }
        .padding(.leading, 16).padding(.trailing, 4).padding(.bottom, 8)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
    }

    private var askBar: some View {
        Button { router.tab = .ask } label: {
            HStack(spacing: 10) {
                Image(systemName: "bubble.left.and.bubble.right").accessibilityHidden(true)
                Text("Ask Summaverick…")
                Spacer()
            }
            .font(.body)
            .foregroundStyle(Theme.secondary)
            .padding(.horizontal, 16)
            .frame(maxWidth: .infinity, minHeight: 52)
            .background(Theme.card, in: Capsule())
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
    }

    private var setupProfileCard: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button { showSetup = true } label: {
                HStack(spacing: 12) {
                    Image(systemName: "person.crop.circle.badge.plus").font(.largeTitle).foregroundStyle(Theme.accent)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 4) {
                        if profile == nil {
                            Text("Set up your profile").font(.headline).foregroundStyle(Theme.text)
                            Text("Your name, your day and your medicines help me time your reminders.")
                                .font(.subheadline).foregroundStyle(Theme.secondary).multilineTextAlignment(.leading)
                        } else {
                            Text("Add your day and your medicines").font(.headline).foregroundStyle(Theme.text)
                            Text("Your mealtimes and medicines help me time your reminders.")
                                .font(.subheadline).foregroundStyle(Theme.secondary).multilineTextAlignment(.leading)
                        }
                    }
                    Spacer(minLength: 0)
                    Image(systemName: "chevron.right").foregroundStyle(Theme.secondary)
                }
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if profile != nil {
                Button("Not now") { dayCardDismissed = true }
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(Theme.link)
                    .frame(minHeight: 44)
                    .padding(.leading, 50)
            }
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
    }

    // MARK: data

    private func reload() async {
        let e = CoreEngine.shared
        let s = await e.suggestions(status: "pending")
        await e.backfillRelationReviewsOnce()
        let rv = await e.relationReviews()
        let q = await e.reviewQueue()
        let r = await e.reminders(activeOnly: true)
        let t = await e.tasks(openOnly: true)
        let i = await e.latestInsight()
        let p = await e.profile()
        let m = await e.medicines()
        let notes = await e.notes()
        // A failed read keeps the cards from before (and so the suggestions they hold stay out of the list).
        queueFailed = q == nil
        if let q { withAnimation { queue = q } }
        let inCards = ReviewItem.suggestionIDsShown(in: queue)
        suggestions = s.filter { !inCards.contains($0.id) }
        relationReviews = rv
        reminders = r
        tasks = t
        briefs = await Self.loadBriefs(reminders: r, tasks: t)
        insight = i.flatMap { $0.dismissed ? nil : $0 }
        profile = p
        medicineCount = m.count
        titles = Dictionary(notes.map { ($0.id, $0.title) }, uniquingKeysWith: { a, _ in a })
        staleIDs.formIntersection(s.map(\.id))
        now = Date()
        loaded = true
        offerSetupIfNeeded()
    }

    /// Opens the setup sheet once per launch while there is no profile, never over the lock screen or
    /// another sheet. The card stays on Today as the way back in.
    private func offerSetupIfNeeded() {
        // Never during, or before the decision on, the first-time setup.
        guard SetupState.shared.progress == .done else { return }
        guard loaded, SetupFlow.offersProfileSheet(hasProfile: profile != nil), !ProfileSetupView.shownThisLaunch, !lock.isLocked, !router.showSettings,
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
        do {
            _ = try await acceptInCore(s, edited: edited)
        } catch {
            errorMessage = error.localizedDescription
        }
        await changed()
    }

    /// true when accepted; false when it came back re-flagged (the card then says it changed).
    private func acceptInCore(_ s: Suggestion, edited: SuggestionPayload?) async throws -> Bool {
        let makesReminder = s.kind == .reminder || (s.kind == .profile && s.payload.medicine != nil)
        switch try await CoreEngine.shared.accept(s.id, edited: edited) {
        case .staleReflagged:
            staleIDs.insert(s.id)
            return false
        case .created, .applied:
            staleIDs.remove(s.id)
            if makesReminder { await ensureNotificationsAllowed() }
            return true
        }
    }

    private func reject(_ s: Suggestion) async {
        defer { busyIDs.remove(s.id) }
        await CoreEngine.shared.reject(s.id)
        staleIDs.remove(s.id)
        await changed()
    }

    /// Yes on a memory card's linked row. The memory card shows a failure inline and tells the app when done;
    /// notifications are rescheduled here.
    private func acceptLinked(_ s: Suggestion, edited: SuggestionPayload?) async -> SuggestionStepResult {
        guard !busyIDs.contains(s.id) else { return .failed }
        busyIDs.insert(s.id)
        defer { busyIDs.remove(s.id) }
        let result: SuggestionStepResult
        do { result = try await acceptInCore(s, edited: edited) ? .done : .stale } catch { result = .failed }
        if result == .done { await ReminderScheduler.shared.resync() }
        return result
    }

    /// No on a memory card's linked row, or Wrong on its memory.
    private func rejectLinked(_ s: Suggestion) async -> Bool {
        guard !busyIDs.contains(s.id) else { return false }
        busyIDs.insert(s.id)
        defer { busyIDs.remove(s.id) }
        let ok = await CoreEngine.shared.reject(s.id)
        if ok { staleIDs.remove(s.id) }
        return ok
    }

    /// The meetings coming up in the next two days that come from a recording with something still open.
    private static func loadBriefs(reminders: [Reminder], tasks: [TaskItem]) async -> [BriefData] {
        let now = Date()
        let occ = ReminderOccurrence.upcoming(reminders, now: now, calendar: .current, days: 3)
            .map { (noteID: $0.reminder.fromDeleted ? nil : $0.reminder.noteID, date: $0.date, text: $0.reminder.text) }
        var out: [BriefData] = []
        for d in MeetingBrief.due(occ, now: now) {
            let mems = await CoreEngine.shared.memories(MemoryFilter(noteID: d.noteID, types: ["decision", "question"]))
            let b = BriefData(noteID: d.noteID, date: d.date, title: d.text,
                              tasks: tasks.filter { $0.noteID == d.noteID && !$0.done },
                              decisions: mems.filter { $0.type == "decision" },
                              questions: mems.filter { $0.type == "question" && !$0.done })
            if !b.tasks.isEmpty || !b.decisions.isEmpty || !b.questions.isEmpty { out.append(b) }
        }
        return out
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


/// One coming meeting and what is open from the recording it was set in.
struct BriefData: Identifiable {
    let noteID: Int64
    let date: Date
    let title: String
    let tasks: [TaskItem]
    let decisions: [BrainMemory]
    let questions: [BrainMemory]
    var id: Int64 { noteID }
}

/// "Before your meeting": the actions with who and when (tick to mark done), what was decided, what is still open.
struct BeforeMeetingCard: View {
    let brief: BriefData
    let selfName: String?
    let onDone: (TaskItem) -> Void
    @Environment(Router.self) private var router

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "person.3.sequence.fill").font(.title3).foregroundStyle(Theme.accent).frame(width: 28)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Before \(brief.title)").font(.headline)
                    Text(brief.date, format: .dateTime.weekday(.wide).hour().minute())
                        .font(.subheadline).foregroundStyle(Theme.secondary)
                }
                Spacer(minLength: 0)
            }
            if !brief.tasks.isEmpty {
                group("Open actions") {
                    ForEach(brief.tasks) { t in
                        HStack(alignment: .top, spacing: 10) {
                            Button { onDone(t) } label: {
                                Image(systemName: "circle").font(.title3).frame(width: 44, height: 44)
                            }
                            .buttonStyle(.plain).foregroundStyle(Theme.secondary)
                            .accessibilityLabel(Text("Mark done"))
                            VStack(alignment: .leading, spacing: 2) {
                                Text(verbatim: t.text).font(.body).fixedSize(horizontal: false, vertical: true)
                                Text(verbatim: actionLine(t)).font(.footnote).foregroundStyle(Theme.secondary)
                            }
                            .padding(.top, 10)
                        }
                    }
                }
            }
            if !brief.decisions.isEmpty {
                group("Decided") { ForEach(brief.decisions) { line($0.text) } }
            }
            if !brief.questions.isEmpty {
                group("Still open") { ForEach(brief.questions) { line($0.text) } }
            }
            Button { router.openNote = (brief.noteID, nil) } label: {
                Label("Open the meeting", systemImage: "play.circle").font(.subheadline.weight(.medium))
                    .frame(minHeight: 44)
            }
            .buttonStyle(.plain).foregroundStyle(Theme.accent)
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
    }

    private func actionLine(_ t: TaskItem) -> String {
        var parts: [String] = []
        if let who = MeetingBrief.owner(t.who, selfName: selfName) { parts.append(who) }
        if let d = t.dueDate, let day = ReminderScheduler.day(d, .current) {
            parts.append(String(localized: "by \(day.formatted(.dateTime.weekday(.wide).day().month()))"))
        }
        return parts.isEmpty ? String(localized: "Nobody named") : parts.joined(separator: " · ")
    }

    private func group<Content: View>(_ title: LocalizedStringKey, @ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.secondary).accessibilityAddTraits(.isHeader)
            content()
        }
    }

    private func line(_ text: String) -> some View {
        Text(verbatim: "• " + text).font(.body).fixedSize(horizontal: false, vertical: true)
    }
}
