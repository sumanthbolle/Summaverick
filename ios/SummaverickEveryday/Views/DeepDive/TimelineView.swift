import SwiftUI

/// The Timeline segment of Deep dive: every recording, newest first, with search and swipe to delete.
/// Deep dive owns the navigation stack, the Record button lives in RootView.
struct TimelineView: View {
    @Environment(\.scenePhase) private var scenePhase
    @State private var notes: [NoteSummary] = []
    @State private var query = ""
    @State private var pipeline = ProcessingPipeline.shared

    var body: some View {
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
        .task { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .onChange(of: scenePhase) { _, p in if p == .active { Task { await reload() } } }
    }

    private func reload() async {
        notes = await CoreEngine.shared.notes(query: query)
    }

    private func delete(at offsets: IndexSet) {
        let ids = offsets.map { notes[$0].id }
        Task {
            for id in ids {
                ProcessingPipeline.shared.forget(noteID: id)
                if let n = await CoreEngine.shared.note(id), let p = AudioRecorder.existingAudioPath(n.audio_path) { AudioRecorder.removeAudio(atPath: p) }
                await CoreEngine.shared.deleteNote(id)
            }
            await reload()
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)   // resync notifications now
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

struct StatusBanner: View {
    let stage: ProcessingPipeline.Stage
    /// Waiting for the charger: "Do it now" runs the note at once.
    var onDoItNow: (() -> Void)? = nil
    /// A regenerate running: Stop cancels it (the old text stays).
    var onStop: (() -> Void)? = nil
    /// Failed or paused: Try again runs the note now. Disabled while another note is running.
    var onTryAgain: (() -> Void)? = nil
    var tryAgainDisabled = false
    var body: some View {
        HStack(spacing: 10) {
            switch stage {
            case .failed: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            case .paused: Image(systemName: "thermometer.medium").foregroundStyle(.orange)
            case .overnight: Image(systemName: "moon.zzz.fill").foregroundStyle(.indigo)
            default: ProgressView().controlSize(.small)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(stage.label).font(.footnote).lineLimit(2)
                if stage.isWorking {
                    Text("Keep Summaverick open until your notes are written.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            if let onDoItNow {
                Button("Do it now", action: onDoItNow)
                    .buttonStyle(.bordered)
                    .font(.footnote.weight(.semibold))
                    .frame(minHeight: 44)
            }
            if let onStop {
                Button("Stop", action: onStop)
                    .buttonStyle(.bordered)
                    .font(.footnote.weight(.semibold))
                    .frame(minHeight: 44)
            }
            if let onTryAgain {
                Button("Try again", action: onTryAgain)
                    .buttonStyle(.bordered)
                    .font(.footnote.weight(.semibold))
                    .frame(minHeight: 44)
                    .disabled(tryAgainDisabled)
            }
        }
        .padding(.horizontal).padding(.vertical, 8)
        .background(Color(.secondarySystemBackground))
    }
}
