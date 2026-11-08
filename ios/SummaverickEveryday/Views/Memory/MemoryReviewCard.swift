// A memory waiting in the review queue ("To check" on Today), as a card with Keep / Edit / Wrong: the same
// pattern as SuggestionCard's Yes / Edit / No. A pending reminder or to-do made from the same words shows inside
// the card as one row with its own Yes / Edit / No, so one thing said is one card.
// The card waits for the core before it goes: a failed change keeps it, with a short message.
import SwiftUI

struct MemoryReviewCard: View {
    let item: ReviewItem
    /// The linked suggestion's Yes / No is running (TodayView owns it).
    let suggestionBusy: Bool
    /// The linked suggestion changed since it was suggested (accept came back re-flagged).
    let suggestionStale: Bool
    let onAcceptSuggestion: (Suggestion, SuggestionPayload?) -> Void
    let onRejectSuggestion: (Suggestion) -> Void
    /// ▶: open the recording (note id, second).
    let onPlay: (Int64, Int) -> Void

    @State private var busy = false
    @State private var failed = false
    @State private var editing = false

    private var m: BrainMemory { item.memory }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if m.health { healthBanner }
            MemoryDetails(memory: m, onPlay: onPlay, showsWaiting: false)
            if failed {
                Label("Could not save this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !m.isMedicine {
                MemoryActionButtons(busy: busy, buttons: [
                    .init(title: "Keep", icon: "checkmark", tint: .green, prominent: true) {
                        Task { await run { await CoreEngine.shared.reviewMemory(m.id, true) } }
                    },
                    .init(title: "Edit", icon: "pencil", tint: nil) { failed = false; editing = true },
                    .init(title: "Wrong", icon: "xmark", tint: .red) {
                        Task { await run { await CoreEngine.shared.reviewMemory(m.id, false) } }
                    },
                ])
            }
            if item.showsSuggestionRow, let s = item.suggestion { suggestionRow(s) }
        }
        .padding(16)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(m.health ? Color.orange : Color.clear, lineWidth: 2))
        .overlay(alignment: .topTrailing) { if busy { ProgressView().padding(12) } }
        .sheet(isPresented: $editing) {
            MemoryEditSheet(memory: m) { text in await run { await CoreEngine.shared.editMemory(m.id, text) } }
        }
        .onChange(of: editing) { _, open in PresentationGate.shared.set("review.\(m.id)", open) }
        .onDisappear { PresentationGate.shared.set("review.\(m.id)", false) }
    }

    private var healthBanner: some View {
        Label("Health-related: please check", systemImage: "heart.text.square.fill")
            .font(.headline)
            .foregroundStyle(.primary)
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.yellow.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color.orange, lineWidth: 1.5))
    }

    // MARK: the linked reminder or to-do

    private func suggestionRow(_ s: Suggestion) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Divider()
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: s.kind == .task ? "checklist" : (s.isMedicine ? "pills.fill" : "bell.fill"))
                    .foregroundStyle(s.isMedicine ? Color.red : Color.accentColor)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.kind == .task ? "Make a task" : "Remind me").font(.headline)
                    if let summary = summary(s) {
                        Text(verbatim: summary).font(.body).foregroundStyle(.secondary)
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
                    .background(Color.orange.opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
            }
            SuggestionDecisionButtons(suggestion: s, noteTitle: s.noteID == m.sources.first?.noteID ? m.sources.first?.title : nil,
                                      isBusy: suggestionBusy,
                                      onAccept: { onAcceptSuggestion(s, $0) }, onReject: { onRejectSuggestion(s) })
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
