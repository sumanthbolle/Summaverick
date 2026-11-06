// Settings → "What Summaverick has learned" (spec §6.4, §7.4): every memory item, grouped, each one
// editable in place and deletable (a visible button as well as a swipe). The person's own edit applies directly.
import SwiftUI

struct LearnedView: View {
    private struct MemoryGroup: Identifiable {
        let kind: String
        let title: LocalizedStringKey
        let empty: LocalizedStringKey
        var id: String { kind }
    }

    private static let groups = [
        MemoryGroup(kind: "vocab", title: "Words to spell this way",
              empty: "No words yet. When you fix a word in a recording, it is added here."),
        MemoryGroup(kind: "preference", title: "Preferences",
              empty: "No preferences yet. They appear when you say Yes to one on Today."),
        MemoryGroup(kind: "fact", title: "Facts",
              empty: "No facts yet. They appear when you say Yes to one on Today."),
    ]

    @State private var items: [MemoryItem] = []
    @State private var editingID: Int64?
    @State private var draft = ""
    @State private var deleting: MemoryItem?
    @State private var errorShown = false
    @State private var deleteFailed = false
    @State private var loaded = false

    var body: some View {
        List {
            Section {
                Text("Summaverick uses these to understand you. Edit or delete anything.")
                    .font(.body).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            ForEach(Self.groups) { g in
                Section {
                    let rows = items.filter { $0.kind == g.kind }
                    if rows.isEmpty && loaded {
                        Text(g.empty).font(.callout).foregroundStyle(.secondary).frame(minHeight: 44, alignment: .leading)
                    }
                    ForEach(rows) { item in row(item) }
                } header: { Text(g.title) }
            }
        }
        .navigationTitle(Text("What Summaverick has learned"))
        .navigationBarTitleDisplayMode(.inline)
        .task { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .confirmationDialog(Text("Delete this?"),
                            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible, presenting: deleting) { item in
            Button("Delete", role: .destructive) { delete(item) }
            Button("Cancel", role: .cancel) {}
        } message: { item in
            Text(verbatim: "“\(item.text)”")
        }
        .alert("Could not save this. Please try again.", isPresented: $errorShown) {
            Button("OK", role: .cancel) {}
        }
        .alert("Could not delete this. Please try again.", isPresented: $deleteFailed) {
            Button("OK", role: .cancel) {}
        }
        .onChange(of: deleting != nil || errorShown || deleteFailed) { _, open in PresentationGate.shared.set("learned", open) }
    }

    @ViewBuilder
    private func row(_ item: MemoryItem) -> some View {
        if editingID == item.id {
            VStack(alignment: .leading, spacing: 8) {
                TextField("Text", text: $draft, axis: .vertical)
                    .font(.body)
                    .textFieldStyle(.roundedBorder)
                    .frame(minHeight: 44)
                    .submitLabel(.done)
                    .onSubmit { saveEdit(item) }
                HStack(spacing: 12) {
                    Button("Cancel") { editingID = nil }
                        .buttonStyle(.bordered)
                        .frame(minHeight: 44)
                    Button("Save") { saveEdit(item) }
                        .buttonStyle(.borderedProminent)
                        .frame(minHeight: 44)
                        .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
            .padding(.vertical, 4)
        } else {
            HStack(spacing: 4) {
                Text(verbatim: item.text)
                    .font(.body)
                    .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                    .fixedSize(horizontal: false, vertical: true)
                Button { startEdit(item) } label: {
                    Image(systemName: "pencil").font(.title3).frame(minWidth: 44, minHeight: 44)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(Text("Edit “\(item.text)”"))
                Button(role: .destructive) { deleting = item } label: {
                    Image(systemName: "trash").font(.title3).frame(minWidth: 44, minHeight: 44)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(Text("Delete “\(item.text)”"))
            }
            .swipeActions {
                Button("Delete", role: .destructive) { deleting = item }
                Button("Edit") { startEdit(item) }.tint(.blue)
            }
        }
    }

    private func startEdit(_ item: MemoryItem) {
        draft = item.text
        editingID = item.id
    }

    private func saveEdit(_ item: MemoryItem) {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        Task {
            if text != item.text {
                guard await CoreEngine.shared.updateMemory(item.id, text) else { errorShown = true; return }
                NotificationCenter.default.post(name: .assistantChanged, object: nil)
            }
            editingID = nil
            await reload()
        }
    }

    private func delete(_ item: MemoryItem) {
        Task {
            guard await CoreEngine.shared.deleteMemory(item.id) else { deleteFailed = true; return }
            if editingID == item.id { editingID = nil }
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            await reload()
        }
    }

    private func reload() async {
        items = await CoreEngine.shared.memory()
        loaded = true
    }
}
