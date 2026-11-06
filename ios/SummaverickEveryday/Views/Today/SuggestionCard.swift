// A suggestion from a recording, shown as a card with Yes / Edit / No.
// This is the safety surface: a medicine reminder confirmed here becomes a real notification, so a
// "Please check" flag is a loud yellow banner, flagged fields are highlighted, the doctor's exact words
// are always shown, and a flagged card asks once more before Yes.
import SwiftUI

// MARK: - formatting helpers

enum AssistantFormat {
    /// "08:30" → the phone's own time style ("8:30 AM"). Unreadable values are shown as they are.
    static func time(_ hhmm: String) -> String {
        guard let (h, m) = ReminderScheduler.clock(hhmm),
              let d = Calendar.current.date(bySettingHour: h, minute: m, second: 0, of: Date()) else { return hhmm }
        return d.formatted(date: .omitted, time: .shortened)
    }

    /// "2026-11-07" → "Saturday 7 November" in the phone's style.
    static func day(_ ymd: String) -> String {
        guard let d = ReminderScheduler.day(ymd, .current) else { return ymd }
        return d.formatted(.dateTime.weekday(.wide).day().month(.wide))
    }

    static func ymd(_ date: Date, _ calendar: Calendar = .current) -> String {
        let c = calendar.dateComponents([.year, .month, .day], from: date)
        return String(format: "%04d-%02d-%02d", c.year ?? 0, c.month ?? 0, c.day ?? 0)
    }

    static func mmss(_ seconds: Int) -> String {
        let s = max(0, seconds)
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    static func repeatLabel(_ r: String?) -> String? {
        guard let r, !r.isEmpty else { return nil }
        switch r {
        case "once": return String(localized: "Once")
        case "daily": return String(localized: "Every day")
        case "twice_daily": return String(localized: "Twice a day")
        case "weekly": return String(localized: "Every week")
        default: return r
        }
    }

    static func changeLabel(_ c: String?) -> String? {
        guard let c, !c.isEmpty else { return nil }
        switch c {
        case "add_medicine": return String(localized: "Add a medicine")
        case "update_medicine": return String(localized: "Change a medicine")
        case "stop_medicine": return String(localized: "Stop a medicine")
        case "routine": return String(localized: "Change your routine")
        default: return c.replacingOccurrences(of: "_", with: " ")
        }
    }

    /// A check field from the core, in plain words.
    static func fieldName(_ key: String) -> String {
        switch key {
        case "dose": return String(localized: "dose")
        case "times": return String(localized: "time")
        case "name": return String(localized: "medicine name")
        case "quote": return String(localized: "the recorded words")
        case "text": return String(localized: "the wording")
        case "due_date", "start_date", "until_date": return String(localized: "date")
        case "routine": return String(localized: "routine time")
        case "when_text": return String(localized: "when to take it")
        case "frequency": return String(localized: "how often")
        case "duration": return String(localized: "how long")
        default: return key.replacingOccurrences(of: "_", with: " ")
        }
    }

    /// "dose and time", without repeats, in the phone's language.
    static func fieldList(_ keys: [String]) -> String {
        var seen = Set<String>()
        let names = keys.map(fieldName).filter { seen.insert($0).inserted }
        return names.formatted(.list(type: .and))
    }
}

extension Suggestion {
    var isFlagged: Bool { checkStatus == "check" }
    var isMedicine: Bool { payload.medicine != nil || medicineID != nil }
}

/// A highlighted (flagged) field: yellow fill and a warning mark.
private struct FlagHighlight: ViewModifier {
    let on: Bool
    func body(content: Content) -> some View {
        content
            .padding(.horizontal, on ? 8 : 0).padding(.vertical, on ? 6 : 0)
            .background(on ? Color.yellow.opacity(0.28) : Color.clear, in: RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(on ? Color.orange : Color.clear, lineWidth: 1.5))
    }
}

extension View {
    fileprivate func flagHighlight(_ on: Bool) -> some View { modifier(FlagHighlight(on: on)) }
}

/// The yellow "Please check" banner. Shared by the card and the Edit form.
struct PleaseCheckBanner: View {
    let fields: [String]
    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.title2).foregroundStyle(.orange)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
                Text("Please check").font(.headline)
                if fields.isEmpty {
                    Text("Some details may not match what was said.").font(.body)
                } else {
                    Text("Look again at: \(AssistantFormat.fieldList(fields))").font(.body)
                }
            }
            Spacer(minLength: 0)
        }
        .foregroundStyle(.primary)
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.yellow.opacity(0.35), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.orange, lineWidth: 2))
        .accessibilityElement(children: .combine)
    }
}

