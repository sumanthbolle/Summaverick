// A memory, shown as a card: what it says, the exact words it came from (▶ plays them), who, when it was said,
// when it is due, and what it replaced. MemoryCardView loads one memory by id and offers Edit, Wrong, Delete and
// (promises and questions) Done; one still waiting to be checked shows the review queue's Keep and Wrong first.
// Medicine memories follow the saved medicine and are read-only here.
// Reused by the review queue (MemoryReviewCard), a note's Remembered section, the map and recall.
import SwiftUI

// MARK: - formatting

enum MemoryFormat {
    static func typeLabel(_ type: String) -> String {
        switch type {
        case "decision": return String(localized: "Decision")
        case "promise": return String(localized: "Promise")
        case "question": return String(localized: "Question")
        case "person": return String(localized: "Person")
        case "idea": return String(localized: "Idea")
        default: return String(localized: "Fact")
        }
    }

    static func typeIcon(_ m: BrainMemory) -> String {
        if m.isMedicine { return "pills.fill" }
        switch m.type {
        case "decision": return "signpost.right"
        case "promise": return "hand.raised"
        case "question": return "questionmark.bubble"
        case "person": return "person"
        case "idea": return "lightbulb"
        default: return "brain"
        }
    }

    /// "Due Friday 13 November ("next Friday")", or the date or the spoken words alone.
    static func due(_ m: BrainMemory) -> String? {
        let words = m.dueText?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        switch (m.dueDate, words.isEmpty) {
        case (let d?, false): return String(localized: "Due \(AssistantFormat.day(d)) (\"\(words)\")")
        case (let d?, true): return String(localized: "Due \(AssistantFormat.day(d))")
        case (nil, false): return String(localized: "Due \("“\(words)”")")
        case (nil, true): return nil
        }
    }
}

extension BrainMemory {
    /// A saved medicine's memory: it follows the medicine list, and the core refuses edit, review and remove.
    var isMedicine: Bool { origin == "medicine" }
    /// Waiting in the review queue and not checked yet.
    var waitingForCheck: Bool { queued && review == "new" }
    var canBeDone: Bool { (type == "promise" || type == "question") && !isMedicine }
    /// Checked names in the form the app shows (the written form of a name said in Telugu).
    var shownNames: [String] {
        var seen = Set<String>()
        return checked.filter { $0.kind == "name" }.map(\.shown).filter { !$0.isEmpty && seen.insert($0).inserted }
    }
}

// MARK: - the card's content

/// What a memory says and where it came from. No actions: the cards around it add those.
struct MemoryDetails: View {
    let memory: BrainMemory
    /// nil: the sources are shown, not playable.
    let onPlay: ((Int64, Int) -> Void)?
    /// Show "Waiting for you to check" (outside the review queue).
    var showsWaiting = true

