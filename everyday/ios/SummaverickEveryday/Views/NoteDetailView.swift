// One recording: title, summary / notes / questions / transcript, playback, and a question box.
// Opened from source links at a moment (startAt): playback starts there and the transcript scrolls to
// that piece. The transcript is shown in pieces with m:ss stamps; long-press a piece to fix a word.
import AVFoundation
import SwiftUI
import UIKit

struct NoteDetailView: View {
    let noteID: Int64
    /// The second to open at (a source link, a reminder). nil = the start.
    let startAt: Int?

    @State private var note: Note?
    @State private var speed: SpeedRecord?
    @State private var check: NoteCheck.Report?
    /// The outputs are stored and their check has not finished (ProcessingPipeline.checksDue).
    @State private var checksPending = false
    enum MapMode: Hashable { case flow, graph }
    @State private var mapMode: MapMode = .flow
    @State private var loaded = false
    @State private var pieces: [TranscriptPiece] = []
    @State private var chapters: [Chapter] = []
    /// Parts of the notes and of the questions that failed 3 times ("I couldn't write part %d. Try again").
    @State private var partsLeft: [NoteSteps.Kind: [Int]] = [:]
    /// What is left to write (pieces, section summaries, parts), for "Finish now · about %d min".
    @State private var workLeft = FinishNow.Work(durationSeconds: 0)
    @State private var tab: Tab
    @State private var title = ""
    @State private var question = ""
    @State private var answer: String?
    @State private var asking = false
    @State private var pipeline = ProcessingPipeline.shared
    @State private var models = ModelManager.shared
    @State private var player: AVAudioPlayer?
    @State private var playing = false
    /// startAt has not been applied to playback yet (it is, on the first play).
    @State private var seekPending: Bool
    /// Index of the highlighted transcript piece (the source moment, or the last stamp tapped).
    @State private var highlighted: Int?
    @State private var scrollRequest: Int?
    @State private var didScrollToStart = false
    @State private var confirmDelete = false
    @State private var fixing: FixTarget?
    @State private var shareItem: ShareItem?
    @State private var errorMessage: String?
    @State private var toast: String?
    @State private var gateKey = "note.\(UUID().uuidString)"
    /// Extraction has run on this note (true until loaded, so the button never flashes).
    @State private var extracted = true
    /// What the last successful regenerate replaced, for Undo; kept until the note is left.
    @State private var undo: ProcessingPipeline.Previous?
    /// A memory's ▶ for another recording: opened on top of this one.
    @State private var openMoment: NoteMoment?
    @Environment(\.dismiss) private var dismiss

    enum Tab: String, CaseIterable, Identifiable {
        case summary, notes, questions, transcript, map
        var id: String { rawValue }
        var title: LocalizedStringKey {
            switch self {
            case .summary: return "Summary"
            case .notes: return "Notes"
            case .questions: return "Questions"
            case .transcript: return "Word for word"
            case .map: return "Map"
            }
        }
        /// What VoiceOver says: the full name where the control shows a short one.
        var accessibilityTitle: LocalizedStringKey { self == .map ? "Flow and knowledge graph" : title }
        /// The output this tab shows; nil for the transcript.
        var kind: OutputKind? {
            switch self {
            case .summary: return .shortSummary
            case .notes: return .detailedNotes
            case .questions: return .revisionQuestions
            case .transcript, .map: return nil
            }
        }
    }

    init(noteID: Int64, startAt: Int? = nil) {
        self.noteID = noteID
        self.startAt = startAt
        _tab = State(initialValue: startAt == nil ? .summary : .transcript)
        _seekPending = State(initialValue: startAt != nil)
    }

    private var isThisNoteBusy: Bool { pipeline.isBusy && pipeline.activeNoteID == noteID }
    /// A regenerate of this note is running: its progress and Stop show under the text.
    private var isRegeneratingHere: Bool { isThisNoteBusy && pipeline.regenerating?.noteID == noteID }
    private var presenting: Bool { confirmDelete || fixing != nil || shareItem != nil || errorMessage != nil }

