import SwiftUI

struct HomeView: View {
    @Environment(AppSettings.self) private var settings
    @State private var notes: [NoteSummary] = []
    @State private var query = ""
    @State private var showRecorder = false
    @State private var showSettings = false
    @State private var showConsent = false
    @State private var pipeline = ProcessingPipeline.shared
    @State private var models = ModelManager.shared

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                BrandHeader()
                if pipeline.isBusy || isFailed {
                    StatusBanner(stage: pipeline.stage)
                }
                if !models.selectedInstalled && !pipeline.isBusy {
                    ModelNudge { showSettings = true }
                }
                List {
                    if notes.isEmpty {
                        ContentUnavailableView(
                            query.isEmpty ? "No notes yet" : "Nothing found",
                            systemImage: query.isEmpty ? "waveform" : "magnifyingglass",
                            description: Text(query.isEmpty ? "Tap Record to capture a lecture, a meeting or a thought. Notes are written on this phone and never leave it." : "Try a different word."))
                            .listRowSeparator(.hidden)
                    }
                    ForEach(notes) { n in
                        NavigationLink(value: n.id) { NoteRow(note: n, isActive: pipeline.activeNoteID == n.id && pipeline.isBusy) }
                    }
                    .onDelete(perform: delete)
                }
                .listStyle(.plain)
                .searchable(text: $query, prompt: "Search your notes")
                .onChange(of: query) { _, _ in Task { await reload() } }

                RecordButton { startRecordingFlow() }
                    .padding(.vertical, 12)
                    .background(.bar)
            }
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: Int64.self) { NoteDetailView(noteID: $0) }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showSettings = true } label: { Image(systemName: "gearshape") }
                }
            }
            .sheet(isPresented: $showSettings) { SettingsView().environment(settings) }
            .fullScreenCover(isPresented: $showRecorder) { RecordView().environment(settings) }
            .alert("Before you record", isPresented: $showConsent) {
                Button("I understand") { settings.consentAcknowledged = true; showRecorder = true }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("Only record people who know they are being recorded and are fine with it. Recordings and notes stay on this phone; nothing is uploaded.")
            }
            .task { await reload() }
            .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
        }
    }

    private var isFailed: Bool { if case .failed = pipeline.stage { return true } else { return false } }

    private func startRecordingFlow() {
        if settings.consentAcknowledged { showRecorder = true } else { showConsent = true }
    }

    private func reload() async {
        notes = await CoreEngine.shared.notes(query: query)
    }

    private func delete(at offsets: IndexSet) {
        let ids = offsets.map { notes[$0].id }
        Task {
            for id in ids {
                if let n = await CoreEngine.shared.note(id), let p = n.audio_path { try? FileManager.default.removeItem(atPath: p) }
                await CoreEngine.shared.deleteNote(id)
            }
            await reload()
        }
    }
}

struct NoteRow: View {
    let note: NoteSummary
    let isActive: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(note.title).font(.headline).lineLimit(1)
                Spacer()
                if isActive { ProgressView().controlSize(.small) }
            }
            Text(note.preview.isEmpty ? "No transcript yet" : note.preview)
                .font(.subheadline).foregroundStyle(.secondary).lineLimit(2)
            HStack(spacing: 8) {
                Text(note.createdAt, style: .date)
                if note.duration_seconds > 0 { Text("· \(Duration.seconds(note.duration_seconds).formatted(.time(pattern: .minuteSecond)))") }
                Text("· \(note.lang.uppercased())")
                if note.has_audio { Image(systemName: "waveform") }
            }
            .font(.caption).foregroundStyle(.tertiary)
        }
        .padding(.vertical, 4)
    }
}

struct RecordButton: View {
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Image(systemName: "mic.fill").font(.title3)
                Text("Record").font(.title3.weight(.semibold))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
        }
        .buttonStyle(.borderedProminent)
        .buttonBorderShape(.capsule)
        .tint(Brand.record)
        .padding(.horizontal, 24)
    }
}

struct StatusBanner: View {
    let stage: ProcessingPipeline.Stage
    var body: some View {
        HStack(spacing: 10) {
            if case .failed = stage { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
            else { ProgressView().controlSize(.small) }
            Text(stage.label).font(.footnote).lineLimit(2)
            Spacer()
        }
        .padding(.horizontal).padding(.vertical, 8)
        .background(Color(.secondarySystemBackground))
    }
}

struct ModelNudge: View {
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack {
                Image(systemName: "arrow.down.circle.fill")
                Text("Download the AI model once (about 640 MB) to get summaries and notes.").font(.footnote)
                Spacer()
                Image(systemName: "chevron.right").font(.caption)
            }
            .padding(.horizontal).padding(.vertical, 8)
            .background(Color.accentColor.opacity(0.12))
        }
        .buttonStyle(.plain)
    }
}
