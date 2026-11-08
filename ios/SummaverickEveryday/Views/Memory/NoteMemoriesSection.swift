// "Remembered" on a note: the memories made from this recording, under the chapters. Each row plays its moment
// (▶) and opens the memory's card. Hidden while the note has none.
import SwiftUI

struct NoteMemoriesSection: View {
    let noteID: Int64
    /// ▶: the moment in a recording (note id, second). The note's own moments play in place.
    let onPlay: (Int64, Int) -> Void

    /// Rows shown before "Show all".
    private static let collapsedCount = 3

    @State private var memories: [BrainMemory] = []
    @State private var expanded = false
    @State private var opened: BrainMemory?
    /// ▶ tapped inside the open card: played once the sheet has gone.
    @State private var playAfterClose: (Int64, Int)?

    var body: some View {
        Group {
            if !memories.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Remembered").font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
                        .accessibilityAddTraits(.isHeader)
                    VStack(spacing: 0) {
                        let shown = expanded ? memories : Array(memories.prefix(Self.collapsedCount))
                        ForEach(shown) { m in
                            row(m)
                            if m.id != shown.last?.id { Divider().padding(.leading, 14) }
                        }
                    }
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
                    if memories.count > Self.collapsedCount {
                        Button { withAnimation { expanded.toggle() } } label: {
                            Text(expanded ? "Show fewer" : "Show all \(memories.count)")
                                .font(.subheadline.weight(.semibold)).frame(minHeight: 44)
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(Color.accentColor)
                    }
                }
            }
        }
        .task(id: noteID) { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .sheet(item: $opened, onDismiss: {
            if let p = playAfterClose { playAfterClose = nil; onPlay(p.0, p.1) }
        }) { m in
            NavigationStack {
                ScrollView {
                    MemoryCardView(memoryID: m.id, onPlay: { id, sec in playAfterClose = (id, sec); opened = nil },
                                   onRemoved: { opened = nil })
                        .padding()
                }
                .navigationTitle(Text("Remembered"))
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) { Button("Done") { opened = nil } }
                }
            }
        }
        .onChange(of: opened != nil) { _, open in PresentationGate.shared.set("remembered.\(noteID)", open) }
        .onDisappear { PresentationGate.shared.set("remembered.\(noteID)", false) }
    }

    private func row(_ m: BrainMemory) -> some View {
        let source = m.sources.first(where: { $0.noteID == noteID }) ?? m.sources.first
        return HStack(alignment: .center, spacing: 4) {
            Button { opened = m } label: {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: MemoryFormat.typeIcon(m)).foregroundStyle(Color.accentColor)
                        .frame(width: 24)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: m.text).font(.body).multilineTextAlignment(.leading)
                            .strikethrough(m.done, color: .secondary)
                        HStack(spacing: 6) {
                            Text(MemoryFormat.typeLabel(m.type))
                            if m.waitingForCheck {
                                Text(verbatim: "·")
                                Text("Waiting for you to check").foregroundStyle(.orange)
                            } else if m.review == "new" {
                                Text(verbatim: "·")
                                Text("New")
                            }
                        }
                        .font(.footnote).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.leading, 14).padding(.vertical, 10)
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if let source {
                let stamp = AssistantFormat.mmss(source.startSec)
                Button { onPlay(source.noteID, source.startSec) } label: {
                    Image(systemName: "play.circle").font(.title3).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Color.accentColor)
                .padding(.trailing, 4)
                .accessibilityLabel(Text("Play from \(stamp)"))
            }
        }
    }

    private func reload() async {
        // In the order said in this recording.
        func sec(_ m: BrainMemory) -> Int { m.sources.first(where: { $0.noteID == noteID })?.startSec ?? Int.max }
        memories = await CoreEngine.shared.memories(MemoryFilter(noteID: noteID))
            .enumerated().sorted { (sec($0.element), $0.offset) < (sec($1.element), $1.offset) }.map(\.element)
    }
}