    var body: some View {
        Group {
            if loaded && note == nil {
                ContentUnavailableView("This came from a deleted recording.", systemImage: "trash.slash")
            } else {
                detail
            }
        }
        .navigationBarTitleDisplayMode(.inline)
        .task {
            await load()
            // An older note without section summaries gets them now (while this screen is open), for chapters.
            await pipeline.backfillSections(noteID: noteID)
        }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await load() } }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await load() } }
        .onChange(of: presenting) { _, open in PresentationGate.shared.set(gateKey, open) }
        // Playback ending by itself resets the play button.
        .task(id: playing) {
            while playing && !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(400))
                if player?.isPlaying != true { playing = false; player = nil }
            }
        }
        .onDisappear {
            saveTitle()
            stopPlayback()
            PresentationGate.shared.set(gateKey, false)
        }
    }

    private var detail: some View {
        ScrollViewReader { proxy in
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    TextField("Title", text: $title, prompt: Theme.prompt("Title"), axis: .vertical)
                        .font(.title2.weight(.semibold))
                        .onSubmit { saveTitle() }

                    if let n = note {
                        HStack(spacing: 8) {
                            Text(n.createdAt, format: .dateTime.day().month().year().hour().minute())
                            if n.duration_seconds > 0 { Text("· \(Duration.seconds(n.duration_seconds).formatted(.time(pattern: .minuteSecond)))") }
                            Text("· \(n.lang.uppercased())")
                            if let p = audioPath {
                                Button { togglePlay(p) } label: {
                                    Image(systemName: playing ? "stop.circle" : "play.circle").font(.title2)
                                        .frame(minWidth: 44, minHeight: 44)
                                }
                                .accessibilityLabel(playing ? Text("Stop") : Text("Play the recording"))
                            }
                        }
                        .font(.caption).foregroundStyle(Theme.secondary)
                    }

                    if let speed, speed.finishedAt != nil {
                        DisclosureGroup("How it ran on this phone") {
                            VStack(alignment: .leading, spacing: 4) {
                                ForEach(SpeedMath.noteLines(speed), id: \.self) { Text(verbatim: $0) }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.top, 4)
                        }
                        .font(.footnote).foregroundStyle(Theme.secondary)
                    }

                    ChaptersView(chapters: chapters, onPlay: audioPath.map { p in { sec in playChapter(p, sec) } })

                    NoteMemoriesSection(noteID: noteID) { id, sec in playMoment(id, sec) }

                    if isThisNoteBusy {
                        if !isRegeneratingHere {
                            StatusBanner(stage: pipeline.stage, line: pipeline.progressLine)
                                .clipShape(RoundedRectangle(cornerRadius: 8))
                            if pipeline.overnightOffer == noteID { overnightOffer }
                            if needsGeneration { finishNowButton }
                        }
                    } else if pipeline.isOvernight(noteID) {
                        StatusBanner(stage: .overnight, onDoItNow: models.isReady ? {
                            Task { await pipeline.doItNow(noteID: noteID) }
                        } : nil)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    } else if waitingForAI {
                        StatusBanner(stage: .paused(models.status == .unsupported ? AIStatus.unsupportedMessage
                                                                                  : ProcessingPipeline.waitingMessage))
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                    } else if pipeline.hasQueuedTap(noteID) {
                        // A tap made while another note was being written (or the AI was not ready, or the phone was
                        // too warm): it runs by itself in its turn.
                        Text(pipeline.tapWaitingLine(noteID)).font(.subheadline).foregroundStyle(Theme.secondary)
                    } else if let failure = pipeline.lastFailure[noteID],
                              let shows = PauseWords.banner(line: failure, failedLine: pipeline.failedLines[noteID]) {
                        // Paused or cut off: it continues by itself, and "Continue now" runs it at once. "Couldn't
                        // finish" and "Try again" only for a real failure (PauseWords: never for a cut-off).
                        let paused = shows == .paused
                        StatusBanner(stage: paused ? .paused(failure) : .failed(failure),
                                     onTryAgain: RegenerateRules.showsTryAgain(needsWork: needsGeneration, aiReady: models.isReady,
                                                                               overnight: pipeline.isOvernight(noteID)) ? {
                                         Task { await pipeline.tryAgain(noteID: noteID) }
                                     } : nil,
                                     continueNow: paused,
                                     tryAgainDisabled: pipeline.isBusy)
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                    }

                    Picker("Section", selection: $tab) {
                        ForEach(Tab.allCases) { Text($0.title).accessibilityLabel(Text($0.accessibilityTitle)).tag($0) }
                    }
                    .pickerStyle(.segmented)

                    checkBanner

                    content

                    missingParts

                    regenerateControls

                    // Hidden while the failure banner offers "Try again" (same action).
                    if needsGeneration && !isThisNoteBusy && models.isReady && !pipeline.isOvernight(noteID)
                        && pipeline.lastFailure[noteID] == nil {
                        Button {
                            Task { await pipeline.regenerate(noteID: noteID) }
                        } label: { Label("Write my notes", systemImage: "sparkles").frame(maxWidth: .infinity) }
                        .buttonStyle(.pillProminent)
                        .disabled(pipeline.isBusy || !(hasTranscript || hasAudio))
                    }

                    if showFindReminders {
                        Button {
                            Task {
                                await pipeline.findReminders(noteID: noteID)
                                await load()
                            }
                        } label: { Label("Find reminders and tasks", systemImage: "checklist").frame(maxWidth: .infinity) }
                        .buttonStyle(.pill)
                        .frame(minHeight: 44)
                        .disabled(pipeline.isBusy)
                    }

                    askSection
                }
                .padding()
            }
            .onChange(of: scrollRequest) { _, i in
                guard let i else { return }
                Task {
                    try? await Task.sleep(for: .milliseconds(300))   // let the transcript lay out first
                    withAnimation { proxy.scrollTo(pieceID(i), anchor: .center) }
                    scrollRequest = nil
                }
            }
        }
        .themedPage()
        .overlay(alignment: .bottom) {
            if let toast {
                Text(verbatim: toast)
                    .font(.callout.weight(.medium))
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 16).padding(.vertical, 12)
                    .background(Theme.chip, in: Capsule())
                    .padding(.bottom, 24).padding(.horizontal)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                if let text = shareText {
                    Button { shareItem = ShareItem(items: [text]) } label: { Image(systemName: "square.and.arrow.up") }
                        .accessibilityLabel(Text("Share"))
                }
                Menu {
                    if shareText != nil {
                        Button {
                            if let url = exportFileURL() { shareItem = ShareItem(items: [url]) }
                        } label: { Label("Export as Markdown file", systemImage: "doc") }
                    }
                    if showsRegenerateAll {
                        Button { regenerate(nil) } label: { Label("Write it all again", systemImage: "arrow.clockwise") }
                            .disabled(pipeline.isBusy)
                    }
                    Button(role: .destructive) { confirmDelete = true } label: { Label("Delete note", systemImage: "trash") }
                } label: { Image(systemName: "ellipsis.circle") }
                .accessibilityLabel(Text("More options"))
            }
        }
        .confirmationDialog("Delete this note and its recording?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { deleteNote() }
        }
        .sheet(item: $shareItem) { ActivityView(items: $0.items) }
        .navigationDestination(item: $openMoment) { NoteDetailView(noteID: $0.noteID, startAt: $0.startSec) }
        .sheet(item: $fixing) { target in
            FixWordSheet(pieceText: target.text) { word in Task { await saveWord(word) } }
        }
        .alert("I couldn't save this. Please try again.", isPresented: errorShown) {
            Button("OK", role: .cancel) {}
        }
    }

    @ViewBuilder private var content: some View {
        if tab == .map {
            Picker("Show", selection: $mapMode) {
                Text("Flow").tag(MapMode.flow)
                Text("Graph").tag(MapMode.graph)
            }
            .pickerStyle(.segmented)
            if mapMode == .flow {
                NoteFlowView(noteID: noteID, chapters: chapters) { sec in playMoment(noteID, sec) }
            } else {
                // This recording's map: its centre is the title. Sideways drags turn it; up and down scroll the page.
                BrainMapScreen(noteID: noteID, centerTitle: title, inScroll: true) { id, sec in playMoment(id, sec) }
                    .frame(height: 540)
                    .background(Theme.background, in: RoundedRectangle(cornerRadius: 16))
                    .clipShape(RoundedRectangle(cornerRadius: 16))
            }
        } else if tab == .transcript && !pieces.isEmpty {
            piecesList
        } else {
            let text: String? = {
                guard let n = note else { return nil }
                switch tab {
                case .summary: return n.short_summary
                case .notes: return n.detailed_notes
                case .questions: return n.revision_questions
                case .transcript: return n.transcript
                case .map: return nil
                }
            }()
            if let text, !text.isEmpty, tab == .transcript {
                // What was heard, exactly: never read as markdown.
                Text(verbatim: text)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else if let text, !text.isEmpty {
                Text(markdown(text))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text(isThisNoteBusy ? "Writing this now…" : (tab == .transcript ? "Nothing written down yet." : "I haven't written this yet."))
                    .foregroundStyle(Theme.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 24)
            }
        }
    }

    /// Under the notes or the questions: a line for each part that could not be written. Try again writes only
    /// those parts and puts the text together again.
    @ViewBuilder private var missingParts: some View {
        if !isThisNoteBusy, pipeline.regenerateFailed[noteID] == nil, let kind = tab.kind.flatMap(NoteSteps.Kind.init),
           let parts = partsLeft[kind], !parts.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(parts, id: \.self) { part in
                    HStack(spacing: 10) {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.cautionIcon)
                        Button(ProcessingPipeline.partLeftLine(part)) { Task { await pipeline.tryAgain(noteID: noteID) } }
                            .font(.footnote.weight(.semibold))
                            .frame(minHeight: 44, alignment: .leading)
                            .disabled(pipeline.isBusy || !models.isReady)
                        Spacer()
                    }
                }
            }
            .padding(.horizontal).padding(.vertical, 8)
            .background(Theme.card, in: RoundedRectangle(cornerRadius: 8))
        }
    }

    /// Under a tab's text: the progress and Stop while a regenerate runs, "Try again" after one failed, else
    /// Regenerate (and Undo after a regenerate).
    @ViewBuilder private var regenerateControls: some View {
        if isRegeneratingHere {
            StatusBanner(stage: pipeline.stage, onStop: { pipeline.stopRegenerating(noteID: noteID) })
                .clipShape(RoundedRectangle(cornerRadius: 8))
        } else if !isThisNoteBusy, let failed = pipeline.regenerateFailed[noteID] {
            HStack(spacing: 10) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.cautionIcon)
                Text("I couldn't write this again. Please try once more.").font(.footnote)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer()
                Button("Try again") { regenerate(failed.kind) }
                    .buttonStyle(.pill)
                    .font(.footnote.weight(.semibold))
                    .frame(minHeight: 44)
                    .disabled(pipeline.isBusy || !models.isReady)
            }
            .padding(.horizontal).padding(.vertical, 8)
            .background(Theme.card, in: RoundedRectangle(cornerRadius: 8))
        } else if let kind = tab.kind {
            let hasText = !(note?.output(kind) ?? "").isEmpty
            let showRegenerate = RegenerateRules.showsRegenerate(hasText: hasText, noteBusy: isThisNoteBusy, aiReady: models.isReady,
                                                                  overnight: pipeline.isOvernight(noteID))
            let showUndo = RegenerateRules.showsUndo(replacedThisTab: undo?.outputs[kind] != nil, noteBusy: isThisNoteBusy)
            if showRegenerate || showUndo {
                HStack(spacing: 12) {
                    if showRegenerate {
                        Button { regenerate(kind) } label: {
                            Label("Write again", systemImage: "arrow.clockwise").frame(minHeight: 44)
                        }
                        .buttonStyle(.pill)
                        .disabled(pipeline.isBusy)
                    }
                    if showUndo {
                        Button { undoRegenerate() } label: {
                            Label("Undo", systemImage: "arrow.uturn.backward").frame(minHeight: 44)
                        }
                        .buttonStyle(.pill)
                    }
                }
            }
        }
    }

    private var showsRegenerateAll: Bool {
        guard let n = note else { return false }
        return RegenerateRules.showsRegenerateAll(hasTranscript: hasTranscript, outputs: OutputKind.allCases.map { n.output($0) != nil },
                                                  noteBusy: isThisNoteBusy, aiReady: models.isReady, overnight: pipeline.isOvernight(noteID))
    }

    /// One output (kind) or, nil, the title and all three. The old text stays until the new is complete.
    private func regenerate(_ kind: OutputKind?) {
        Task {
            if let previous = await pipeline.regenerateOutput(noteID: noteID, kind: kind) { undo = previous }
            await load()
        }
    }

    /// The text before the last regenerate back, saved.
    private func undoRegenerate() {
        guard let previous = undo else { return }
        undo = nil
        Task {
            await pipeline.restore(previous, noteID: noteID)
            await load()
        }
    }

    /// The notes are still being written: keep the app open with a dim screen and they finish in minutes (FinishNowView).
    private var finishNowButton: some View {
        Button { FinishNowController.shared.start(noteID: noteID) } label: {
            Label(String(localized: "Finish now · about \(FinishNow.estimateMinutes(workLeft, onGPU: true)) min"),
                  systemImage: "bolt")
                .frame(maxWidth: .infinity, minHeight: 44)
        }
        .buttonStyle(.pill)
    }

    /// After Stop, when the work left is long: finish overnight while charging, or keep going now.
    private var overnightOffer: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("This one will take a while. Shall I finish it overnight while the phone charges?", systemImage: "moon.zzz")
                .font(.subheadline)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Button { pipeline.finishOvernight(noteID: noteID) } label: {
                    Text("Yes").frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pillProminent)
                Button { pipeline.keepGoing(noteID: noteID) } label: {
                    Text("Keep going").frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pill)
            }
        }
        .padding()
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 8))
    }

    // MARK: transcript pieces

    private func pieceID(_ i: Int) -> String { "piece-\(i)" }

    private var piecesList: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Array(pieces.enumerated()), id: \.offset) { i, p in
                pieceRow(i, p).id(pieceID(i))
            }
            Text("Touch and hold a part to fix a word.")
                .font(.footnote).foregroundStyle(Theme.secondary).padding(.top, 8)
        }
    }

    private func pieceRow(_ i: Int, _ p: TranscriptPiece) -> some View {
        let stamp = AssistantFormat.mmss(p.startSec)
        return HStack(alignment: .firstTextBaseline, spacing: 10) {
            if let path = audioPath {
                Button { playFrom(path, p.startSec, piece: i) } label: {
                    Text(verbatim: stamp).font(.callout.monospacedDigit().weight(.semibold))
                        .frame(minWidth: 52, minHeight: 44, alignment: .leading)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.accent)
                .accessibilityLabel(Text("Play from \(stamp)"))
            } else {
                Text(verbatim: stamp).font(.callout.monospacedDigit()).foregroundStyle(Theme.secondary)
                    .frame(minWidth: 52, alignment: .leading)
            }
            Text(verbatim: p.text)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 8).padding(.vertical, 2)
        .background(highlighted == i ? Theme.highlight : Color.clear, in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(highlighted == i ? Theme.secondary : Color.clear, lineWidth: 1))
        .contentShape(RoundedRectangle(cornerRadius: 8))
        .contextMenu {
            Button { fixing = FixTarget(text: p.text) } label: { Label("Fix a word", systemImage: "character.cursor.ibeam") }
        }
        .accessibilityAction(named: Text("Fix a word")) { fixing = FixTarget(text: p.text) }
    }

    private func saveWord(_ word: String) async {
        let w = word.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !w.isEmpty else { return }
        fixing = nil
        let id = await CoreEngine.shared.addVocab(w)
        guard id > 0 else { errorMessage = String(localized: "I couldn't save this. Please try again."); return }
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        showToast(String(localized: "Saved “\(w)”. I'll remember this word."))
    }

    private func showToast(_ message: String) {
        withAnimation { toast = message }
        UIAccessibility.post(notification: .announcement, argument: message)
        Task {
            try? await Task.sleep(for: .seconds(3))
            if toast == message { withAnimation { toast = nil } }
        }
    }

    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }

    // MARK: ask about this recording

    private var askSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Divider().padding(.vertical, 8)
            Text("Ask about this recording").font(.headline)
            HStack {
                TextField("e.g. What was the homework?", text: $question, prompt: Theme.prompt("e.g. What was the homework?"))
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { ask() }
                Button { ask() } label: { Image(systemName: "paperplane.fill").frame(minWidth: 44, minHeight: 44) }
                    .disabled(question.isEmpty || asking || pipeline.isBusy)
                    .accessibilityLabel(Text("Send"))
            }
            if asking { ProgressView().controlSize(.small) }
            if let a = answer { Text(verbatim: a).textSelection(.enabled).padding(.top, 4) }
        }
    }

    private var audioPath: String? {
        AudioRecorder.existingAudioPath(note?.audio_path)
    }
    private var hasTranscript: Bool { !(note?.transcript ?? "").isEmpty }
    private var hasAudio: Bool { audioPath != nil }

    /// Older recordings never went through extraction: offer it once, when the AI is ready.
    private var showFindReminders: Bool {
        models.isReady && !extracted && hasTranscript && !isThisNoteBusy
    }

    /// Recorded before the AI was ready: the audio is kept and processed once it has downloaded.
    private var waitingForAI: Bool { !models.isReady && !hasTranscript && hasAudio }

    private var needsGeneration: Bool {
        guard let n = note else { return false }
        return n.short_summary == nil || n.detailed_notes == nil || n.revision_questions == nil
    }

    private var shareText: String? {
        guard let n = note else { return nil }
        var s = "# \(n.title)\n\n"
        if let x = n.short_summary { s += "## \(String(localized: "Summary"))\n\(x)\n\n" }
        if let x = n.detailed_notes { s += x + "\n\n" }
        if let x = n.revision_questions { s += "## \(String(localized: "Revision questions"))\n\(x)\n\n" }
        s += String(localized: "— Made with Summaverick Everyday, on this device.")
        return s
    }

    private func exportFileURL() -> URL? {
        guard let text = shareText else { return nil }
        let safe = title.replacingOccurrences(of: "/", with: "-").prefix(60)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(safe).md")
        do { try text.write(to: url, atomically: true, encoding: .utf8) } catch { return nil }
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

    /// "Please check" on an output the checking agent could not get through its checks (NoteCheck).
    @ViewBuilder private var checkBanner: some View {
        let part: String? = {
            switch tab {
            case .summary: return "summary"
            case .notes: return "notes"
            case .questions: return "questions"
            default: return nil
            }
        }()
        if let part, let p = NoteCheck.flagged(check, part: part) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.title2).foregroundStyle(Theme.caution)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Please check").font(.headline)
                    ForEach(NoteCheck.reasons(p), id: \.self) { Text($0).font(.body) }
                    // Each flagged line with where the recording says it: tap to hear that passage.
                    ForEach((p.evidence ?? []).filter { $0.startSec != nil }, id: \.self) { e in
                        Button { playMoment(noteID, e.startSec ?? 0) } label: {
                            Label {
                                Text(verbatim: "\(AssistantFormat.mmss(e.startSec ?? 0))  \(e.item)").lineLimit(2)
                            } icon: { Image(systemName: "play.circle") }
                            .font(.footnote)
                        }
                        .buttonStyle(.plain)
                        .frame(minHeight: 44, alignment: .leading)
                        .accessibilityLabel(Text("Listen at \(AssistantFormat.mmss(e.startSec ?? 0)): \(e.item)"))
                    }
                    Text("I checked this against the recording and tried again, but could not be sure.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                Spacer(minLength: 0)
            }
            .foregroundStyle(Theme.text)
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.cautionFill, in: RoundedRectangle(cornerRadius: 12))
            .accessibilityElement(children: (p.evidence ?? []).contains { $0.startSec != nil } ? .contain : .combine)
        } else if part != nil, let status = NoteCheck.status(pending: checksPending, report: check) {
            // The run's verdict, under every written output: honest about checks not run yet.
            Label(status.text, systemImage: status.symbol)
                .font(.footnote)
                .foregroundStyle(Theme.secondary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    // MARK: data

    private func load() async {
        let previous = note
        let n = await CoreEngine.shared.note(noteID)
        let ps = await CoreEngine.shared.pieces(noteID)
        if let json = await CoreEngine.shared.setting(SpeedRecord.key(noteID)), let data = json.data(using: .utf8) {
            speed = try? JSONDecoder().decode(SpeedRecord.self, from: data)
        }
        check = await CoreEngine.shared.setting(NoteCheck.key(noteID)).flatMap(NoteCheck.report)
        checksPending = pipeline.checksPending(noteID)
        chapters = await CoreEngine.shared.chapters(noteID)
        extracted = await CoreEngine.shared.noteExtracted(noteID)
        let lang = n?.lang ?? "en"
        partsLeft = [.notes: await ProcessingPipeline.partsLeft(noteID: noteID, lang: lang, kind: .notes),
                     .questions: await ProcessingPipeline.partsLeft(noteID: noteID, lang: lang, kind: .questions)]
        if let n, n.detailed_notes == nil || n.revision_questions == nil {
            workLeft = await FinishNowController.work(for: n)
        }
        note = n
        pieces = ps
        loaded = true
        guard let n else { return }
        // Adopt the stored title unless the user has typed their own.
        let userEdited = previous != nil && title != previous!.title
        if !userEdited { title = n.title }
        // Opened at a moment: show and highlight the piece it falls in (once).
        if let startAt, !didScrollToStart, !ps.isEmpty {
            didScrollToStart = true
            let i = ps.lastIndex(where: { $0.startSec <= startAt }) ?? 0
            highlighted = i
            scrollRequest = i
        }
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
            // Live transcription needs the model, piece after piece: a question would hold it.
            let recording = String(localized: "Recording now. Ask again after you stop.")
            guard !LiveTranscriber.shared.liveRunning else { answer = recording; return }
            guard models.isReady else { answer = String(localized: "I'm still getting ready"); return }
            guard !Heat.isTooHot(ProcessInfo.processInfo.thermalState) else {
                answer = String(localized: "The phone is too warm. Try again when it has cooled down."); return
            }
            // The person first: memory work stops for the answer. Other work still running: the busy line.
            let busy = String(localized: "I'm working on another note. Try again in a moment.")
            guard await pipeline.yieldMemories(), !pipeline.isWorking else { answer = busy; return }
            do {
                try await models.loadIntoCore()   // the backend for right now (GPU in front)
                guard !pipeline.isWorking else { answer = busy; return }
                guard !LiveTranscriber.shared.liveRunning else { answer = recording; return }
                let context = [n.detailed_notes, t].compactMap { $0 }.joined(separator: "\n\n")
                answer = try await CoreEngine.shared.ask(context: context, question: q, lang: n.lang)
            } catch {
                answer = error.localizedDescription
            }
        }
    }

    // MARK: playback

    private func togglePlay(_ path: String) {
        if let p = player, p.isPlaying { stopPlayback(); return }
        let from = seekPending ? startAt : nil
        seekPending = false
        play(path, from: from)
    }

    /// A tapped m:ss stamp: play from there and highlight that piece.
    private func playFrom(_ path: String, _ sec: Int, piece: Int) {
        seekPending = false
        highlighted = piece
        play(path, from: sec)
    }

    /// A chapter tapped: play from its start and highlight the piece it starts in.
    private func playChapter(_ path: String, _ sec: Int) {
        seekPending = false
        highlighted = pieces.lastIndex(where: { $0.startSec <= sec })
        play(path, from: sec)
    }

    /// A memory's ▶: this recording plays from that second (the transcript shows the piece); another one opens.
    private func playMoment(_ id: Int64, _ sec: Int) {
        guard id == noteID else { stopPlayback(); openMoment = NoteMoment(noteID: id, startSec: sec); return }
        if let p = audioPath { playChapter(p, sec) } else { highlighted = pieces.lastIndex(where: { $0.startSec <= sec }) }
        if !pieces.isEmpty {
            tab = .transcript
            scrollRequest = highlighted ?? pieces.lastIndex(where: { $0.startSec <= sec })
        }
    }

    private func play(_ path: String, from sec: Int?) {
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        if player == nil { player = try? AVAudioPlayer(contentsOf: URL(fileURLWithPath: path)) }
        guard let p = player else { return }
        if let sec { p.currentTime = min(TimeInterval(max(0, sec)), max(0, p.duration - 0.5)) }
        playing = p.play()
    }

    private func stopPlayback() {
        player?.stop()
        player = nil
        playing = false
    }

    private func deleteNote() {
        Task {
            stopPlayback()
            pipeline.forget(noteID: noteID)
            if let p = AudioRecorder.existingAudioPath(note?.audio_path) { AudioRecorder.removeAudio(atPath: p) }
            await CoreEngine.shared.deleteNote(noteID)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)   // resync notifications now
            dismiss()
        }
    }
}

