// Deep dive (spec §6.2): Topics · People · Timeline. Topics and people are filed by Gemma after each
// recording. RootView owns the Record button and the settings sheet; recordings open by pushing
// NoteDetailView inside this tab's own navigation stack.
import SwiftUI

/// What a topic row pushes. Small and Hashable so it can be a navigation value.
struct TopicRoute: Hashable {
    let id: Int64
    let name: String
    let kind: String
}

struct DeepDiveView: View {
    @Environment(Router.self) private var router
    @State private var pipeline = ProcessingPipeline.shared
    @State private var models = ModelManager.shared
    @State private var segment: Segment = .topics

    enum Segment: Hashable { case topics, people, timeline }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                BrandHeader()
                if pipeline.isBusy || isFailed {
                    StatusBanner(stage: pipeline.stage)
                }
                if !models.selectedInstalled && !pipeline.isBusy {
                    ModelNudge { router.showSettings = true }
                }
                Picker("Browse by", selection: $segment) {
                    Text("Topics").tag(Segment.topics)
                    Text("People").tag(Segment.people)
                    Text("Timeline").tag(Segment.timeline)
                }
                .pickerStyle(.segmented)
                .padding(.horizontal).padding(.vertical, 8)

                switch segment {
                case .topics: TopicListView(kind: "topic").id("topic")
                case .people: TopicListView(kind: "person").id("person")
                case .timeline: TimelineView()
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: Int64.self) { NoteDetailView(noteID: $0) }
            .navigationDestination(for: TopicRoute.self) { TopicDetailView(route: $0) }
        }
    }

    private var isFailed: Bool { if case .failed = pipeline.stage { return true } else { return false } }
}

// MARK: - topic and person lists

struct TopicListView: View {
    let kind: String   // "topic" | "person"
    @Environment(\.scenePhase) private var scenePhase
    @State private var topics: [Topic] = []
    @State private var loaded = false
    @State private var request: TopicEditRequest?

    private var isPeople: Bool { kind == "person" }

    var body: some View {
        List {
            if loaded && topics.isEmpty {
                ContentUnavailableView(
                    isPeople ? "No people yet" : "No topics yet",
                    systemImage: isPeople ? "person.2" : "tag",
                    description: Text(isPeople
                        ? "People you talk about appear here after your recordings are processed with Gemma 4."
                        : "Topics appear here after your recordings are processed with Gemma 4."))
                    .listRowSeparator(.hidden)
            }
            ForEach(topics) { t in
                HStack(spacing: 4) {
                    NavigationLink(value: TopicRoute(id: t.id, name: t.name, kind: t.kind)) { TopicRow(topic: t) }
                    Menu {
                        Button { request = .rename(t) } label: { Label("Rename", systemImage: "pencil") }
                        Button { request = .merge(t) } label: { Label("Merge into…", systemImage: "arrow.triangle.merge") }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                            .font(.title3)
                            .frame(minWidth: 44, minHeight: 44)
                            .contentShape(Rectangle())
                    }
                    .accessibilityLabel(Text("More options"))
                }
                .contextMenu {
                    Button { request = .rename(t) } label: { Label("Rename", systemImage: "pencil") }
                    Button { request = .merge(t) } label: { Label("Merge into…", systemImage: "arrow.triangle.merge") }
                }
            }
        }
        .listStyle(.plain)
        .topicEditing(request: $request, gateKey: "deepdive.list.\(kind)") { _ in Task { await reload() } }
        .task { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
        .onChange(of: scenePhase) { _, p in if p == .active { Task { await reload() } } }
    }

    private func reload() async {
        topics = await CoreEngine.shared.topics(kind: kind)
        loaded = true
    }
}

private struct TopicRow: View {
    let topic: Topic
    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            VStack(alignment: .leading, spacing: 4) {
                Text(topic.name).font(.headline).lineLimit(2)
                if let last = topic.lastTitle, !last.isEmpty {
                    Text("Latest: \(last)").font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                }
                Text("\(topic.noteCount) recordings").font(.caption).foregroundStyle(.tertiary)
            }
            Spacer(minLength: 0)
            if topic.openTasks > 0 {
                Text("\(topic.openTasks)")
                    .font(.footnote.weight(.bold)).foregroundStyle(.white)
                    .frame(minWidth: 24, minHeight: 24)
                    .padding(.horizontal, 4)
                    .background(Capsule().fill(Brand.record))
                    .accessibilityLabel(Text("\(topic.openTasks) open to-dos"))
            }
        }
        .padding(.vertical, 4)
        .frame(minHeight: 44)
    }
}

