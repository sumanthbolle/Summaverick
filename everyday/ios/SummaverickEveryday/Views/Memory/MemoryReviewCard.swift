// A memory waiting in the review queue ("To check" on Today), as a card with Keep / Edit / Wrong: the same
// pattern as SuggestionCard's Yes / Edit / No. A pending reminder or to-do made from the same words shows inside
// the card as one row with its own Yes / Edit / No, so one thing said is one card and one decision
// (ReviewItem.linkedDecision): Yes on the row also keeps the memory; Wrong also declines the row.
// The card waits for the core before it goes: a failed change keeps it, with a short message.
import SwiftUI

struct MemoryReviewCard: View {
    let item: ReviewItem
    /// The linked suggestion's Yes / No is running (TodayView owns it).
    let suggestionBusy: Bool
    /// The linked suggestion changed since it was suggested (accept came back re-flagged).
    let suggestionStale: Bool
    /// Accepts the linked suggestion (nil payload: as proposed). TodayView owns it (reminders, notifications).
    let onAcceptSuggestion: (Suggestion, SuggestionPayload?) async -> SuggestionStepResult
    /// Declines the linked suggestion; false when it failed.
    let onRejectSuggestion: (Suggestion) async -> Bool
    /// ▶: open the recording (note id, second).
    let onPlay: (Int64, Int) -> Void

    @State private var busy = false
    @State private var failed = false
    @State private var editing = false

    private var m: BrainMemory { item.memory }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            MemoryDetails(memory: m, onPlay: onPlay, showsWaiting: false)
            if failed {
                Label("I couldn't save this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !m.isMedicine {
                MemoryActionButtons(busy: busy, buttons: [
                    .init(title: "Keep", icon: "checkmark", tone: .positive, prominent: true) { decide(.keep) },
                    .init(title: "Edit", icon: "pencil") { failed = false; editing = true },
                    .init(title: "Wrong", icon: "xmark", tone: .negative) { decide(.wrong) },
                ])
            }
            if item.showsSuggestionRow, let s = item.suggestion { suggestionRow(s) }
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(m.health ? Theme.healthIcon : Color.clear, lineWidth: 2))
        .overlay(alignment: .topTrailing) { if busy { ProgressView().padding(12) } }
        .sheet(isPresented: $editing) {
            MemoryEditSheet(memory: m) { text in await run { await CoreEngine.shared.editMemory(m.id, text) } }
        }
        .onChange(of: editing) { _, open in PresentationGate.shared.set("review.\(m.id)", open) }
        .onDisappear { PresentationGate.shared.set("review.\(m.id)", false) }
    }

    // MARK: the linked reminder or to-do

    private func suggestionRow(_ s: Suggestion) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Divider()
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: s.kind == .task ? "checklist" : (s.isMedicine ? "pills.fill" : "bell.fill"))
                    .foregroundStyle(s.isMedicine ? Theme.medicineIcon : Theme.accent)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.kind == .task ? "Make a task" : "Remind me").font(.headline)
                    if let summary = summary(s) {
                        Text(verbatim: summary).font(.body).foregroundStyle(Theme.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Spacer(minLength: 0)
                if suggestionBusy { ProgressView() }
            }
            .accessibilityElement(children: .combine)
            if s.isFlagged { PleaseCheckBanner(fields: s.checkFields) }
            if suggestionStale {
                Label("This changed since it was suggested. Please check it again.", systemImage: "arrow.triangle.2.circlepath")
                    .font(.callout.weight(.medium))
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Theme.cautionFill, in: RoundedRectangle(cornerRadius: 10))
            }
            SuggestionDecisionButtons(suggestion: s, noteTitle: s.noteID == m.sources.first?.noteID ? m.sources.first?.title : nil,
                                      isBusy: suggestionBusy || busy,
                                      onAccept: { decide(.suggestionYes(edited: $0 != nil), payload: $0) },
                                      onReject: { decide(.suggestionNo) },
                                      mustConfirm: suggestionStale)
        }
    }

    /// "Call Ravi · 9:00 AM · Every day" or "Send the form · Due Saturday 7 November".
    private func summary(_ s: Suggestion) -> String? {
        let p = s.payload
        var parts: [String] = []
        if let t = p.text?.trimmingCharacters(in: .whitespacesAndNewlines), !t.isEmpty { parts.append(t) }
        if let times = p.times, !times.isEmpty { parts.append(times.map(AssistantFormat.time).joined(separator: ", ")) }
        if let r = AssistantFormat.repeatLabel(p.repeat) { parts.append(r) }
        if let d = p.dueDate { parts.append(String(localized: "Due \(AssistantFormat.day(d))")) }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    /// Runs what one tap decides: the linked suggestion first, then the memory. A failed step stops there with the
    /// inline message and the card stays; a re-flagged accept stops quietly (the row says it changed).
    private func decide(_ tap: ReviewTap, payload: SuggestionPayload? = nil) {
        guard !busy else { return }
        let plan = item.linkedDecision(tap)
        let memoryID = m.id
        let linked = item.suggestion
        busy = true
        failed = false
        Task {
            let r = await plan.perform(
                suggestionStep: { step in
                    guard let s = linked else { return .failed }
                    switch step {
                    case .accept: return await onAcceptSuggestion(s, payload)
                    case .reject: return await onRejectSuggestion(s) ? .done : .failed
                    }
                },
                memoryStep: { review in await CoreEngine.shared.reviewMemory(memoryID, review == "kept") },
                removeStep: { await CoreEngine.shared.removeMemory(memoryID) })
            busy = false
            failed = r.failed
            // Also after a re-flagged accept: the row must show the new check fields before the next Yes.
            if r.needsReload { NotificationCenter.default.post(name: .assistantChanged, object: nil) }
        }
    }

    /// One change at a time; on success the app reloads (the card leaves the queue), on failure it stays.
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
}
