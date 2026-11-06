import AVFoundation
import SwiftUI

struct NoteDetailView: View {
    let noteID: Int64
    @State private var note: Note?
    @State private var tab: Tab = .summary
    @State private var title = ""
    @State private var question = ""
    @State private var answer: String?
    @State private var asking = false
    @State private var pipeline = ProcessingPipeline.shared
    @State private var player: AVAudioPlayer?
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss

    enum Tab: String, CaseIterable, Identifiable {
        case summary = "Summary", notes = "Notes", questions = "Questions", transcript = "Transcript"
        var id: String { rawValue }
    }

    private var isThisNoteBusy: Bool { pipeline.isBusy && pipeline.activeNoteID == noteID }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                TextField("Title", text: $title, axis: .vertical)
                    .font(.title2.weight(.semibold))
                    .onSubmit { saveTitle() }

                if let n = note {
                    HStack(spacing: 8) {
                        Text(n.createdAt, format: .dateTime.day().month().year().hour().minute())
                        if n.duration_seconds > 0 { Text("· \(Duration.seconds(n.duration_seconds).formatted(.time(pattern: .minuteSecond)))") }
                        Text("· \(n.lang.uppercased())")
                        if let p = n.audio_path, FileManager.default.fileExists(atPath: p) {
                            Button { togglePlay(p) } label: { Image(systemName: player?.isPlaying == true ? "stop.circle" : "play.circle") }
                        }
                    }
                    .font(.caption).foregroundStyle(.secondary)
                }

                if isThisNoteBusy {
                    StatusBanner(stage: pipeline.stage).clipShape(RoundedRectangle(cornerRadius: 8))
                } else if let failure = pipeline.lastFailure[noteID] {
                    StatusBanner(stage: .failed(failure)).clipShape(RoundedRectangle(cornerRadius: 8))
                }

                Picker("Section", selection: $tab) {
                    ForEach(Tab.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)

                content

                if needsGeneration && !isThisNoteBusy {
                    Button {
                        Task { await pipeline.regenerate(noteID: noteID) }
                    } label: { Label("Generate notes", systemImage: "sparkles").frame(maxWidth: .infinity) }
                    .buttonStyle(.borderedProminent)
                    .disabled(pipeline.isBusy || !(hasTranscript || hasAudio))
                }

                askSection
            }
            .padding()
        }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                if let text = shareText { ShareLink(item: text, subject: Text(title)) }
                Menu {
                    if let url = exportFileURL() { ShareLink("Export as Markdown file", item: url) }
                    Button(role: .destructive) { confirmDelete = true } label: { Label("Delete note", systemImage: "trash") }
                } label: { Image(systemName: "ellipsis.circle") }
            }
        }
        .confirmationDialog("Delete this note and its recording?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { deleteNote() }
        }
        .task { await load() }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await load() } }
        .onDisappear { saveTitle(); player?.stop() }
    }

    @ViewBuilder private var content: some View {
        let text: String? = {
            guard let n = note else { return nil }
            switch tab {
            case .summary: return n.short_summary
            case .notes: return n.detailed_notes
            case .questions: return n.revision_questions
            case .transcript: return n.transcript
            }
        }()
        if let text, !text.isEmpty {
            Text(markdown(text))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        } else {
            Text(isThisNoteBusy ? "Being written…" : (tab == .transcript ? "No transcript." : "Not generated yet."))
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.vertical, 24)
        }
    }

    private var askSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Divider().padding(.vertical, 8)
            Text("Ask about this recording").font(.headline)
            HStack {
                TextField("e.g. What was the homework?", text: $question)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { ask() }
                Button { ask() } label: { Image(systemName: "paperplane.fill") }
                    .disabled(question.isEmpty || asking || pipeline.isBusy)
            }
            if asking { ProgressView().controlSize(.small) }
            if let a = answer { Text(a).textSelection(.enabled).padding(.top, 4) }
        }
    }

    private var hasTranscript: Bool { !(note?.transcript ?? "").isEmpty }
    private var hasAudio: Bool {
        guard let p = note?.audio_path else { return false }
        return FileManager.default.fileExists(atPath: p)
    }

    private var needsGeneration: Bool {
        guard let n = note else { return false }
        return n.short_summary == nil || n.detailed_notes == nil || n.revision_questions == nil
    }

    private var shareText: String? {
        guard let n = note else { return nil }
        var s = "# \(n.title)\n\n"
        if let x = n.short_summary { s += "## Summary\n\(x)\n\n" }
        if let x = n.detailed_notes { s += x + "\n\n" }
        if let x = n.revision_questions { s += "## Revision questions\n\(x)\n\n" }
        s += "— Made with Summaverick Everyday, on this device."
        return s
    }

    private func exportFileURL() -> URL? {
        guard let text = shareText else { return nil }
        let safe = title.replacingOccurrences(of: "/", with: "-").prefix(60)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(safe).md")
        try? text.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    private func markdown(_ s: String) -> AttributedString {
        // Keep headings and bullets readable; fall back to plain text if parsing fails.
        var opts = AttributedString.MarkdownParsingOptions()
        opts.interpretedSyntax = .inlineOnlyPreservingWhitespace
        let cleaned = s.replacingOccurrences(of: "## ", with: "**").replacingOccurrences(of: "\n**", with: "\n**")
            .split(separator: "\n", omittingEmptySubsequences: false)
            .map { line -> String in
                var l = String(line)
                if l.hasPrefix("**") && !l.hasSuffix("**") { l += "**" }
                if l.hasPrefix("- ") { l = "• " + l.dropFirst(2) }
                return l
            }
            .joined(separator: "\n")
        return (try? AttributedString(markdown: cleaned, options: opts)) ?? AttributedString(s)
    }

    private func load() async {
        let previous = note
        note = await CoreEngine.shared.note(noteID)
        guard let n = note else { return }
        // Adopt the stored title unless the user has typed their own.
        let userEdited = previous != nil && title != previous!.title
        if !userEdited { title = n.title }
    }

    private func saveTitle() {
        let t = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let n = note, !t.isEmpty, t != n.title else { return }
        note?.title = t
        Task { await CoreEngine.shared.setTitle(noteID, t); NotificationCenter.default.post(name: .notesChanged, object: nil) }
    }

    private func ask() {
        guard let n = note, let t = n.transcript, !question.isEmpty else { return }
        let q = question
        asking = true
        answer = nil
        Task {
            defer { asking = false }
            let mm = ModelManager.shared
            guard mm.selectedInstalled else { answer = "Download the AI model in Settings first."; return }
            do {
                try await CoreEngine.shared.loadModel(at: mm.fileURL(for: mm.selected).path)
                let context = [n.detailed_notes, t].compactMap { $0 }.joined(separator: "\n\n")
                answer = try await CoreEngine.shared.ask(context: context, question: q, lang: n.lang)
            } catch {
                answer = error.localizedDescription
            }
        }
    }

    private func togglePlay(_ path: String) {
        if let p = player, p.isPlaying { p.stop(); player = nil; return }
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        player = try? AVAudioPlayer(contentsOf: URL(fileURLWithPath: path))
        player?.play()
    }

    private func deleteNote() {
        Task {
            if let p = note?.audio_path { try? FileManager.default.removeItem(atPath: p) }
            await CoreEngine.shared.deleteNote(noteID)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            dismiss()
        }
    }
}