/// The piece whose word is being fixed.
private struct FixTarget: Identifiable {
    let id = UUID()
    let text: String
}

/// What the share sheet shares.
private struct ShareItem: Identifiable {
    let id = UUID()
    let items: [Any]
}

/// The system share sheet, presented by this screen (so it can be registered with PresentationGate).
private struct ActivityView: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }
    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}

/// "Fix a word": pick the word that was heard wrong (or type it), write it correctly, save.
/// The word is the person's own edit, so it is added to Summaverick's vocabulary directly.
private struct FixWordSheet: View {
    let pieceText: String
    let onSave: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var word = ""

    private var words: [String] {
        var seen = Set<String>()
        return pieceText.split(whereSeparator: { $0.isWhitespace })
            .map { $0.trimmingCharacters(in: .punctuationCharacters.union(.symbols)) }
            .filter { !$0.isEmpty && seen.insert($0.lowercased()).inserted }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text(verbatim: pieceText).font(.body)
                } header: { Text("What was written") }
                .listRowBackground(Theme.formRow)

                if !words.isEmpty {
                    Section {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 8)], alignment: .leading, spacing: 8) {
                            ForEach(words, id: \.self) { w in
                                Button { word = w } label: {
                                    Text(verbatim: w).font(.body).lineLimit(1).minimumScaleFactor(0.7)
                                        .frame(maxWidth: .infinity, minHeight: 44)
                                }
                                .buttonStyle(.pill)
                            }
                        }
                        .padding(.vertical, 4)
                    } header: { Text("Tap the word that is wrong") }
                    .listRowBackground(Theme.formRow)
                }

                Section {
                    TextField("Correct word", text: $word, prompt: Theme.prompt("Correct word"))
                        .font(.title3)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .frame(minHeight: 44)
                } header: { Text("Write it correctly") } footer: {
                    Text("I'll remember this word for your next recordings.")
                }
                .listRowBackground(Theme.formRow)
            }
            .themedForm()
            .navigationTitle("Fix a word")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { onSave(word) }
                        .font(.headline)
                        .disabled(word.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
            }
        }
    }
}