/// The exact recorded words, with a link to the moment in the recording.
struct QuoteBlock: View {
    let suggestion: Suggestion
    let noteTitle: String?
    var onOpen: ((Int64, Int?) -> Void)?

    var body: some View {
        let flagged = suggestion.checkFields.contains("quote")
        VStack(alignment: .leading, spacing: 8) {
            Text("What was said").font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
            HStack(alignment: .top, spacing: 10) {
                RoundedRectangle(cornerRadius: 2).fill(Color.accentColor.opacity(0.6)).frame(width: 4)
                if let q = suggestion.quote?.trimmingCharacters(in: .whitespacesAndNewlines), !q.isEmpty {
                    Text(verbatim: "“\(q)”").font(.body.italic()).textSelection(.enabled)
                } else if suggestion.noteID == nil && suggestion.medicineID != nil {
                    Text("From your profile.").font(.body).foregroundStyle(.secondary)
                } else {
                    Text("No recorded words were saved for this.").font(.body).foregroundStyle(.secondary)
                }
            }
            .fixedSize(horizontal: false, vertical: true)
            .flagHighlight(flagged)
            if let id = suggestion.noteID, let onOpen {
                Button { onOpen(id, suggestion.startSec) } label: {
                    Text(verbatim: "▶ \(noteTitle ?? String(localized: "Recording")) @\(AssistantFormat.mmss(suggestion.startSec ?? 0))")
                        .font(.subheadline.weight(.medium))
                        .multilineTextAlignment(.leading)
                        .frame(minHeight: 44, alignment: .leading)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.accentColor)
                .accessibilityLabel(Text("Open the recording: \(noteTitle ?? String(localized: "Recording"))"))
            }
        }
    }
}

// MARK: - card

struct SuggestionCard: View {
    let suggestion: Suggestion
    let noteTitle: String?
    let isStale: Bool
    let isBusy: Bool
    /// Called with the edited payload, or nil when the person accepts it as proposed.
    let onAccept: (SuggestionPayload?) -> Void
    let onReject: () -> Void

    @Environment(Router.self) private var router
    @State private var showEdit = false
    @State private var confirmFlagged = false
    /// What Save in the confirmation alert sends: nil = as proposed.
    @State private var pendingEdit: SuggestionPayload?
    /// Set by the Edit form; acted on after the sheet has gone, so the alert can show.
    @State private var afterEdit: (payload: SuggestionPayload?, needsConfirm: Bool)?
    /// The person's unsaved edits, so "Edit" in the confirmation alert reopens the form with them.
    @State private var resumeDraft: SuggestionDraft?

