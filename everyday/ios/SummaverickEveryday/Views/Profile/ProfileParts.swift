// Pieces shared by the setup sheet and Settings → Profile: languages, routine rows, the medicine form,
// the medicines list and a small toast. Everything here is the person's own edit, so it is saved directly
// (spec §7.9). A medicine saved here also gets a pending reminder card on Today (spec §7.8b).
import SwiftUI

// MARK: - languages and routine

enum ProfileLanguage {
    /// Preferred language codes, with the name in its own script.
    static let all: [(code: String, native: String, name: LocalizedStringKey)] = [
        ("en", "English", "English"), ("te", "తెలుగు", "Telugu"), ("hi", "हिन्दी", "Hindi"),
        ("ta", "தமிழ்", "Tamil"), ("kn", "ಕನ್ನಡ", "Kannada"), ("ml", "മലയാളം", "Malayalam"),
    ]
}

enum RoutineSlot: String, CaseIterable, Identifiable {
    case wake, breakfast, lunch, dinner, bed
    var id: String { rawValue }

    var label: LocalizedStringKey {
        switch self {
        case .wake: return "Wake up"
        case .breakfast: return "Breakfast"
        case .lunch: return "Lunch"
        case .dinner: return "Dinner"
        case .bed: return "Bedtime"
        }
    }

    var icon: String {
        switch self {
        case .wake: return "sunrise"
        case .breakfast: return "cup.and.saucer"
        case .lunch: return "fork.knife"
        case .dinner: return "moon"
        case .bed: return "bed.double"
        }
    }

    /// Sensible defaults offered in setup (controller ruling).
    var defaultTime: String {
        switch self {
        case .wake: return "06:30"
        case .breakfast: return "08:00"
        case .lunch: return "13:00"
        case .dinner: return "20:00"
        case .bed: return "22:00"
        }
    }
}

extension Routine {
    static let defaults = Routine(wake: "06:30", breakfast: "08:00", lunch: "13:00", dinner: "20:00", bed: "22:00")

    subscript(slot: RoutineSlot) -> String? {
        get {
            switch slot {
            case .wake: return wake
            case .breakfast: return breakfast
            case .lunch: return lunch
            case .dinner: return dinner
            case .bed: return bed
            }
        }
        set {
            switch slot {
            case .wake: wake = newValue
            case .breakfast: breakfast = newValue
            case .lunch: lunch = newValue
            case .dinner: dinner = newValue
            case .bed: bed = newValue
            }
        }
    }
}

/// One routine time: a time picker when set, or a "Set time" button when not. Never invents a value
/// unless the person taps (it then starts at the slot's default).
struct RoutineRow: View {
    let slot: RoutineSlot
    @Binding var value: String?

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: slot.icon).foregroundStyle(Theme.accent).frame(width: 28).accessibilityHidden(true)
            if let v = value, ReminderScheduler.clock(v) != nil {
                DatePicker(selection: Binding(get: { HHMM.date(v) }, set: { value = HHMM.string($0) }),
                           displayedComponents: .hourAndMinute) { Text(slot.label) }
                Button { value = nil } label: {
                    Image(systemName: "xmark.circle.fill").font(.title3).foregroundStyle(Theme.secondary)
                        .frame(minWidth: 44, minHeight: 44)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(Text("Clear \(Text(slot.label))"))
            } else {
                Text(slot.label)
                Spacer()
                Button { value = slot.defaultTime } label: { Text("Set time").frame(minHeight: 44) }
                    .buttonStyle(.borderless)
                    .accessibilityLabel(Text("Set time for \(Text(slot.label))"))
            }
        }
        .frame(minHeight: 44)
    }
}

enum HHMM {
    static func date(_ s: String) -> Date {
        guard let (h, m) = ReminderScheduler.clock(s),
              let d = Calendar.current.date(bySettingHour: h, minute: m, second: 0, of: Date()) else { return Date() }
        return d
    }
    static func string(_ d: Date) -> String {
        let c = Calendar.current.dateComponents([.hour, .minute], from: d)
        return String(format: "%02d:%02d", c.hour ?? 0, c.minute ?? 0)
    }
}

// MARK: - toast

struct ToastModifier: ViewModifier {
    @Binding var message: String?

    func body(content: Content) -> some View {
        content.overlay(alignment: .bottom) {
            if let message {
                Text(verbatim: message)
                    .font(.callout.weight(.medium))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 16).padding(.vertical, 12)
                    .background(Theme.chip, in: Capsule())
                    .padding(.bottom, 24).padding(.horizontal)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                    .onTapGesture { withAnimation { self.message = nil } }
            }
        }
        .onChange(of: message) { _, new in
            guard let new else { return }
            UIAccessibility.post(notification: .announcement, argument: new)
            Task {
                try? await Task.sleep(for: .seconds(4))
                if message == new { withAnimation { message = nil } }
            }
        }
    }
}

extension View {
    func toast(_ message: Binding<String?>) -> some View { modifier(ToastModifier(message: message)) }
}

// MARK: - saving

enum ProfileActions {
    static let savedMedicineToast = String(localized: "Saved. Check Today to turn on the reminder.")