/// A recording's flow: its discussion in order (the chapters) and, under each part, what came out of it by when it
/// was said: decisions, actions with who and when (done or open, or still waiting for a Yes), promises and open
/// questions; then the next meeting, if one was set. Key points are counted, opened on a tap.
struct NoteFlowView: View {
    let noteID: Int64
    let chapters: [Chapter]
    let onPlay: (Int) -> Void

    struct Outcome: Identifiable {
        enum Kind { case decision, action, proposed, promise, question, point }
        let id: String
        let kind: Kind
        let text: String
        let detail: String?
        let done: Bool
        let second: Int?
    }

    @State private var outcomes: [Outcome] = []
    @State private var next: (text: String, date: Date)?
    @State private var openPoints: Set<Int> = []
    @State private var loaded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if loaded && outcomes.isEmpty && chapters.isEmpty {
                Text("Nothing came out of this recording yet: no decisions, actions or open questions.")
                    .foregroundStyle(Theme.secondary).padding(.vertical, 16)
            }
            let groups = grouped()
            ForEach(groups.indices, id: \.self) { i in
                stage(i, groups[i], last: i == groups.count - 1 && next == nil)
            }
            if let next {
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: "calendar.badge.clock").foregroundStyle(Theme.accent).frame(width: 24)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Next meeting").font(.subheadline.weight(.semibold))
                        Text(verbatim: next.text + " · " + next.date.formatted(.dateTime.weekday(.wide).day().month().hour().minute()))
                            .font(.footnote).foregroundStyle(Theme.secondary)
                    }
                }
                .padding(.top, 8)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .task { await load() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await load() } }
    }

    /// One part of the discussion and what came out of it. The last group (index chapters.count) holds what has no time.
    private func stage(_ i: Int, _ items: [Outcome], last: Bool) -> some View {
        let isChapter = i < chapters.count
        let points = items.filter { $0.kind == .point }
        let outs = items.filter { $0.kind != .point }
        return HStack(alignment: .top, spacing: 12) {
            VStack(spacing: 0) {
                Circle().fill(Theme.accent).frame(width: 10, height: 10).padding(.top, 6)
                if !last { Rectangle().fill(Theme.secondary.opacity(0.35)).frame(width: 2).frame(maxHeight: .infinity) }
            }
            .frame(width: 24)
            VStack(alignment: .leading, spacing: 8) {
                if isChapter {
                    Button { onPlay(chapters[i].start_sec) } label: {
                        HStack(spacing: 6) {
                            Text(verbatim: chapters[i].title).font(.headline).multilineTextAlignment(.leading)
                            Text(verbatim: Duration.seconds(chapters[i].start_sec).formatted(.time(pattern: .minuteSecond)))
                                .font(.caption.monospacedDigit()).foregroundStyle(Theme.secondary)
                        }
                        .frame(minHeight: 32, alignment: .leading)
                    }
                    .buttonStyle(.plain)
                } else if !items.isEmpty {
                    Text("Also from this recording").font(.headline)
                }
                ForEach(outs) { row($0) }
                if !points.isEmpty {
                    Button { withAnimation { if openPoints.contains(i) { openPoints.remove(i) } else { openPoints.insert(i) } } } label: {
                        Label(openPoints.contains(i) ? "Hide key points" : "\(points.count) key points",
                              systemImage: openPoints.contains(i) ? "chevron.up" : "chevron.down")
                            .font(.footnote.weight(.medium)).frame(minHeight: 32)
                    }
                    .buttonStyle(.plain).foregroundStyle(Theme.accent)
                    if openPoints.contains(i) { ForEach(points) { row($0) } }
                }
            }
            .padding(.bottom, 14)
            Spacer(minLength: 0)
        }
    }

    private func row(_ o: Outcome) -> some View {
        let icon: String, label: LocalizedStringKey
        switch o.kind {
        case .decision: icon = "checkmark.seal.fill"; label = "Decided"
        case .action: icon = o.done ? "checkmark.circle.fill" : "circle"; label = o.done ? "Done" : "Action"
        case .proposed: icon = "circle.dashed"; label = "Action, waiting for your Yes"
        case .promise: icon = "hand.raised.fill"; label = "Promised"
        case .question: icon = "questionmark.circle.fill"; label = o.done ? "Answered" : "Still open"
        case .point: icon = "lightbulb"; label = "Key point"
        }
        return Button { if let s = o.second { onPlay(s) } } label: {
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: icon).foregroundStyle(o.kind == .question && !o.done ? Theme.cautionIcon : Theme.accent)
                    .frame(width: 20).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(label).font(.caption.weight(.semibold)).foregroundStyle(Theme.secondary)
                    Text(verbatim: o.text).font(.body).strikethrough(o.kind == .action && o.done)
                        .multilineTextAlignment(.leading).fixedSize(horizontal: false, vertical: true)
                    if let d = o.detail { Text(verbatim: d).font(.footnote).foregroundStyle(Theme.secondary) }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(o.second == nil)
    }

    /// The outcomes by part of the discussion, in the order said; the last group holds those with no time.
    private func grouped() -> [[Outcome]] {
        var g = Array(repeating: [Outcome](), count: chapters.count + 1)
        let starts = chapters.map(\.start_sec)
        for o in outcomes.sorted(by: { ($0.second ?? .max) < ($1.second ?? .max) }) {
            g[FlowGroups.chapterIndex(chapterStarts: starts, second: o.second) ?? chapters.count].append(o)
        }
        if g.last?.isEmpty == true { g.removeLast() }
        return g
    }

    private func who(_ w: String?, due: String?) -> String? {
        var parts: [String] = []
        if let w = MeetingBrief.owner(w, selfName: nil) { parts.append(w) }
        if let d = due, let day = ReminderScheduler.day(d, .current) {
            parts.append(String(localized: "by \(day.formatted(.dateTime.weekday(.wide).day().month()))"))
        }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    private func load() async {
        let e = CoreEngine.shared
        var out: [Outcome] = []
        for t in await e.tasks(openOnly: false) where t.noteID == noteID {
            out.append(Outcome(id: "t\(t.id)", kind: .action, text: t.text, detail: who(t.who, due: t.dueDate), done: t.done, second: t.startSec))
        }
        for s in await e.suggestions(status: "pending") where s.noteID == noteID && s.kind == .task {
            out.append(Outcome(id: "s\(s.id)", kind: .proposed, text: s.payload.text ?? "", detail: who(s.payload.who, due: s.payload.dueDate),
                               done: false, second: s.startSec))
        }
        for m in await e.memories(MemoryFilter(noteID: noteID, types: ["decision", "promise", "question", "fact", "idea"])) {
            let kind: Outcome.Kind
            switch m.type {
            case "decision": kind = .decision
            case "promise": kind = .promise
            case "question": kind = .question
            default: kind = .point
            }
            let sec = m.sources.first(where: { $0.noteID == noteID })?.startSec
            let detail = m.type == "promise" || m.type == "decision" ? who(m.who, due: m.dueDate) : nil
            out.append(Outcome(id: "m\(m.id)", kind: kind, text: m.text, detail: detail, done: m.done, second: sec))
        }
        outcomes = out
        let now = Date()
        let rs = await e.reminders(activeOnly: true).filter { $0.noteID == noteID && $0.medicineID == nil }
        next = ReminderOccurrence.upcoming(rs, now: now, calendar: .current, days: 14).first { $0.date > now }
            .map { (text: $0.reminder.text, date: $0.date) }
        loaded = true
    }
}