    private var s: Suggestion { suggestion }
    private func flagged(_ key: String) -> Bool { s.checkFields.contains(key) }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if s.isFlagged { PleaseCheckBanner(fields: s.checkFields) }
            if isStale {
                Label("This changed since it was suggested. Please check it again.", systemImage: "arrow.triangle.2.circlepath")
                    .font(.callout.weight(.medium))
                    .foregroundStyle(.primary)
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.orange.opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
            }
            if let text = s.payload.text, !text.isEmpty {
                Text(text).font(.title3.weight(.semibold))
                    .fixedSize(horizontal: false, vertical: true)
                    .flagHighlight(flagged("text"))
            }
            details
            QuoteBlock(suggestion: s, noteTitle: noteTitle) { id, sec in router.openNote = (id, sec) }
            buttons
        }
        .padding(16)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(s.isFlagged ? Color.orange : Color.clear, lineWidth: 2))
        .sheet(isPresented: $showEdit, onDismiss: runAfterEdit) {
            SuggestionEditForm(suggestion: s, noteTitle: noteTitle, resume: resumeDraft) { payload, changed, draft in
                resumeDraft = draft
                let fixedAFlag = !Set(s.checkFields).isDisjoint(with: changed)
                afterEdit = (payload, s.isFlagged && !fixedAFlag)
                showEdit = false
            }
        }
        .alert("Some details could not be matched to the recording. Save anyway?", isPresented: $confirmFlagged) {
            Button("Save") { onAccept(pendingEdit) }
            Button("Edit") { showEdit = true }
        }
    }

    private var header: some View {
        HStack(spacing: 10) {
            Image(systemName: icon).font(.title3).foregroundStyle(s.isMedicine ? Color.red : Color.accentColor)
                .frame(width: 28)
                .accessibilityHidden(true)
            Text(kindLabel).font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
            Spacer()
            if isBusy { ProgressView() }
        }
    }

    private var icon: String {
        switch s.kind {
        case .reminder: return s.isMedicine ? "pills.fill" : "bell.fill"
        case .task: return "checklist"
        case .profile: return s.isMedicine ? "pills.fill" : "person.crop.circle"
        case .vocab: return "character.book.closed"
        case .fact: return "lightbulb"
        case .unknown: return "questionmark.circle"
        }
    }

    private var kindLabel: String {
        switch s.kind {
        case .reminder: return s.isMedicine ? String(localized: "Medicine reminder") : String(localized: "Reminder")
        case .task: return String(localized: "To do")
        case .profile: return String(localized: "Change to your profile")
        case .vocab: return String(localized: "New word")
        case .fact: return String(localized: "Remember this")
        case .unknown: return String(localized: "Suggestion")
        }
    }

    // MARK: details

    private struct Row: Identifiable {
        let label: String
        let value: String
        let key: String?
        var id: String { label + "|" + (key ?? "") }
    }

    private var rows: [Row] {
        let p = s.payload
        var r: [Row] = []
        if s.kind == .profile, let c = AssistantFormat.changeLabel(p.change) {
            r.append(Row(label: String(localized: "Change"), value: c, key: nil))
        }
        if let m = p.medicine {
            if let v = m.name, !v.isEmpty { r.append(Row(label: String(localized: "Medicine"), value: v, key: "name")) }
            if let v = m.dose, !v.isEmpty { r.append(Row(label: String(localized: "Dose"), value: v, key: "dose")) }
            if let v = m.whenText ?? p.whenText, !v.isEmpty { r.append(Row(label: String(localized: "When to take it"), value: v, key: "when_text")) }
            if let v = m.frequency, !v.isEmpty { r.append(Row(label: String(localized: "How often"), value: v, key: "frequency")) }
            if let v = m.duration, !v.isEmpty { r.append(Row(label: String(localized: "How long"), value: v, key: "duration")) }
        } else if let v = p.whenText, !v.isEmpty, s.kind == .profile || s.kind == .reminder {
            r.append(Row(label: String(localized: "When"), value: v, key: "when_text"))
        }
        if let t = p.times, !t.isEmpty {
            r.append(Row(label: String(localized: "Time"), value: t.map(AssistantFormat.time).joined(separator: ", "), key: "times"))
        }
        if let v = AssistantFormat.repeatLabel(p.repeat) { r.append(Row(label: String(localized: "Repeats"), value: v, key: nil)) }
        if let v = p.startDate { r.append(Row(label: String(localized: "Starts"), value: AssistantFormat.day(v), key: "start_date")) }
        if let v = p.untilDate { r.append(Row(label: String(localized: "Until"), value: AssistantFormat.day(v), key: "until_date")) }
        if let v = p.who, !v.isEmpty { r.append(Row(label: String(localized: "Who"), value: v, key: nil)) }
        if let v = p.dueDate { r.append(Row(label: String(localized: "Due"), value: AssistantFormat.day(v), key: "due_date")) }
        if let rt = p.routine {
            let parts: [(String, String?)] = [(String(localized: "Wake up"), rt.wake), (String(localized: "Breakfast"), rt.breakfast),
                                              (String(localized: "Lunch"), rt.lunch), (String(localized: "Dinner"), rt.dinner),
                                              (String(localized: "Bed time"), rt.bed)]
            for (label, v) in parts { if let v, !v.isEmpty { r.append(Row(label: label, value: AssistantFormat.time(v), key: "routine")) } }
        }
        // A flagged field with nothing to show (e.g. no time at all) still gets a highlighted row.
        let shown = Set(r.compactMap(\.key)).union(["quote", "text"])
        for key in s.checkFields where !shown.contains(key) {
            r.append(Row(label: AssistantFormat.fieldName(key).localizedCapitalized, value: String(localized: "Not set"), key: key))
        }
        return r
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(rows) { row in
                let on = row.key.map(flagged) ?? false
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    if on {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                            .accessibilityLabel(Text("Please check"))
                    }
                    Text(row.label).font(.body).foregroundStyle(.secondary)
                    Spacer(minLength: 8)
                    Text(row.value).font(.body.weight(on ? .bold : .medium)).multilineTextAlignment(.trailing)
                }
                .flagHighlight(on)
            }
        }
    }

    // MARK: buttons

    private var buttons: some View {
        HStack(spacing: 10) {
            Button { yes() } label: {
                Label("Yes", systemImage: "checkmark").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.borderedProminent).tint(.green)
            Button { resumeDraft = nil; showEdit = true } label: {
                Label("Edit", systemImage: "pencil").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.bordered)
            Button { onReject() } label: {
                Label("No", systemImage: "xmark").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.bordered).tint(.red)
        }
        .labelStyle(.titleAndIcon)
        .disabled(isBusy)
    }

    private func yes() {
        resumeDraft = nil
        if s.isFlagged {
            pendingEdit = nil
            confirmFlagged = true
        } else {
            onAccept(nil)
        }
    }

    private func runAfterEdit() {
        guard let a = afterEdit else { return }
        afterEdit = nil
        if a.needsConfirm {
            pendingEdit = a.payload
            confirmFlagged = true
        } else {
            onAccept(a.payload)
        }
    }
}

