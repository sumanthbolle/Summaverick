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
                .accessibilityLabel(Text("Open the recording: \(noteTitle ?? String(localized: "Recording")) at \(AssistantFormat.mmss(suggestion.startSec ?? 0))"))
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
            SuggestionEditForm(suggestion: s, noteTitle: noteTitle, resume: resumeDraft) { payload, _, draft in
                resumeDraft = draft
                // A flagged card always asks again, edited or not: the core does not enforce the flag.
                afterEdit = (payload, s.isFlagged)
                showEdit = false
            }
        }
        .alert("Some details could not be matched to the recording. Save anyway?", isPresented: $confirmFlagged) {
            Button("Save") { onAccept(pendingEdit) }
            Button("Edit") { showEdit = true }
        }
        .onChange(of: showEdit || confirmFlagged) { _, open in PresentationGate.shared.set("card.\(s.id)", open) }
        .onDisappear { PresentationGate.shared.set("card.\(s.id)", false) }
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
        // A reminder needs a time; the core refuses one without. Ask for it in the form instead.
        if s.kind == .reminder && (s.payload.times ?? []).allSatisfy({ ReminderScheduler.clock($0) == nil }) {
            showEdit = true
        } else if s.isFlagged {
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
    /// Text is compared trimmed, so a stray space is not an edit.
    func changedKeys(from before: SuggestionDraft) -> Set<String> {
        func t(_ s: String) -> String { s.trimmingCharacters(in: .whitespacesAndNewlines) }
        var k = Set<String>()
        if t(text) != t(before.text) { k.insert("text") }
        if times != before.times { k.insert("times") }
        if repeatRule != before.repeatRule { k.insert("repeat") }
        if untilDate != before.untilDate { k.insert("until_date") }
        if dueDate != before.dueDate { k.insert("due_date") }
        if t(who) != t(before.who) { k.insert("who") }
        if t(medName) != t(before.medName) { k.insert("name") }
        if t(medDose) != t(before.medDose) { k.insert("dose") }
        if t(medWhen) != t(before.medWhen) { k.insert("when_text") }
        if t(medFrequency) != t(before.medFrequency) { k.insert("frequency") }
        if t(medDuration) != t(before.medDuration) { k.insert("duration") }
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

/// What an Edit form opened from an Ask answer ("Make reminder" / "Make to-do") shows in place of the
/// recorded quote: the answer exactly as it was given.
struct AnswerContext: Equatable {
    let text: String
    /// A medicine answer: built in the core from saved facts and recorded words; shown quoted, never changed.
    let medicine: Bool
}

struct SuggestionEditForm: View {
    let noteTitle: String?
    /// Set when the form was opened from an Ask answer.
    let answer: AnswerContext?
    /// The edited payload (nil when nothing changed), the changed field names and the form's values.
    let onSave: (SuggestionPayload?, Set<String>, SuggestionDraft) -> Void

    @Environment(\.dismiss) private var dismiss
    @Environment(AppSettings.self) private var settings
    @Environment(Router.self) private var router
    @State private var draft: SuggestionDraft
    /// Frozen when the form opens, so a reload behind the sheet never changes what "unchanged" means.
    @State private var original: Suggestion
    @State private var before: SuggestionDraft
    /// What the picker sheet is choosing. A time index == times.count means a new time.
    private enum Picking: Identifiable, Hashable {
        case time(Int), until, due
        var id: Self { self }
    }
    @State private var picking: Picking?
    /// The wheel's starting position only; nothing is written until the person confirms.
    @State private var pickedDate = Date()
    @State private var openMoment: NoteMoment?

    /// resume: earlier unsaved values to start from. Changes are always measured against the suggestion itself.
    init(suggestion: Suggestion, noteTitle: String?, resume: SuggestionDraft? = nil, answer: AnswerContext? = nil,
         onSave: @escaping (SuggestionPayload?, Set<String>, SuggestionDraft) -> Void) {
        self.noteTitle = noteTitle
        self.answer = answer
        self.onSave = onSave
        let d = SuggestionDraft(suggestion.payload)
        _original = State(initialValue: suggestion)
        _before = State(initialValue: d)
        _draft = State(initialValue: resume ?? d)
    }

    private var s: Suggestion { original }
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
                if let answer {
                    answerSection(answer)
                } else {
                    Section { QuoteBlock(suggestion: s, noteTitle: noteTitle) { id, sec in openMoment = NoteMoment(noteID: id, startSec: sec) } }
                }

                Section {
                    TextField("Wording", text: $draft.text, axis: .vertical)
                        .lineLimit(2...6)
                        .highlightedRow(flagged("text"))
                    if textMissing {
                        Label("Write what this is about before saving.", systemImage: "exclamationmark.circle.fill")
                            .font(.callout.weight(.medium)).foregroundStyle(.red)
                    }
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
                        Toggle("Has a due date", isOn: hasDate($draft.dueDate, .due))
                            .frame(minHeight: 44)
                            .highlightedRow(flagged("due_date"))
                        if let v = draft.dueDate {
                            dateRow("Due", v, $draft.dueDate, .due)
                                .highlightedRow(flagged("due_date"))
                        }
                    } header: { fieldHeader("To do", flagged("due_date")) }
                }
            }
            .navigationTitle("Edit")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }.font(.headline).disabled(timesProblem != nil || textMissing || repeatMissing)
                }
            }
            // The ▶ link opens the recording inside this sheet, so the edits are kept.
            .navigationDestination(item: $openMoment) { m in
                NoteDetailView(noteID: m.noteID, startAt: m.startSec).environment(settings).environment(router)
            }
            .sheet(item: $picking) { pickerSheet($0) }
        }
    }

    /// The person chooses a time or date here and confirms it; nothing is written until "Set".
    private func pickerSheet(_ what: Picking) -> some View {
        let isTime: Bool = { if case .time = what { return true } else { return false } }()
        return NavigationStack {
            DatePicker(isTime ? "Time" : "Date", selection: $pickedDate,
                       displayedComponents: isTime ? .hourAndMinute : .date)
                .datePickerStyle(.wheel)
                .labelsHidden()
                .frame(maxWidth: .infinity)
                .padding()
                .navigationTitle(isTime ? "Pick a time" : "Pick a date")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("Cancel") { picking = nil } }
                    ToolbarItem(placement: .confirmationAction) {
                        Button(isTime ? "Set time" : "Set date") { confirmPick(what) }.font(.headline)
                    }
                }
        }
        .presentationDetents([.medium])
    }

    private func confirmPick(_ what: Picking) {
        switch what {
        case .time(let i):
            let c = Calendar.current.dateComponents([.hour, .minute], from: pickedDate)
            let v = String(format: "%02d:%02d", c.hour ?? 0, c.minute ?? 0)
            if i < draft.times.count { draft.times[i] = v } else { draft.times.append(v) }
        case .until: draft.untilDate = AssistantFormat.ymd(pickedDate)
        case .due: draft.dueDate = AssistantFormat.ymd(pickedDate)
        }
        picking = nil
    }

    private func pickTime(at i: Int) {
        // The wheel starts at the current time; it is only a starting position, never saved unless confirmed.
        pickedDate = Date()
        picking = .time(i)
    }

    private func pickDate(_ what: Picking) {
        pickedDate = Date()
        picking = what
    }

    /// A readable date gets an inline picker; an unreadable one is shown as it is and must be picked.
    @ViewBuilder
    private func dateRow(_ title: LocalizedStringKey, _ value: String, _ binding: Binding<String?>, _ what: Picking) -> some View {
        if ReminderScheduler.day(value, .current) != nil {
            DatePicker(title, selection: dateValue(binding), displayedComponents: .date)
                .frame(minHeight: 44)
        } else {
            Button { pickDate(what) } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Label("Not set — please pick a date", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.primary)
                    if !value.isEmpty {
                        Text(verbatim: "“\(value)”").font(.footnote).foregroundStyle(.secondary)
                    }
                }
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .accessibilityHint(Text("Pick a date"))
        }
    }

    /// Why Save is blocked for a reminder's times, or nil.
    private var timesProblem: LocalizedStringKey? {
        guard hasSchedule else { return nil }
        // A profile medicine (e.g. "as needed") may have no times; a reminder may not.
        if s.kind == .reminder && draft.times.isEmpty { return "Add at least one time before saving." }
        if draft.times != before.times && draft.times.contains(where: { ReminderScheduler.clock($0) == nil }) {
            return "Pick a time for each row, or remove it."
        }
        // A reminder made here starts today: an end date before today would never ring (the core refuses it).
        if answer != nil, s.kind == .reminder, let u = draft.untilDate, u < AssistantFormat.ymd(Date()) {
            return "The end date is before today. Pick a later end date, or turn it off."
        }
        // A new one-time reminder starts today; if every time has passed it would never ring.
        if answer != nil, s.kind == .reminder, draft.repeatRule == "once",
           draft.times.allSatisfy(Self.hasPassedToday) {
            return "That time has passed today. Pick a later time, or choose how often it repeats."
        }
        return nil
    }

    private static func hasPassedToday(_ hhmm: String) -> Bool {
        guard let (h, m) = ReminderScheduler.clock(hhmm),
              let d = Calendar.current.date(bySettingHour: h, minute: m, second: 0, of: Date()) else { return false }
        return d <= Date()
    }

    /// A medicine reminder from an answer has no repeat until the person picks one.
    private var repeatMissing: Bool {
        answer?.medicine == true && s.kind == .reminder && draft.repeatRule.isEmpty
    }

    /// A reminder or to-do the person makes from an answer needs words (the core refuses empty text).
    private var textMissing: Bool {
        answer != nil && draft.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    /// The answer exactly as given, its source link, and for medicine a reminder to check against the doctor's words.
    private func answerSection(_ a: AnswerContext) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 8) {
                Text(a.medicine ? "Copied from your recordings" : "Summaverick's answer")
                    .font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
                HStack(alignment: .top, spacing: 10) {
                    RoundedRectangle(cornerRadius: 2).fill(Color.accentColor.opacity(0.6)).frame(width: 4)
                        .accessibilityHidden(true)
                    Text(verbatim: a.text).font(a.medicine ? .body.italic() : .body).textSelection(.enabled)
                }
                .fixedSize(horizontal: false, vertical: true)
                if let id = s.noteID {
                    let title = noteTitle.flatMap { $0.isEmpty ? nil : $0 } ?? String(localized: "Recording")
                    Button { openMoment = NoteMoment(noteID: id, startSec: s.startSec) } label: {
                        Text(verbatim: "▶ \(title) @\(AssistantFormat.mmss(s.startSec ?? 0))")
                            .font(.subheadline.weight(.medium))
                            .multilineTextAlignment(.leading)
                            .frame(minHeight: 44, alignment: .leading)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Color.accentColor)
                    .accessibilityLabel(Text("Open the recording: \(title) at \(AssistantFormat.mmss(s.startSec ?? 0))"))
                }
            }
            if a.medicine {
                Label("Please check the dose and time against your doctor's words above.", systemImage: "exclamationmark.triangle.fill")
                    .font(.body.weight(.medium))
                    .foregroundStyle(.primary)
                    .listRowBackground(Color.yellow.opacity(0.3))
            }
        }
    }

    private var scheduleSection: some View {
        Section {
            ForEach(draft.times.indices, id: \.self) { i in
                HStack {
                    if ReminderScheduler.clock(draft.times[i]) != nil {
                        DatePicker("Time", selection: timeValue(i), displayedComponents: .hourAndMinute)
                    } else {
                        // Unreadable value: show it as it is and ask for a real time. Never invent one.
                        Button { pickTime(at: i) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Label("Not set — please pick a time", systemImage: "exclamationmark.triangle.fill")
                                    .foregroundStyle(.primary)
                                if !draft.times[i].isEmpty {
                                    Text(verbatim: "“\(draft.times[i])”").font(.footnote).foregroundStyle(.secondary)
                                }
                            }
                            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.borderless)
                        .accessibilityHint(Text("Pick a time"))
                    }
                    Button(role: .destructive) { draft.times.remove(at: i) } label: {
                        Image(systemName: "minus.circle.fill").font(.title3)
                            .frame(minWidth: 44, minHeight: 44)
                    }
                    .buttonStyle(.borderless)
                    .accessibilityLabel(Text("Remove this time"))
                }
                .highlightedRow(flagged("times"))
            }
            Button { pickTime(at: draft.times.count) } label: {
                Label("Add a time", systemImage: "plus.circle").frame(minHeight: 44)
            }
            .highlightedRow(flagged("times") && draft.times.isEmpty)
            if let problem = timesProblem {
                Label(problem, systemImage: "exclamationmark.circle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(.red)
            }
            if repeatMissing {
                Label("Choose how often it repeats before saving.", systemImage: "exclamationmark.circle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(.red)
            }
            Picker("Repeats", selection: $draft.repeatRule) {
                ForEach(repeatOptions, id: \.self) { r in
                    Text(AssistantFormat.repeatLabel(r) ?? String(localized: "Not set")).tag(r)
                }
            }
            .frame(minHeight: 44)
            Toggle("Has an end date", isOn: hasDate($draft.untilDate, .until))
                .frame(minHeight: 44)
                .highlightedRow(flagged("until_date"))
            if let v = draft.untilDate {
                dateRow("Until", v, $draft.untilDate, .until)
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
                if flagged(key) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                        .accessibilityLabel(Text("Please check this"))
                }
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
            if on {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    .accessibilityLabel(Text("Please check this"))
            }
        }
    }

    /// Turning the toggle on opens the picker; the date is set only when the person confirms one.
    private func hasDate(_ v: Binding<String?>, _ what: Picking) -> Binding<Bool> {
        Binding(get: { v.wrappedValue != nil },
                set: { on in
                    if !on { v.wrappedValue = nil } else if v.wrappedValue == nil { pickDate(what) }
                })
    }

    /// Only used for dates that parse (see dateRow).
    private func dateValue(_ v: Binding<String?>) -> Binding<Date> {
        Binding(get: { v.wrappedValue.flatMap { ReminderScheduler.day($0, .current) } ?? Date() },
                set: { v.wrappedValue = AssistantFormat.ymd($0) })
    }

    private func timeValue(_ i: Int) -> Binding<Date> {
        Binding(get: {
            // Only used for rows whose value parses; unreadable rows use the picker sheet instead.
            guard i < draft.times.count, let (h, m) = ReminderScheduler.clock(draft.times[i]),
                  let d = Calendar.current.date(bySettingHour: h, minute: m, second: 0, of: Date()) else { return Date() }
            return d
        }, set: { d in
            guard i < draft.times.count else { return }
            let c = Calendar.current.dateComponents([.hour, .minute], from: d)
            draft.times[i] = String(format: "%02d:%02d", c.hour ?? 0, c.minute ?? 0)
        })
    }

    private func save() {
        guard timesProblem == nil else { return }
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
