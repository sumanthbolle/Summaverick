// One topic or person: a combined summary of its recordings, key terms, a quiz, open to-dos and the
// recordings themselves. The summary is written by Gemma and cached by the core.
import SwiftUI

struct TopicDetailView: View {
    let route: TopicRoute
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @Environment(Router.self) private var router
    @Environment(AppSettings.self) private var settings

    @State private var models = ModelManager.shared
    @State private var name: String
    @State private var topic: Topic?
    @State private var notes: [TopicNote] = []
    @State private var tasks: [TaskItem] = []
    @State private var summary: TopicSummary?
    @State private var loadingSummary = false
    @State private var summaryError: String?
    @State private var summaryBusy = false
    @State private var pipeline = ProcessingPipeline.shared
    @State private var revealed: Set<Int> = []
    @State private var request: TopicEditRequest?

    init(route: TopicRoute) {
        self.route = route
        _name = State(initialValue: route.name)
    }

    private var canSummarise: Bool { models.isReady }

    var body: some View {
        List {
            summarySection
            if let s = summary, !s.keyTerms.isEmpty { keyTermsSection(s.keyTerms) }
            if let s = summary, !s.quiz.isEmpty { quizSection(s.quiz) }
            if !tasks.isEmpty { tasksSection }
            recordingsSection
        }
        .navigationTitle(Text(verbatim: name))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    if let t = currentTopic {
                        Button { request = .rename(t) } label: { Label("Rename", systemImage: "pencil") }
                        Button { request = .merge(t) } label: { Label("Merge into…", systemImage: "arrow.triangle.merge") }
                    }
                } label: {
                    Image(systemName: "ellipsis.circle").frame(minWidth: 44, minHeight: 44)
                }
                .accessibilityLabel(Text("More options"))
            }
        }
        .topicEditing(request: $request, gateKey: "deepdive.topic.\(route.id)") { removed in
            if removed?.id == route.id { dismiss() } else { Task { await reload() } }
        }
        .task {
            await reload()
            if summary == nil { await loadSummary() }   // first open only; later changes need a tap
        }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
        .onChange(of: scenePhase) { _, p in if p == .active { Task { await reload() } } }
    }

    /// The topic as the list last loaded it (nil until the first load).
    private var currentTopic: Topic? { topic }

    // MARK: sections

    @ViewBuilder
    private var summarySection: some View {
        Section {
            if !canSummarise {
                AIGettingReadyPanel()
                    .listRowInsets(EdgeInsets())
            } else if let s = summary {
                VStack(alignment: .leading, spacing: 10) {
                    if loadingSummary {
                        HStack(spacing: 12) {
                            ProgressView()
                            Text("Reading your recordings…").foregroundStyle(.secondary)
                        }
                        .frame(minHeight: 44)
                    } else if summaryBusy {
                        busyView
                    } else if let err = summaryError {
                        VStack(alignment: .leading, spacing: 6) {
                            Label("I couldn't write the summary again.", systemImage: "exclamationmark.triangle.fill")
                                .font(.subheadline).foregroundStyle(.orange)
                            if err == tooWarmMessage || err == recordingMessage {
                                Text(verbatim: err).font(.footnote).foregroundStyle(.secondary)
                            }
                            Button("Try again") { Task { await loadSummary() } }
                                .buttonStyle(.bordered).frame(minHeight: 44)
                        }
                    } else if topic?.summaryStale == 1 {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("New recordings were added.").font(.subheadline).foregroundStyle(.secondary)
                            Button("Write the summary again") { Task { await loadSummary() } }
                                .buttonStyle(.bordered).frame(minHeight: 44)
                        }
                    }
                    if s.summary.isEmpty {
                        Text("No summary yet.").foregroundStyle(.secondary)
                    } else {
                        Text(verbatim: s.summary).font(.body)
                    }
                    ForEach(s.sources) { src in
                        NavigationLink(value: NoteMoment(noteID: src.noteID, startSec: src.startSec)) {
                            Text(verbatim: "▶ \(src.title.isEmpty ? String(localized: "Recording") : src.title) @\(AssistantFormat.mmss(src.startSec))")
                                .font(.subheadline).foregroundStyle(Color.accentColor)
                                .frame(minHeight: 44, alignment: .leading)
                        }
                        .accessibilityLabel(Text("Open the recording: \(src.title.isEmpty ? String(localized: "Recording") : src.title) at \(AssistantFormat.mmss(src.startSec))"))
                    }
                }
                .padding(.vertical, 4)
            } else if let err = summaryError {
                VStack(alignment: .leading, spacing: 10) {
                    Label("I couldn't write the summary.", systemImage: "exclamationmark.triangle.fill").font(.headline)
                    Text(verbatim: err).font(.footnote).foregroundStyle(.secondary)
                    Button("Try again") { Task { await loadSummary() } }
                        .buttonStyle(.borderedProminent).frame(minHeight: 44)
                }
                .padding(.vertical, 4)
            } else if summaryBusy {
                busyView
            } else if loadingSummary {
                HStack(spacing: 12) {
                    ProgressView()
                    Text("Reading your recordings…").foregroundStyle(.secondary)
                }
                .frame(minHeight: 44)
            } else {
                Button("Write the summary") { Task { await loadSummary() } }
                    .buttonStyle(.borderedProminent).frame(minHeight: 44)
            }
        } header: {
            Text("Summary")
        }
    }

    private var busyView: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("I'm working on another note. Try again in a moment.").font(.subheadline).foregroundStyle(.secondary)
            Button("Try again") { Task { await loadSummary() } }
                .buttonStyle(.bordered).frame(minHeight: 44)
        }
    }

    private func keyTermsSection(_ terms: [KeyTerm]) -> some View {
        Section("Key terms") {
            ForEach(terms, id: \.self) { t in
                (Text(verbatim: t.term).bold() + Text(verbatim: " — \(t.meaning)"))
                    .font(.body).padding(.vertical, 2)
            }
        }
    }

    private func quizSection(_ quiz: [QuizItem]) -> some View {
        Section {
            ForEach(Array(quiz.enumerated()), id: \.offset) { i, item in
                let open = revealed.contains(i)
                Button {
                    if open { revealed.remove(i) } else { revealed.insert(i) }
                } label: {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack(alignment: .top) {
                            Text(verbatim: item.q).font(.body.weight(.semibold)).foregroundStyle(.primary)
                                .multilineTextAlignment(.leading)
                            Spacer(minLength: 8)
                            Image(systemName: open ? "chevron.up" : "chevron.down")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                        if open {
                            Text(verbatim: item.a).font(.body).foregroundStyle(.secondary)
                                .multilineTextAlignment(.leading)
                        }
                    }
                    .frame(minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityHint(Text(open ? "Hides the answer" : "Shows the answer"))
            }
        } header: {
            Text("Quiz")
        } footer: {
            Text("Tap a question to see the answer.")
        }
    }

    private var tasksSection: some View {
        Section("Open to-dos") {
            ForEach(tasks) { t in
                let row = VStack(alignment: .leading, spacing: 2) {
                    Text(verbatim: t.text).font(.body)
                    if let due = t.dueDate, !due.isEmpty {
                        Text("Due: \(AssistantFormat.day(due))").font(.caption).foregroundStyle(.secondary)
                    }
                }
                .frame(minHeight: 44, alignment: .leading)
                if let id = t.noteID, !t.fromDeleted {
                    NavigationLink(value: NoteMoment(noteID: id, startSec: t.startSec)) { row }
                } else {
                    row
                }
            }
        }
    }

    private var recordingsSection: some View {
        Section("Recordings") {
            ForEach(notes) { n in
                NavigationLink(value: n.id) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(verbatim: n.title).font(.headline).lineLimit(2)
                        if let s = n.shortSummary, !s.isEmpty {
                            Text(verbatim: s).font(.subheadline).foregroundStyle(.secondary).lineLimit(2)
                        }
                        if let c = n.createdAt {
                            Text(Date(timeIntervalSince1970: TimeInterval(c)), style: .date)
                                .font(.caption).foregroundStyle(.tertiary)
                        }
                    }
                    .frame(minHeight: 44, alignment: .leading)
                }
            }
        }
    }

    // MARK: loading

    private func reload() async {
        let e = CoreEngine.shared
        let all = await e.topics(kind: route.kind)
        guard let t = all.first(where: { $0.id == route.id }) else { dismiss(); return }   // merged away
        topic = t
        name = t.name
        notes = await e.topicNotes(route.id)
        let ids = Set(notes.map(\.id))
        tasks = await e.tasks(openOnly: true).filter { $0.noteID.map(ids.contains) ?? false }
    }

    private var tooWarmMessage: String { String(localized: "The phone is too warm. Try again when it has cooled down.") }
    private var recordingMessage: String { String(localized: "Recording now. Ask again after you stop.") }

    /// Only called on first open and from the Refresh / Try again buttons: making a summary occupies the
    /// core for a while, so background changes never trigger it. An old summary stays while a new one is made.
    private func loadSummary() async {
        guard canSummarise, !loadingSummary else { return }
        summaryBusy = false
        // Live transcription needs the model, piece after piece: a summary would hold it.
        if LiveTranscriber.shared.liveRunning { summaryError = recordingMessage; return }
        if pipeline.isBusy { summaryBusy = true; return }
        guard await pipeline.yieldMemories(), !pipeline.isWorking else { summaryBusy = true; return }
        loadingSummary = true
        defer { loadingSummary = false }
        summaryError = nil
        // Writing the summary is a model run: not while the phone is too hot.
        guard !Heat.isTooHot(ProcessInfo.processInfo.thermalState) else { summaryError = tooWarmMessage; return }
        do {
            try await ModelManager.shared.loadIntoCore()   // the backend for right now (GPU in front), never left on the CPU
            guard !pipeline.isWorking else { summaryBusy = true; return }
            guard !LiveTranscriber.shared.liveRunning else { summaryError = recordingMessage; return }
            let s = try await CoreEngine.shared.topicSummary(route.id, lang: settings.notesLanguage)
            if Task.isCancelled { return }
            if s != summary { revealed = [] }
            summary = s
            topic?.summaryStale = 0   // the core cleared it
        } catch {
            if Task.isCancelled { return }
            summaryError = error.localizedDescription
        }
    }
}