// MARK: - edit form

/// The editable fields of a suggestion, as plain values, so "did the person change anything" is a
/// simple comparison and an unchanged form never sends an edit.
struct SuggestionDraft: Equatable {
    var text: String
    var times: [String]
    var repeatRule: String
    var untilDate: String?
    var dueDate: String?
    var who: String
    var medName: String
    var medDose: String
    var medWhen: String
    var medFrequency: String
    var medDuration: String

    init(_ p: SuggestionPayload) {
        text = p.text ?? ""
        times = p.times ?? []
        repeatRule = p.repeat ?? ""
        untilDate = p.untilDate
        dueDate = p.dueDate
        who = p.who ?? ""
        let m = p.medicine
        medName = m?.name ?? ""
        medDose = m?.dose ?? ""
        medWhen = m?.whenText ?? p.whenText ?? ""
        medFrequency = m?.frequency ?? ""
        medDuration = m?.duration ?? ""
    }

    /// The core's field names that differ between `self` and `before`.
    func changedKeys(from before: SuggestionDraft) -> Set<String> {
        var k = Set<String>()
        if text != before.text { k.insert("text") }
        if times != before.times { k.insert("times") }
        if repeatRule != before.repeatRule { k.insert("repeat") }
        if untilDate != before.untilDate { k.insert("until_date") }
        if dueDate != before.dueDate { k.insert("due_date") }
        if who != before.who { k.insert("who") }
        if medName != before.medName { k.insert("name") }
        if medDose != before.medDose { k.insert("dose") }
        if medWhen != before.medWhen { k.insert("when_text") }
        if medFrequency != before.medFrequency { k.insert("frequency") }
        if medDuration != before.medDuration { k.insert("duration") }
        return k
    }