    private var m: BrainMemory { memory }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if m.health && m.waitingForCheck { HealthCheckBanner() }
            header
            Text(verbatim: m.text).font(.title3.weight(.semibold))
                .fixedSize(horizontal: false, vertical: true)
                .strikethrough(m.done, color: Theme.secondary)
            ForEach(Array(m.history.enumerated()), id: \.offset) { _, old in historyLine(old) }
            if m.lowConfidence, let first = m.sources.first, let onPlay {
                Button { onPlay(first.noteID, first.startSec) } label: {
                    Label("I may have heard this wrong. Listen?", systemImage: "ear")
                        .font(.callout.weight(.medium))
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.caution)
            } else if m.lowConfidence {
                Label("I may have heard this wrong. Listen?", systemImage: "ear")
                    .font(.callout.weight(.medium)).foregroundStyle(Theme.caution)
            }
            details
            ForEach(m.sources) { source($0) }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: MemoryFormat.typeIcon(m)).font(.title3)
                .foregroundStyle(m.isMedicine ? Theme.medicineIcon : Theme.accent)
                .frame(width: 28)
                .accessibilityHidden(true)
            Text(MemoryFormat.typeLabel(m.type)).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.secondary)
            if m.review == "new" {
                Text("New").font(.caption.weight(.bold)).foregroundStyle(Theme.primaryLabel)
                    .padding(.horizontal, 8).padding(.vertical, 2)
                    .background(Theme.primaryFill, in: Capsule())
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }

    private func historyLine(_ old: BrainMemory) -> some View {
        Group {
            if let said = old.saidDate {
                Text("Was \(old.text), now \(m.text), said on \(AssistantFormat.day(said))")
            } else {
                Text("Was \(old.text), now \(m.text)")
            }
        }
        .font(.callout).foregroundStyle(Theme.secondary)
        .fixedSize(horizontal: false, vertical: true)
    }

    private struct Row: Identifiable {
        let label: String
        let value: String
        var id: String { label }
    }

    private var rows: [Row] {
        var r: [Row] = []
        if let who = m.who?.trimmingCharacters(in: .whitespacesAndNewlines), !who.isEmpty {
            r.append(Row(label: String(localized: "Who"), value: who))
        }
        let names = m.shownNames.filter { $0 != m.who }
        if !names.isEmpty { r.append(Row(label: String(localized: "Names"), value: names.formatted(.list(type: .and)))) }
        return r
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 6) {
            if showsWaiting && m.waitingForCheck {
                Label("Waiting for you to check", systemImage: "hourglass")
                    .font(.callout.weight(.medium)).foregroundStyle(Theme.caution)
            }
            ForEach(rows) { row in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(row.label).foregroundStyle(Theme.secondary)
                    Spacer(minLength: 8)
                    Text(verbatim: row.value).fontWeight(.medium).multilineTextAlignment(.trailing)
                }
                .font(.body)
                .accessibilityElement(children: .combine)
            }
            if let due = MemoryFormat.due(m) {
                Text(verbatim: due).font(.body.weight(.medium))
                    .foregroundStyle(m.done ? Theme.secondary : Theme.text)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let said = m.saidDate {
                Text("Said on \(AssistantFormat.day(said))").font(.callout).foregroundStyle(Theme.secondary)
            }
        }
    }

    /// The exact words, in quotation marks as said, and ▶ to the moment in the recording.
    private func source(_ s: MemorySource) -> some View {
        let title = s.title.isEmpty ? String(localized: "Recording") : s.title
        let stamp = AssistantFormat.mmss(s.startSec)
        return VStack(alignment: .leading, spacing: 4) {
            if !s.quote.isEmpty {
                HStack(alignment: .top, spacing: 10) {
                    RoundedRectangle(cornerRadius: 2).fill(Theme.secondary).frame(width: 4)
                        .accessibilityHidden(true)
                    Text(verbatim: "“\(s.quote)”").font(.body.italic()).textSelection(.enabled)
                }
                .fixedSize(horizontal: false, vertical: true)
            }
            if let onPlay {
                Button { onPlay(s.noteID, s.startSec) } label: {
                    Text(verbatim: "▶ \(title) @\(stamp)")
                        .font(.subheadline.weight(.medium))
                        .multilineTextAlignment(.leading)
                        .frame(minHeight: 44, alignment: .leading)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.accent)
                .accessibilityLabel(Text("Open the recording: \(title) at \(stamp)"))
            } else {
                Text(verbatim: "\(title) @\(stamp)").font(.subheadline).foregroundStyle(Theme.secondary)
            }
        }
    }
}

/// The yellow line on a health-related memory that is still waiting to be checked.
struct HealthCheckBanner: View {
    var body: some View {
        Label("Health-related: please check", systemImage: "heart.text.square.fill")
            .font(.headline)
            .foregroundStyle(Theme.text)
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.healthFill, in: RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Theme.healthIcon, lineWidth: 1.5))
    }
}

// MARK: - edit sheet

/// The person's own words for a memory. Save waits for the core: a failure keeps the sheet open with a message.
struct MemoryEditSheet: View {
    let memory: BrainMemory
    /// true when saved.
    let onSave: (String) async -> Bool

    @Environment(\.dismiss) private var dismiss
    @State private var draft: String
    @State private var saving = false
    @State private var failed = false

    init(memory: BrainMemory, onSave: @escaping (String) async -> Bool) {
        self.memory = memory
        self.onSave = onSave
        _draft = State(initialValue: memory.text)
    }

    private var trimmed: String { draft.trimmingCharacters(in: .whitespacesAndNewlines) }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Wording", text: $draft, prompt: Theme.prompt("Wording"), axis: .vertical)
                        .font(.body)
                        .frame(minHeight: 44)
                }
                .listRowBackground(Theme.formRow)
                if let q = memory.sources.first?.quote, !q.isEmpty {
                    Section("What was said") {
                        Text(verbatim: "“\(q)”").font(.body.italic()).textSelection(.enabled)
                    }
                    .listRowBackground(Theme.formRow)
                }
                if failed {
                    Section {
                        Label("I couldn't save this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(Theme.warning)
                    }
                    .listRowBackground(Theme.formRow)
                }
            }
            .themedForm()
            .navigationTitle(Text("Edit"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(trimmed.isEmpty || saving)
                }
            }
        }
    }

    private func save() {
        guard !saving else { return }
        if trimmed == memory.text { dismiss(); return }
        saving = true
        failed = false
        Task {
            let ok = await onSave(trimmed)
            saving = false
            if ok { dismiss() } else { failed = true }
        }
    }
}