    /// After a change to medicines or the profile: reschedule notifications, then tell the app.
    static func changed() async {
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        await ReminderScheduler.shared.resync()
    }
}

// MARK: - medicine form

/// Add or edit one medicine: name (required), dose, when, times, repeat (must be picked), until date.
struct MedicineForm: View {
    let original: Medicine?
    /// Called after a successful save, with the medicine id.
    let onSaved: (Int64) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var dose = ""
    @State private var when = ""
    @State private var times: [String] = []
    /// Times that came from the routine and have not been changed by the person.
    @State private var fromRoutine: Set<String> = []
    /// Once the person adds, changes or removes a time, "when" no longer refills the times.
    @State private var timesTouched = false
    @State private var repeatRule: String?
    @State private var untilDate: String?
    @State private var saving = false
    @State private var errorShown = false
    @State private var started = false

    private static let repeatOptions = ["daily", "twice_daily", "weekly", "once"]

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Medicine name", text: $name, prompt: Theme.prompt("Medicine name"))
                        .textInputAutocapitalization(.words)
                        .frame(minHeight: 44)
                    TextField("Dose, e.g. 5 mg", text: $dose, prompt: Theme.prompt("Dose, e.g. 5 mg"))
                        .frame(minHeight: 44)
                    TextField("When, e.g. after dinner", text: $when, prompt: Theme.prompt("When, e.g. after dinner"), axis: .vertical)
                        .frame(minHeight: 44)
                } header: { Text("Medicine") } footer: {
                    if name.trimmingCharacters(in: .whitespaces).isEmpty {
                        Text("A name is needed to save.")
                    }
                }
                .listRowBackground(Theme.formRow)

                Section {
                    ForEach(times.indices, id: \.self) { i in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                DatePicker("Time", selection: timeBinding(i), displayedComponents: .hourAndMinute)
                                Button(role: .destructive) { removeTime(i) } label: {
                                    Image(systemName: "minus.circle.fill").font(.title3)
                                        .frame(minWidth: 44, minHeight: 44)
                                }
                                .buttonStyle(.borderless)
                                .accessibilityLabel(Text("Remove this time"))
                            }
                            if fromRoutine.contains(times[i]) {
                                Text("From your routine: \(AssistantFormat.time(times[i])) — change if needed")
                                    .font(.footnote).foregroundStyle(Theme.secondary)
                            }
                        }
                        .frame(minHeight: 44)
                    }
                    Button(action: addTime) {
                        Label("Add a time", systemImage: "plus.circle").frame(minHeight: 44)
                    }
                    Picker(selection: $repeatRule) {
                        Text("Choose").tag(String?.none)
                        ForEach(Self.repeatOptions, id: \.self) { r in
                            Text(AssistantFormat.repeatLabel(r) ?? r).tag(String?.some(r))
                        }
                    } label: { Text("Repeats") }
                    .frame(minHeight: 44)
                    Toggle("Has an end date", isOn: hasUntil).tint(Theme.toggleOn).frame(minHeight: 44)
                    if let u = untilDate {
                        DatePicker("Until", selection: Binding(
                            get: { ReminderScheduler.day(u, .current) ?? Date() },
                            set: { untilDate = AssistantFormat.ymd($0) }), displayedComponents: .date)
                            .frame(minHeight: 44)
                    }
                } header: { Text("When to take it") } footer: {
                    VStack(alignment: .leading, spacing: 4) {
                        if times.isEmpty {
                            Text("No time yet. Add one, or the reminder on Today will ask you for it.")
                        }
                        if repeatRule == nil {
                            Text("Choose how often it repeats before saving.").foregroundStyle(Theme.warning)
                        }
                        if twiceDailyShort {
                            Text("Twice a day needs two times.").foregroundStyle(Theme.warning)
                        }
                    }
                }
                .listRowBackground(Theme.formRow)
            }
            .themedForm()
            .navigationTitle(original == nil ? Text("Add a medicine") : Text("Edit medicine"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }.disabled(!canSave || saving)
                }
            }
            .onChange(of: when) { _, new in refill(new) }
            .alert("I couldn't save this. Please try again.", isPresented: $errorShown) {
                Button("OK", role: .cancel) {}
            }
            .onAppear(perform: start)
        }
        .onAppear { PresentationGate.shared.set("medicineForm", true) }
        .onDisappear { PresentationGate.shared.set("medicineForm", false) }
    }

    private var canSave: Bool {
        !name.trimmingCharacters(in: .whitespaces).isEmpty && repeatRule != nil && !twiceDailyShort
    }

    private var twiceDailyShort: Bool { AssistantFormat.twiceDailyNeedsTwoTimes(repeatRule, times) }

    private var hasUntil: Binding<Bool> {
        Binding(get: { untilDate != nil },
                set: { on in untilDate = on ? (untilDate ?? AssistantFormat.ymd(Date())) : nil })
    }

    private func start() {
        guard !started else { return }
        started = true
        guard let m = original else { return }
        name = m.name; dose = m.dose ?? ""; when = m.whenText ?? ""
        times = m.times; timesTouched = true
        repeatRule = m.repeat.flatMap { Self.repeatOptions.contains($0) ? $0 : nil }
        untilDate = (m.untilDate?.isEmpty ?? true) ? nil : m.untilDate
    }

    /// Times from the saved routine for the words of "when" (the core's keyword table). Never invents
    /// a time: no keyword, no time. Stops once the person has touched the times.
    private func refill(_ text: String) {
        guard started, !timesTouched else { return }
        Task {
            let t = await CoreEngine.shared.resolveTimes(text)
            guard text == when, !timesTouched else { return }   // a newer edit wins
            times = t
            fromRoutine = Set(t)
        }
    }

    private func timeBinding(_ i: Int) -> Binding<Date> {
        Binding(get: { i < times.count ? HHMM.date(times[i]) : Date() },
                set: { d in
                    guard i < times.count else { return }
                    let new = HHMM.string(d)
                    if new != times[i] { fromRoutine.remove(times[i]); times[i] = new; timesTouched = true }
                })
    }

    private func removeTime(_ i: Int) {
        guard i < times.count else { return }
        fromRoutine.remove(times[i])
        times.remove(at: i)
        timesTouched = true
    }

    private func addTime() {
        // A new row starts at the next whole hour; the person sets it.
        let now = Calendar.current.component(.hour, from: Date())
        guard let t = (1...24).map({ String(format: "%02d:00", (now + $0) % 24) }).first(where: { !times.contains($0) })
        else { return }
        times.append(t)
        timesTouched = true
    }

    private func save() {
        guard canSave, !saving else { return }
        saving = true
        var m = original ?? Medicine(name: "")
        m.name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        m.dose = dose.trimmingCharacters(in: .whitespacesAndNewlines)
        m.whenText = when.trimmingCharacters(in: .whitespacesAndNewlines)
        m.times = Array(Set(times)).sorted()
        m.repeat = repeatRule
        m.untilDate = untilDate ?? ""   // "" clears an end date the medicine had
        m.createdAt = nil
        let toSave = m
        Task {
            do {
                let id = try await CoreEngine.shared.saveMedicine(toSave)
                await ProfileActions.changed()
                saving = false
                onSaved(id)
                dismiss()
            } catch {
                saving = false
                errorShown = true
            }
        }
    }
}