    /// The original payload with only the changed fields written into it. Unknown keys are kept.
    func apply(to original: SuggestionPayload, changed: Set<String>) -> SuggestionPayload {
        var p = original
        func opt(_ s: String) -> String? {
            let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
            return t.isEmpty ? nil : t
        }
        if changed.contains("text") { p.text = opt(text) }
        if changed.contains("times") { p.times = times }
        if changed.contains("repeat") { p.repeat = opt(repeatRule) }
        if changed.contains("until_date") { p.untilDate = untilDate }
        if changed.contains("due_date") { p.dueDate = dueDate }
        if changed.contains("who") { p.who = opt(who) }
        // Medicine fields are written into the existing medicine object, so its other keys survive.
        let medKeys: [(String, String, String)] = [("name", "name", medName), ("dose", "dose", medDose),
                                                   ("when_text", "when_text", medWhen), ("frequency", "frequency", medFrequency),
                                                   ("duration", "duration", medDuration)]
        if case .object(var m)? = p.raw["medicine"] {
            for (changeKey, jsonKey, value) in medKeys where changed.contains(changeKey) {
                m[jsonKey] = opt(value).map { .string($0) } ?? .null
            }
            p.raw["medicine"] = .object(m)
        }
        // The top-level when_text (reminders, profile changes) follows the medicine's.
        if changed.contains("when_text"), p.raw["when_text"] != nil || p.raw["medicine"] == nil {
            p.whenText = opt(medWhen)
        }
        return p
    }
}

struct SuggestionEditForm: View {
    let suggestion: Suggestion
    let noteTitle: String?
    /// The edited payload (nil when nothing changed), the changed field names and the form's values.
    let onSave: (SuggestionPayload?, Set<String>, SuggestionDraft) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var draft: SuggestionDraft
    private let before: SuggestionDraft

    /// resume: earlier unsaved values to start from. Changes are always measured against the suggestion itself.
    init(suggestion: Suggestion, noteTitle: String?, resume: SuggestionDraft? = nil,
         onSave: @escaping (SuggestionPayload?, Set<String>, SuggestionDraft) -> Void) {
        self.suggestion = suggestion
        self.noteTitle = noteTitle
        self.onSave = onSave
        let d = SuggestionDraft(suggestion.payload)
        before = d
        _draft = State(initialValue: resume ?? d)
    }

    private var s: Suggestion { suggestion }
    private var hasMedicine: Bool { s.payload.medicine != nil }
    private var hasSchedule: Bool { s.kind == .reminder || (s.kind == .profile && hasMedicine) }
    private func flagged(_ key: String) -> Bool { s.checkFields.contains(key) }