// MARK: - the card

/// What the map adds to a memory's card: make it a reminder or a to-do, or ask about it.
enum MapMemoryAction: Equatable { case remind, task, ask }

struct MemoryCardView: View {
    let memoryID: Int64
    /// ▶: open the recording (note id, second).
    let onPlay: ((Int64, Int) -> Void)?
    /// Called after Wrong or Delete took the memory away.
    var onRemoved: (() -> Void)? = nil
    /// Set by the map: the card offers Ask about this, Remind me and Make a task, and hands them back here.
    var onMapAction: ((MapMemoryAction, BrainMemory) -> Void)? = nil

    @State private var memory: BrainMemory?
    /// The core said it no longer exists.
    @State private var gone = false
    /// The last read failed (not the same as gone): a line with Try again.
    @State private var readFailed = false
    @State private var busy = false
    @State private var failed = false
    @State private var editing = false
    @State private var confirmDelete = false

    init(memoryID: Int64, onPlay: ((Int64, Int) -> Void)?, onRemoved: (() -> Void)? = nil,
         onMapAction: ((MapMemoryAction, BrainMemory) -> Void)? = nil) {
        self.memoryID = memoryID
        self.onPlay = onPlay
        self.onRemoved = onRemoved
        self.onMapAction = onMapAction
    }