// MARK: - medicines list

/// Rows for the person's medicines, with Edit and Delete (confirmed). Used in setup step 4 and Profile.
struct MedicinesList: View {
    let medicines: [Medicine]
    let onEdit: (Medicine) -> Void
    let onDelete: (Medicine) -> Void

    var body: some View {
        ForEach(medicines) { m in
            HStack(alignment: .center, spacing: 8) {
                Button { onEdit(m) } label: {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(verbatim: [m.name, m.dose ?? ""].filter { !$0.isEmpty }.joined(separator: " "))
                            .font(.headline).foregroundStyle(Theme.text)
                        if let line = detail(m) {
                            Text(verbatim: line).font(.subheadline).foregroundStyle(Theme.secondary)
                        }
                    }
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityHint(Text("Edit this medicine"))
                Button(role: .destructive) { onDelete(m) } label: {
                    Image(systemName: "trash").font(.title3).frame(minWidth: 44, minHeight: 44)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(Text("Delete \(m.name)"))
            }
            .swipeActions {
                Button("Delete", role: .destructive) { onDelete(m) }
            }
        }
    }

    private func detail(_ m: Medicine) -> String? {
        var parts: [String] = []
        if let w = m.whenText, !w.isEmpty { parts.append(w) }
        if !m.times.isEmpty { parts.append(m.times.map(AssistantFormat.time).formatted(.list(type: .and))) }
        if let r = AssistantFormat.repeatLabel(m.repeat) { parts.append(r) }
        if let u = m.untilDate, !u.isEmpty { parts.append(String(localized: "until \(AssistantFormat.day(u))")) }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }
}

/// Which medicine form is open: a new one, or editing one.
struct MedicineEditing: Identifiable {
    let medicine: Medicine?
    var id: Int64 { medicine?.id ?? 0 }
}

extension View {
    /// The confirmation shown before a medicine is deleted, and the delete itself.
    /// onDeleted(false): the delete failed; the caller shows an error.
    func medicineDeleteConfirmation(_ target: Binding<Medicine?>, onDeleted: @escaping (Bool) -> Void) -> some View {
        confirmationDialog(
            Text("Delete \(target.wrappedValue?.name ?? "")?"),
            isPresented: Binding(get: { target.wrappedValue != nil }, set: { if !$0 { target.wrappedValue = nil } }),
            titleVisibility: .visible,
            presenting: target.wrappedValue
        ) { m in
            Button("Delete medicine", role: .destructive) {
                Task {
                    let ok = await CoreEngine.shared.deleteMedicine(m.id)
                    await ProfileActions.changed()
                    onDeleted(ok)
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("Its reminders will stop and be removed.")
        }
        .onChange(of: target.wrappedValue != nil) { _, open in PresentationGate.shared.set("medicineDelete", open) }
    }
}