// MARK: - rename and merge (shared by the list and the detail screen)

enum TopicEditRequest: Equatable {
    case rename(Topic)
    case merge(Topic)
}

private struct TopicEditing: ViewModifier {
    @Binding var request: TopicEditRequest?
    let gateKey: String
    /// Called after a rename or merge was applied (the list or detail then reloads). `removed` is the topic that no longer exists (merge).
    let onChanged: (Topic?) -> Void

    @State private var renaming: Topic?
    @State private var showRename = false
    @State private var newName = ""
    @State private var merging: Topic?
    @State private var showMergeSheet = false
    @State private var candidates: [Topic] = []
    @State private var choice: Topic?
    @State private var showMergeConfirm = false

    func body(content: Content) -> some View {
        content
            .onChange(of: request) { _, r in
                guard let r else { return }
                request = nil
                switch r {
                case .rename(let t): renaming = t; newName = t.name; showRename = true
                case .merge(let t):
                    merging = t
                    Task {
                        let all = await CoreEngine.shared.topics(kind: t.kind)
                        candidates = all.filter { $0.id != t.id }
                        showMergeSheet = true
                    }
                }
            }
            .alert("Rename", isPresented: $showRename) {
                TextField("Name", text: $newName)
                Button("Save") { saveRename() }
                    .disabled(newName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                Button("Cancel", role: .cancel) { renaming = nil }
            } message: {
                Text("Type a new name.")
            }
            .sheet(isPresented: $showMergeSheet, onDismiss: { merging = nil; choice = nil; candidates = [] }) { mergeSheet }
            .onChange(of: showRename || showMergeSheet) { _, open in PresentationGate.shared.set(gateKey, open) }
            .onDisappear { PresentationGate.shared.set(gateKey, false) }
    }

    private func saveRename() {
        guard let t = renaming else { return }
        let name = newName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return }
        Task {
            await CoreEngine.shared.renameTopic(t.id, name)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            onChanged(nil)
        }
    }

    private var mergeSheet: some View {
        NavigationStack {
            List {
                if candidates.isEmpty {
                    Text("There is nothing else to merge into.").foregroundStyle(.secondary)
                }
                ForEach(candidates) { c in
                    Button {
                        choice = c
                        showMergeConfirm = true
                    } label: {
                        HStack {
                            Text(c.name).foregroundStyle(.primary)
                            Spacer()
                            Text("\(c.noteCount) recordings").font(.caption).foregroundStyle(.secondary)
                        }
                        .frame(minHeight: 44)
                    }
                }
            }
            .navigationTitle(Text("Merge into…"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { showMergeSheet = false } }
            }
            .alert(mergeTitle, isPresented: $showMergeConfirm) {
                Button("Merge", role: .destructive) { performMerge() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("All recordings of \(merging?.name ?? "") move into \(choice?.name ?? ""). You can't undo this.")
            }
        }
    }

    private var mergeTitle: LocalizedStringKey { "Merge \(merging?.name ?? "") into \(choice?.name ?? "")?" }

    private func performMerge() {
        guard let from = merging, let into = choice else { return }
        showMergeSheet = false
        Task {
            await CoreEngine.shared.mergeTopic(from.id, into: into.id)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            onChanged(from)
        }
    }
}

extension View {
    /// Adds the rename alert and the merge picker. Set `request` to start one.
    func topicEditing(request: Binding<TopicEditRequest?>, gateKey: String,
                      onChanged: @escaping (Topic?) -> Void) -> some View {
        modifier(TopicEditing(request: request, gateKey: gateKey, onChanged: onChanged))
    }
}