    var body: some View {
        Group {
            if let m = memory, !gone, m.review != "wrong" {
                card(m)
            } else if gone || memory != nil {
                Text("This one was removed from what I remember.").font(.body).foregroundStyle(Theme.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(16)
            } else if readFailed {
                loadFailedLine.padding(16)
            } else {
                ProgressView().frame(maxWidth: .infinity, minHeight: 88)
            }
        }
        .task(id: memoryID) { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .onChange(of: editing || confirmDelete) { _, open in PresentationGate.shared.set("memory.\(memoryID)", open) }
        .onDisappear { PresentationGate.shared.set("memory.\(memoryID)", false) }
    }

    private func card(_ m: BrainMemory) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            MemoryDetails(memory: m, onPlay: onPlay)
            if m.isMedicine {
                Label("This follows your medicine list. Change it there.", systemImage: "pills")
                    .font(.callout).foregroundStyle(Theme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                actions(m)
            }
            if failed {
                Label("I couldn't save this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if readFailed { loadFailedLine }
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(m.health && m.waitingForCheck ? Theme.healthIcon : Color.clear, lineWidth: 2))
        .sheet(isPresented: $editing) {
            MemoryEditSheet(memory: m) { text in await run { await CoreEngine.shared.editMemory(m.id, text) } }
        }
        .confirmationDialog(Text("Forget this"), isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Forget this", role: .destructive) {
                decideLinked(.delete, m)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(verbatim: "“\(m.text)”")
        }
    }

    private func actions(_ m: BrainMemory) -> some View {
        VStack(spacing: 10) {
            // Not checked yet: the review queue's Keep and Wrong come first (the same calls as the queue's card).
            if m.waitingForCheck {
                MemoryActionButtons(busy: busy, buttons: [
                    .init(title: "Keep", icon: "checkmark", tone: .positive, prominent: true) { decideLinked(.keep, m) },
                    .init(title: "Wrong", icon: "xmark", tone: .negative) { decideLinked(.wrong, m) },
                ])
            }
            if let onMapAction {
                Button { onMapAction(.ask, m) } label: {
                    Label("Ask about this", systemImage: "bubble.left.and.text.bubble.right")
                        .font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pillProminent)
                .disabled(busy)
                MemoryActionButtons(busy: busy, buttons: [
                    .init(title: "Remind me", icon: "bell") { onMapAction(.remind, m) },
                    .init(title: "Make a task", icon: "checklist") { onMapAction(.task, m) },
                ])
            }
            if m.canBeDone {
                Button { Task { await run { await CoreEngine.shared.setMemoryDone(m.id, !m.done) } } } label: {
                    Label("Done", systemImage: m.done ? "checkmark.circle.fill" : "circle")
                        .font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pillPositive)
                .accessibilityAddTraits(m.done ? .isSelected : [])
            }
            MemoryActionButtons(busy: busy, buttons: lastRow(m))
        }
    }

    /// Edit, Wrong (unless it is already above, for a memory not checked yet) and Forget this.
    private func lastRow(_ m: BrainMemory) -> [MemoryActionButtons.Item] {
        var row: [MemoryActionButtons.Item] = [.init(title: "Edit", icon: "pencil") { failed = false; editing = true }]
        if !m.waitingForCheck { row.append(.init(title: "Wrong", icon: "xmark", tone: .negative) { decideLinked(.wrong, m) }) }
        row.append(.init(title: "Forget this", icon: "trash", tone: .warning) { confirmDelete = true })
        return row
    }

    /// Runs one change; on success tells the app (every view showing memories reloads), on failure keeps the
    /// card as it is and shows the inline message. Taps while one runs are ignored.
    @discardableResult private func run(_ change: () async -> Bool) async -> Bool {
        guard !busy else { return false }
        busy = true
        failed = false
        let ok = await change()
        busy = false
        failed = !ok
        if ok { NotificationCenter.default.post(name: .assistantChanged, object: nil) }
        return ok
    }

    private var loadFailedLine: some View {
        HStack(spacing: 12) {
            Label("I couldn't open this.", systemImage: "exclamationmark.triangle.fill")
                .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
            Spacer(minLength: 0)
            Button("Try again") { Task { await reload() } }
                .buttonStyle(.pill).frame(minHeight: 44)
        }
    }

    /// Keep, Wrong or Delete. A memory waiting in the queue goes through the queue's own plan
    /// (ReviewItem.linkedDecision): Wrong or Delete declines a pending reminder or to-do in its card first, so it
    /// does not come back on its own in "Waiting for you"; Keep touches only the memory. When the queue cannot be
    /// read, nothing is changed and the inline message shows. Keep leaves the card open (now checked).
    private func decideLinked(_ tap: ReviewTap, _ m: BrainMemory) {
        guard !busy else { return }
        busy = true
        failed = false
        Task {
            var plan = LinkedDecision(memoryReview: tap == .wrong ? "wrong" : tap == .keep ? "kept" : nil,
                                      removesMemory: tap == .delete)
            var linked: Suggestion?
            var r = LinkedRunResult()
            if m.waitingForCheck {
                if let q = await CoreEngine.shared.reviewQueue() {
                    if let item = q.first(where: { $0.id == m.id }) { plan = item.linkedDecision(tap); linked = item.suggestion }
                } else {
                    r.failed = true
                }
            }
            if !r.failed {
                r = await plan.perform(
                    suggestionStep: { step in
                        guard step == .reject, let s = linked else { return .failed }
                        return await CoreEngine.shared.reject(s.id) ? .done : .failed
                    },
                    memoryStep: { review in await CoreEngine.shared.reviewMemory(m.id, review == "kept") },
                    removeStep: { await CoreEngine.shared.removeMemory(m.id) })
            }
            busy = false
            failed = r.failed
            if r.needsReload { NotificationCenter.default.post(name: .assistantChanged, object: nil) }
            if !r.failed && tap != .keep { onRemoved?() }
        }
    }

    private func reload() async {
        switch await CoreEngine.shared.lookupMemory(memoryID) {
        case .found(let m): memory = m; gone = false; readFailed = false
        case .gone: gone = true; readFailed = false
        case .failed: readFailed = true
        }
    }
}

// MARK: - a row of big buttons

/// Equal-width buttons in a row, stacked when the text is very large (Dynamic Type accessibility sizes).
struct MemoryActionButtons: View {
    struct Item: Identifiable {
        let title: LocalizedStringKey
        let icon: String
        enum Tone { case plain, positive, negative, warning }
        var tone = Tone.plain
        var prominent = false
        let action: () -> Void
        var id: String { icon }

        init(title: LocalizedStringKey, icon: String, tone: Tone = .plain, prominent: Bool = false, action: @escaping () -> Void) {
            self.title = title; self.icon = icon; self.tone = tone; self.prominent = prominent; self.action = action
        }
    }

    let busy: Bool
    let buttons: [Item]
    @Environment(\.dynamicTypeSize) private var typeSize

    var body: some View {
        let layout = typeSize.isAccessibilitySize ? AnyLayout(VStackLayout(spacing: 10)) : AnyLayout(HStackLayout(spacing: 10))
        layout {
            ForEach(buttons) { b in
                if b.prominent {
                    Button(action: b.action) { label(b) }.buttonStyle(b.tone == .positive ? .pillPositiveFilled : .pillProminent)
                } else {
                    Button(action: b.action) { label(b) }.buttonStyle(b.tone == .negative ? .pillNegative : b.tone == .warning ? .pillWarning : b.tone == .positive ? .pillPositive : .pill)
                }
            }
        }
        .labelStyle(.titleAndIcon)
        .disabled(busy)
    }

    private func label(_ b: Item) -> some View {
        Label(b.title, systemImage: b.icon).font(.headline)
            .lineLimit(2).minimumScaleFactor(0.8)
            .frame(maxWidth: .infinity, minHeight: 44)
    }
}