    var body: some View {
        NavigationStack {
            Form {
                if s.isFlagged {
                    Section { PleaseCheckBanner(fields: s.checkFields) }
                        .listRowInsets(EdgeInsets()).listRowBackground(Color.clear)
                }
                Section { QuoteBlock(suggestion: s, noteTitle: noteTitle) }

                Section {
                    TextField("Wording", text: $draft.text, axis: .vertical)
                        .lineLimit(2...6)
                        .highlightedRow(flagged("text"))
                } header: { fieldHeader("Wording", flagged("text")) }

                if hasMedicine {
                    Section {
                        labeled("Medicine name", $draft.medName, "name")
                        labeled("Dose", $draft.medDose, "dose")
                        labeled("When to take it", $draft.medWhen, "when_text")
                        labeled("How often", $draft.medFrequency, "frequency")
                        labeled("How long", $draft.medDuration, "duration")
                    } header: { Text("Medicine") }
                }

                if hasSchedule { scheduleSection }

                if s.kind == .task {
                    Section {
                        labeled("Who", $draft.who, "who")
                        Toggle("Has a due date", isOn: hasDate($draft.dueDate))
                            .frame(minHeight: 44)
                            .highlightedRow(flagged("due_date"))
                        if draft.dueDate != nil {
                            DatePicker("Due", selection: dateValue($draft.dueDate), displayedComponents: .date)
                                .frame(minHeight: 44)
                                .highlightedRow(flagged("due_date"))
                        }
                    } header: { fieldHeader("To do", flagged("due_date")) }
                }
            }
            .navigationTitle("Edit")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) { Button("Save") { save() }.font(.headline) }
            }
        }
    }

    private var scheduleSection: some View {
        Section {
            ForEach(draft.times.indices, id: \.self) { i in
                HStack {
                    DatePicker("Time", selection: timeValue(i), displayedComponents: .hourAndMinute)
                    Button(role: .destructive) { draft.times.remove(at: i) } label: {
                        Image(systemName: "minus.circle.fill").font(.title3)
                            .frame(minWidth: 44, minHeight: 44)
                    }
                    .buttonStyle(.borderless)
                    .accessibilityLabel(Text("Remove this time"))
                }
                .highlightedRow(flagged("times"))
            }
            Button { draft.times.append(draft.times.last ?? "08:00") } label: {
                Label("Add a time", systemImage: "plus.circle").frame(minHeight: 44)
            }
            .highlightedRow(flagged("times") && draft.times.isEmpty)
            Picker("Repeats", selection: $draft.repeatRule) {
                ForEach(repeatOptions, id: \.self) { r in
                    Text(AssistantFormat.repeatLabel(r) ?? String(localized: "Not set")).tag(r)
                }
            }
            .frame(minHeight: 44)
            Toggle("Has an end date", isOn: hasDate($draft.untilDate))
                .frame(minHeight: 44)
                .highlightedRow(flagged("until_date"))
            if draft.untilDate != nil {
                DatePicker("Until", selection: dateValue($draft.untilDate), displayedComponents: .date)
                    .frame(minHeight: 44)
                    .highlightedRow(flagged("until_date"))
            }
        } header: { fieldHeader("When", flagged("times") || flagged("until_date")) }
    }

    private var repeatOptions: [String] {
        var o = ["once", "daily", "twice_daily", "weekly"]
        if !o.contains(before.repeatRule) { o.insert(before.repeatRule, at: 0) }   // keeps an unusual or empty value
        return o
    }

    private func labeled(_ title: LocalizedStringKey, _ value: Binding<String>, _ key: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                if flagged(key) { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
                Text(title).font(.footnote).foregroundStyle(flagged(key) ? Color.primary : Color.secondary)
            }
            TextField(title, text: value, axis: .vertical).font(.body.weight(flagged(key) ? .semibold : .regular))
        }
        .frame(minHeight: 44)
        .highlightedRow(flagged(key))
    }

    private func fieldHeader(_ title: LocalizedStringKey, _ on: Bool) -> some View {
        HStack(spacing: 6) {
            Text(title)
            if on { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
        }
    }

    private func hasDate(_ v: Binding<String?>) -> Binding<Bool> {
        Binding(get: { v.wrappedValue != nil },
                set: { v.wrappedValue = $0 ? (v.wrappedValue ?? AssistantFormat.ymd(Date())) : nil })
    }

    private func dateValue(_ v: Binding<String?>) -> Binding<Date> {
        Binding(get: { v.wrappedValue.flatMap { ReminderScheduler.day($0, .current) } ?? Date() },
                set: { v.wrappedValue = AssistantFormat.ymd($0) })
    }

    private func timeValue(_ i: Int) -> Binding<Date> {
        Binding(get: {
            guard i < draft.times.count, let (h, m) = ReminderScheduler.clock(draft.times[i]) else { return defaultTime }
            return Calendar.current.date(bySettingHour: h, minute: m, second: 0, of: Date()) ?? defaultTime
        }, set: { d in
            guard i < draft.times.count else { return }
            let c = Calendar.current.dateComponents([.hour, .minute], from: d)
            draft.times[i] = String(format: "%02d:%02d", c.hour ?? 0, c.minute ?? 0)
        })
    }

    private var defaultTime: Date { Calendar.current.date(bySettingHour: 8, minute: 0, second: 0, of: Date()) ?? Date() }

    private func save() {
        let changed = draft.changedKeys(from: before)
        onSave(changed.isEmpty ? nil : draft.apply(to: s.payload, changed: changed), changed, draft)
    }
}

extension View {
    /// Yellow row background in a Form for a flagged field.
    fileprivate func highlightedRow(_ on: Bool) -> some View {
        listRowBackground(on ? Color.yellow.opacity(0.3) : nil)
    }
}
