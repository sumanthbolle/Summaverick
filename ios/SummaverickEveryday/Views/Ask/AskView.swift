// Ask Summaverick (spec §6.3): a chat over all recordings, typed or spoken. Answers come from the core
// (sve_assistant_chat) and are shown as given: medicine answers are quoted word for word, never reworded,
// and nothing is added to them. Every answer links to the moments it came from. The answer appears as it is
// written (Stop keeps what was written); follow-ups go with the earlier turns (ChatWindow); chats are kept on
// the phone (ChatStore), with "New chat" and the earlier chats in the toolbar.
import AVFoundation
import SwiftUI
import UserNotifications

/// What the "Make reminder" / "Make to-do" form is opened for.
private struct AnswerFormRequest: Identifiable {
    let id = UUID()
    let kind: SuggestionKind      // .reminder or .task
    let text: String
    let medicine: Bool
    let source: Source?
    /// The one saved medicine a medicine answer names, if exactly one.
    let medicineID: Int64?

    /// A card that exists only on this screen: the answer text, no times or dates (never invented).
    /// A medicine reminder also has no repeat until the person picks one.
    var suggestion: Suggestion {
        var p = SuggestionPayload()
        p.text = text
        if kind == .reminder {
            p.times = []
            p.repeat = medicine ? "" : "once"
        }
        return Suggestion(local: kind, payload: p, noteID: source?.noteID, startSec: source?.startSec)
    }
}

/// A short message above the question box.
private enum AskNotice: Equatable {
    case busy, needsModel, tooWarm, micReady, holdLonger, notHeard, transcribeFailed, micFailed

    var text: LocalizedStringKey {
        switch self {
        case .busy: return "Busy with a recording — try again in a moment."
        case .needsModel: return "Summaverick is still getting ready"
        case .tooWarm: return "The phone is too warm. Try again when it has cooled down."
        case .micReady: return "The microphone is ready. Hold the mic button and speak."
        case .holdLonger: return "Hold the mic button while you speak."
        case .notHeard: return "I couldn't hear a question. Please try again."
        case .transcribeFailed: return "Could not write down your question. Please try again, or type it."
        case .micFailed: return "Could not start the microphone."
        }
    }
}

struct AskView: View {
    @Environment(Router.self) private var router
    @Environment(AppSettings.self) private var settings
    @State private var pipeline = ProcessingPipeline.shared
    @State private var models = ModelManager.shared

    @State private var chats = ChatStore.shared
    /// Saved medicine ids the core finds in a medicine answer, by turn ([] otherwise); missing = could not tell yet.
    @State private var named: [UUID: [Int64]] = [:]
    @State private var askTask: Task<Void, Never>?
    /// Quiz turns whose expected answer the person has asked to see.
    @State private var revealed: Set<UUID> = []
    @State private var text = ""
    @State private var generating = false
    @State private var notice: AskNotice?
    @State private var profileLang = "en"

    @State private var recorder = AudioRecorder()
    @State private var listening = false
    @State private var transcribing = false
    @State private var micDenied = false

    @State private var formRequest: AnswerFormRequest?
    /// Set by the form's Save; saved once the sheet has gone, so a following alert can show.
    @State private var pendingSave: (request: AnswerFormRequest, draft: SuggestionDraft)?
    @State private var errorMessage: String?
    @State private var notificationsOff = false
    @State private var toast: String?
    @FocusState private var fieldFocused: Bool

    /// The longest voice question.
    private static let maxVoiceSeconds: TimeInterval = 30

    private var isTelugu: Bool { profileLang == "te" }
    private var turns: [ChatTurn] { chats.current?.turns ?? [] }
    /// Grows while an answer is written: the chat follows it down.
    private var streamedCount: Int {
        if case .streaming(let t) = turns.last?.state { return t.count }
        return 0
    }
    private var presenting: Bool { formRequest != nil || errorMessage != nil || notificationsOff || micDenied }

    var body: some View {
        NavigationStack {
            Group {
                if models.isReady {
                    chat
                } else {
                    ScrollView { AIGettingReadyPanel().padding(16) }
                }
            }
            .navigationTitle("Ask Summaverick")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu {
                        ForEach(chats.chats) { c in
                            Button { chats.select(c.id) } label: {
                                if c.id == chats.currentID {
                                    Label { Text(verbatim: c.title) } icon: { Image(systemName: "checkmark") }
                                } else {
                                    Text(verbatim: c.title)
                                }
                            }
                        }
                    } label: {
                        Image(systemName: "clock.arrow.circlepath").frame(minWidth: 44, minHeight: 44)
                    }
                    .accessibilityLabel(Text("Earlier chats"))
                    .disabled(generating || chats.chats.isEmpty)
                }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button { chats.newChat() } label: {
                        Label("New chat", systemImage: "square.and.pencil").frame(minWidth: 44, minHeight: 44)
                    }
                    .disabled(generating || turns.isEmpty)
                    Button { router.showSettings = true } label: {
                        Image(systemName: "gearshape").frame(minWidth: 44, minHeight: 44)
                    }
                    .accessibilityLabel(Text("Settings"))
                }
            }
        }
        .task { await loadProfile() }
        .task(id: chats.currentID) { await loadNamed() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await loadProfile() } }
        // "Delete everything": the chats (medicine answers too) go with the data. Settings deletes them;
        // an answer still being written stops.
        .onReceive(NotificationCenter.default.publisher(for: .assistantWiped)) { _ in
            askTask?.cancel()
            chats.deleteAll()
            named = [:]
        }
        .onAppear { takeDraft() }
        .onChange(of: router.askDraft) { _, _ in takeDraft() }
        .onChange(of: recorder.elapsed) { _, t in if listening && t >= Self.maxVoiceSeconds { endVoice() } }
        .onChange(of: presenting) { _, open in PresentationGate.shared.set("ask", open) }
        .onDisappear {
            if listening { cancelVoice() }
            PresentationGate.shared.set("ask", false)
        }
        .sheet(item: $formRequest, onDismiss: savePending) { r in
            SuggestionEditForm(suggestion: r.suggestion, noteTitle: r.source?.title,
                               answer: AnswerContext(text: r.text, medicine: r.medicine)) { _, _, draft in
                pendingSave = (r, draft)
                formRequest = nil
            }
            .environment(settings)
            .environment(router)
        }
        .alert("Could not save this. Please try again.", isPresented: errorShown) {
            Button("OK", role: .cancel) {}
        }
        .alert("Notifications are off", isPresented: $notificationsOff) {
            Button("Open Settings") { openSystemSettings() }
            Button("Not now", role: .cancel) {}
        } message: {
            Text("The reminder is saved, but it cannot ring until notifications are on for Summaverick.")
        }
        .alert("The microphone is off", isPresented: $micDenied) {
            Button("Open Settings") { openSystemSettings() }
            Button("Not now", role: .cancel) {}
        } message: {
            Text("To ask by voice, allow the microphone for Summaverick in Settings. You can still type your question.")
        }
    }

    // MARK: chat

    private var chat: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 18) {
                        if turns.isEmpty { intro }
                        ForEach(turns) { turn in
                            turnView(turn).id(turn.id)
                        }
                        Color.clear.frame(height: 1).id("bottom")
                    }
                    .padding(16)
                }
                .scrollDismissesKeyboard(.interactively)
                .onChange(of: turns.count) { _, _ in scrollDown(proxy) }
                .onChange(of: generating) { _, _ in scrollDown(proxy) }
                .onChange(of: chats.currentID) { _, _ in scrollDown(proxy) }
                .onChange(of: streamedCount) { _, _ in proxy.scrollTo("bottom", anchor: .bottom) }
            }
            .overlay(alignment: .bottom) {
                if let toast {
                    Text(verbatim: toast)
                        .font(.callout.weight(.medium))
                        .padding(.horizontal, 16).padding(.vertical, 12)
                        .background(.regularMaterial, in: Capsule())
                        .padding(.bottom, 12)
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            inputArea
        }
    }

    private func scrollDown(_ proxy: ScrollViewProxy) {
        Task {
            try? await Task.sleep(for: .milliseconds(100))
            withAnimation { proxy.scrollTo("bottom", anchor: .bottom) }
        }
    }

    private var intro: some View {
        VStack(alignment: .leading, spacing: 8) {
            Image(systemName: "bubble.left.and.bubble.right").font(.largeTitle).foregroundStyle(Color.accentColor)
                .accessibilityHidden(true)
            Text("Ask about anything you recorded.").font(.title3.bold())
            Text("Answers from your recordings link to the moment they came from. Anything else comes from general knowledge, and says so.")
                .font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, 8)
    }

    @ViewBuilder
    private func turnView(_ turn: ChatTurn) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Spacer(minLength: 48)
                Text(verbatim: turn.question)
                    .font(.body)
                    .foregroundStyle(.white)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .background(Color.accentColor, in: RoundedRectangle(cornerRadius: 18))
                    .textSelection(.enabled)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel(Text("You asked: \(turn.question)"))

            switch turn.state {
            case .waiting:
                TypingIndicator()
            case .streaming(let partial):
                if partial.isEmpty { TypingIndicator() } else { streamingView(partial) }
            case .answered(let a):
                if a.kind == "quiz" {
                    quizCard(a, turn: turn.id)
                } else {
                    answerView(a, turn: turn.id, named: named[turn.id])
                }
            case .stopped(let partial):
                stoppedView(partial, turn: turn)
            case .failed:
                VStack(alignment: .leading, spacing: 8) {
                    Label("Could not answer this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                        .font(.body).foregroundStyle(.primary)
                    Button("Try again") { retry(turn) }
                        .buttonStyle(.bordered)
                        .frame(minHeight: 44)
                        .disabled(generating)
                }
                .padding(14)
                .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
            }
        }
    }

    /// The not-found sentence in the answer language, word for word as the core writes it.
    private func notFoundText(_ lang: String?) -> String {
        (lang ?? profileLang) == "te" ? "మీ రికార్డింగ్‌లలో ఇది నేను వినలేదు." : "I didn't hear that in your recordings."
    }

    /// The answer as it is written. Never a medicine answer: those come whole, built in code.
    private func streamingView(_ partial: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(verbatim: partial)
                .font(.body)
                .fixedSize(horizontal: false, vertical: true)
            TypingIndicator(compact: true)   // more is coming: a sentence shows only once it is whole
        }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
            .accessibilityElement(children: .combine)
            .accessibilityLabel(Text("Summaverick is writing an answer"))
    }

    /// Stop was tapped: what was written before it, marked "(stopped)".
    private func stoppedView(_ partial: String, turn: ChatTurn) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if !partial.isEmpty {
                Text(verbatim: partial)
                    .font(.body)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text("(stopped)").font(.footnote).foregroundStyle(.secondary)
            HStack(spacing: 10) {
                if !partial.isEmpty { copyButton(partial) }
                Button("Try again") { retry(turn) }
                    .buttonStyle(.bordered)
                    .frame(minHeight: 44)
                    .disabled(generating)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func copyButton(_ words: String) -> some View {
        Button {
            UIPasteboard.general.string = words
            showToast(String(localized: "Copied."))
        } label: {
            Label("Copy", systemImage: "doc.on.doc").frame(minHeight: 44)
        }
        .buttonStyle(.bordered)
    }

    /// One revision question from a recording; the expected answer when the person asks, then the link and the next.
    @ViewBuilder
    private func quizCard(_ a: AskAnswer, turn: UUID) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Quiz", systemImage: "questionmark.circle")
                .font(.footnote.weight(.semibold)).foregroundStyle(.secondary)
            if let q = a.question {
                Text(verbatim: q)
                    .font(.body.weight(.medium))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                if revealed.contains(turn) {
                    Text(verbatim: a.answer)
                        .font(.body)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(a.sources) { src in sourceLink(src) }
                } else {
                    Button("Show answer") { revealed.insert(turn) }
                        .buttonStyle(.bordered)
                        .frame(minHeight: 44)
                }
            } else {
                Text(verbatim: a.answer)
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if turns.last?.id == turn {
                if a.quizDone {
                    Button("Start over") {
                        chats.clearQuiz(turn: turn)
                        send(String(localized: "Next question"), fromField: false)
                    }
                    .buttonStyle(.bordered)
                    .frame(minHeight: 44)
                    .disabled(generating || listening || transcribing)
                } else if a.question != nil {
                    Button("Next question") { send(String(localized: "Next question"), fromField: false) }
                        .buttonStyle(.bordered)
                        .frame(minHeight: 44)
                        .disabled(generating || listening || transcribing)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func answerView(_ a: AskAnswer, turn: UUID, named: [Int64]?) -> some View {
        let shown = a.answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? notFoundText(a.lang) : a.answer
        let quoted = a.medicine && shown == a.answer
        return VStack(alignment: .leading, spacing: 10) {
            if a.kind == "general" && !a.medicine {
                Label("General knowledge — not from your recordings", systemImage: "globe")
                    .font(.footnote.weight(.semibold)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if a.kind == "summary" && !a.notFound {
                Label("Summary of your recordings", systemImage: "text.alignleft")
                    .font(.footnote.weight(.semibold)).foregroundStyle(.secondary)
            }
            if a.kind == "due" && !a.medicine {
                Label("From your reminders and to-dos", systemImage: "checklist")
                    .font(.footnote.weight(.semibold)).foregroundStyle(.secondary)
            }
            if quoted {
                // Built in the core from saved facts and recorded words: shown exactly, in full.
                VStack(alignment: .leading, spacing: 6) {
                    Label(a.kind == "due" ? "From your reminders and to-dos" : AssistantFormat.medicineAnswerLabel(shown),
                          systemImage: "quote.opening")
                        .font(.footnote.weight(.semibold)).foregroundStyle(.secondary)
                    HStack(alignment: .top, spacing: 10) {
                        RoundedRectangle(cornerRadius: 2).fill(Color.accentColor.opacity(0.6)).frame(width: 4)
                            .accessibilityHidden(true)
                        Text(verbatim: a.answer)
                            .font(.body)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            } else {
                Text(verbatim: shown)
                    .font(.body)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if !a.sources.isEmpty {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(a.sources) { src in sourceLink(src) }
                }
            }

            if !a.notFound && (a.kind == "due" || a.kind == "summary") {
                copyButton(a.answer)   // already a list, or many points: not one reminder or to-do
            } else if !a.notFound && !a.answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 10) { makeButtons(a, turn: turn, named: named); copyButton(a.answer) }
                    VStack(alignment: .leading, spacing: 8) { makeButtons(a, turn: turn, named: named); copyButton(a.answer) }
                }
                if !canMakeReminder(a, named) {
                    Text("Make reminders from each medicine in your profile.")
                        .font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func sourceLink(_ src: Source) -> some View {
        let title = src.title.isEmpty ? String(localized: "Recording") : src.title
        return Button { router.openNote = (src.noteID, src.startSec) } label: {
            Text(verbatim: "▶ \(title) @\(AssistantFormat.mmss(src.startSec))")
                .font(.subheadline.weight(.medium))
                .multilineTextAlignment(.leading)
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(Color.accentColor)
        .accessibilityLabel(Text("Open the recording: \(title) at \(AssistantFormat.mmss(src.startSec))"))
    }

    /// A medicine answer naming several medicines cannot become one reminder (it could not follow each
    /// medicine's dose changes); the person makes one per medicine from the profile. Nor can one whose
    /// medicines the core could not read.
    private func canMakeReminder(_ a: AskAnswer, _ named: [Int64]?) -> Bool {
        guard a.medicine else { return true }
        guard let named else { return false }
        return named.count <= 1
    }

    @ViewBuilder
    private func makeButtons(_ a: AskAnswer, turn: UUID, named: [Int64]?) -> some View {
        Button { Task { await openForm(.reminder, a, turn: turn) } } label: {
            Label("Make reminder", systemImage: "bell").frame(minHeight: 44)
        }
        .buttonStyle(.bordered)
        .disabled(!canMakeReminder(a, named))
        Button { Task { await openForm(.task, a, turn: turn) } } label: {
            Label("Make to-do", systemImage: "checklist").frame(minHeight: 44)
        }
        .buttonStyle(.bordered)
    }

    // MARK: input

    private var inputArea: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let notice {
                Label(notice.text, systemImage: notice == .micReady ? "mic.fill" : "info.circle.fill")
                    .font(.callout)
                    .foregroundStyle(.primary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 4)
            }
            if listening {
                Label("Listening… \(Int(recorder.elapsed)) s of 30. Let go to stop.", systemImage: "waveform")
                    .font(.callout.weight(.medium)).foregroundStyle(Brand.record)
                    .padding(.horizontal, 4)
            } else if transcribing {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Writing down your question…").font(.callout)
                }
                .padding(.horizontal, 4)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    chip("Quiz me", isTelugu ? "నా ఇటీవలి రికార్డింగ్‌ల మీద నాకు క్విజ్ పెట్టండి." : "Quiz me on my recent recordings.")
                    chip("What's due?", isTelugu ? "త్వరలో ఏమేమి చేయాల్సి ఉంది?" : "What's due soon?")
                    chip("Summarise this week", isTelugu ? "ఈ వారం రికార్డింగ్‌ల సారాంశం చెప్పండి." : "Summarise this week's recordings.")
                }
                .padding(.horizontal, 4)
            }
            HStack(alignment: .bottom, spacing: 8) {
                micButton
                TextField("Ask about your recordings…", text: $text, axis: .vertical)
                    .lineLimit(1...5)
                    .focused($fieldFocused)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .frame(minHeight: 44)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 22))
                    .disabled(listening || transcribing)
                    .onChange(of: text) { _, _ in if notice == .busy || notice == .notHeard { notice = nil } }
                if generating {
                    Button { askTask?.cancel() } label: {
                        Image(systemName: "stop.circle.fill").font(.system(size: 34))
                            .frame(width: 44, height: 44)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Color.accentColor)
                    .accessibilityLabel(Text("Stop"))
                } else {
                    Button { send(text) } label: {
                        Image(systemName: "arrow.up.circle.fill").font(.system(size: 34))
                            .frame(width: 44, height: 44)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(canSend ? Color.accentColor : Color.secondary)
                    .disabled(!canSend)
                    .accessibilityLabel(Text("Send"))
                }
            }
        }
        .padding(.horizontal, 12).padding(.top, 8).padding(.bottom, 8)
        .background {
            Rectangle().fill(.bar).overlay(alignment: .top) { Divider() }
        }
    }

    private var canSend: Bool {
        !generating && !listening && !transcribing && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private func chip(_ label: LocalizedStringKey, _ question: String) -> some View {
        Button { send(question, fromField: false) } label: {
            Text(label).font(.subheadline.weight(.medium))
                .padding(.horizontal, 14)
                .frame(minHeight: 44)
                .background(Color.accentColor.opacity(0.12), in: Capsule())
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .foregroundStyle(Color.accentColor)
        .disabled(generating || listening || transcribing)
    }

    /// Hold to record (at most 30 s), let go to stop. VoiceOver: activate to start, activate again to stop.
    private var micButton: some View {
        Image(systemName: listening ? "mic.fill" : "mic")
            .font(.title2)
            .foregroundStyle(listening ? Color.white : Color.accentColor)
            .frame(width: 44, height: 44)
            .background(listening ? Brand.record : Color.accentColor.opacity(0.12), in: Circle())
            .scaleEffect(listening ? 1.15 : 1)
            .animation(.easeOut(duration: 0.15), value: listening)
            .contentShape(Circle())
            .opacity(generating || transcribing ? 0.4 : 1)
            .onLongPressGesture(minimumDuration: Self.maxVoiceSeconds, maximumDistance: 80, perform: { endVoice() },
                                onPressingChanged: { pressing in pressing ? beginVoice() : endVoice() })
            .accessibilityElement()
            .accessibilityAddTraits(.isButton)
            .accessibilityLabel(listening ? Text("Stop and write down my question") : Text("Ask by voice"))
            .accessibilityHint(Text("Hold to speak, then let go."))
            .accessibilityAction { listening ? endVoice() : beginVoice() }
    }

    // MARK: asking

    private func takeDraft() {
        guard let d = router.askDraft else { return }
        router.askDraft = nil
        text = d
    }

    private func send(_ raw: String, fromField: Bool = true) {
        let q = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !q.isEmpty, !generating, !listening, !transcribing else { return }
        // A recording is being processed: the core is busy with it, so do not queue a question behind it.
        if pipeline.isBusy { notice = .busy; return }
        guard models.isReady else { notice = .needsModel; return }
        if Heat.isTooHot(ProcessInfo.processInfo.thermalState) { notice = .tooWarm; return }
        notice = nil
        if fromField { text = "" }
        let id = chats.append(q)
        ask(id, q)
    }

    private func retry(_ turn: ChatTurn) {
        guard !generating else { return }
        if pipeline.isBusy { notice = .busy; return }
        guard models.isReady else { notice = .needsModel; return }
        if Heat.isTooHot(ProcessInfo.processInfo.thermalState) { notice = .tooWarm; return }
        notice = nil
        chats.update(turn.id, .waiting)
        ask(turn.id, turn.question)
    }

    private func ask(_ id: UUID, _ q: String) {
        generating = true
        // The earlier turns of this chat go with it (the core trims them to its token budget).
        let messages = ChatWindow.messages(earlier: chats.turns(before: id).map(\.windowTurn), question: q,
                                           quizAsked: chats.quizAsked(turn: id))
        askTask = Task {
            defer { generating = false; askTask = nil }
            // The person first: memory work stops for the answer. Other work still running: the busy notice.
            guard await pipeline.yieldMemories(), !pipeline.isWorking else { notice = .busy; chats.update(id, .failed); return }
            do {
                try await loadSelectedModel()   // the backend for right now (GPU in front)
                guard !pipeline.isWorking else { notice = .busy; chats.update(id, .failed); return }
                try Task.checkCancellation()
                let a = try await CoreEngine.shared.chat(messages: messages, langHint: profileLang) { partial in
                    // Only while this answer is still being written (a late redraw after the end is dropped).
                    guard isWriting(id) else { return }
                    chats.update(id, .streaming(partial))
                }
                named[id] = await namedMedicines(a)
                if let quizID = a.quizID { chats.markQuizAsked(quizID, turn: id) }
                chats.update(id, .answered(a))
            } catch let stopped as CoreEngine.ChatStopped {
                // Stop: everything already released stays, marked "(stopped)". A late redraw is dropped (isWriting).
                chats.update(id, .stopped(stopped.partial))
            } catch {
                if Task.isCancelled {   // stopped before the core started: nothing was written
                    chats.update(id, .stopped(""))
                    return
                }
                NSLog("[sve] ask failed: %@", String(describing: error))
                chats.update(id, .failed)
            }
        }
    }

    private func state(of id: UUID) -> ChatTurn.State? {
        chats.chats.lazy.flatMap(\.turns).first { $0.id == id }?.state
    }

    private func isWriting(_ id: UUID) -> Bool {
        switch state(of: id) {
        case .waiting, .streaming: return true
        default: return false
        }
    }

    /// The medicine ids of the restored medicine answers of the chat on screen.
    private func loadNamed() async {
        for t in turns {
            guard named[t.id] == nil, case .answered(let a) = t.state, a.medicine else { continue }
            if let ids = await namedMedicines(a) { named[t.id] = ids }
        }
    }

    private func loadSelectedModel() async throws {
        try await models.loadIntoCore()
    }

    private func loadProfile() async {
        profileLang = await CoreEngine.shared.profile()?.lang == "te" ? "te" : "en"
    }

    // MARK: voice question

    private func beginVoice() {
        guard !listening, !transcribing, !generating else { return }
        if pipeline.isBusy { notice = .busy; return }
        guard models.isReady else { notice = .needsModel; return }
        if Heat.isTooHot(ProcessInfo.processInfo.thermalState) { notice = .tooWarm; return }
        switch AVAudioApplication.shared.recordPermission {
        case .granted:
            recorder.start(in: AudioRecorder.askVoiceDirectory)
            if recorder.isRecording {
                listening = true
                notice = nil
                fieldFocused = false
            } else {
                notice = .micFailed
            }
        case .denied:
            micDenied = true
        default:
            // First time: ask, then let the person hold again (the press has usually ended by now).
            Task {
                if await recorder.requestPermission() { notice = .micReady } else { micDenied = true }
            }
        }
    }

    private func endVoice() {
        guard listening else { return }
        listening = false
        guard let (url, seconds) = recorder.stop() else { notice = .micFailed; return }
        if seconds < 1 {
            try? FileManager.default.removeItem(at: url)
            notice = .holdLonger
            return
        }
        transcribing = true
        Task {
            defer {
                try? FileManager.default.removeItem(at: url)
                transcribing = false
            }
            if pipeline.isBusy { notice = .busy; return }
            guard await pipeline.yieldMemories(), !pipeline.isWorking else { notice = .busy; return }
            do {
                try await loadSelectedModel()
                guard !pipeline.isWorking else { notice = .busy; return }
                let pieces = try await CoreEngine.shared.transcribeAudioPieces(path: url.path)
                let heard = pieces.map { $0.text.trimmingCharacters(in: .whitespacesAndNewlines) }
                    .filter { !$0.isEmpty }.joined(separator: " ")
                if heard.isEmpty { notice = .notHeard; return }
                // Into the box for the person to read and fix; they tap Send. Never sent by itself.
                let current = text.trimmingCharacters(in: .whitespacesAndNewlines)
                text = current.isEmpty ? heard : current + " " + heard
                notice = nil
                fieldFocused = true
            } catch {
                NSLog("[sve] voice question failed: %@", String(describing: error))
                notice = .transcribeFailed
            }
        }
    }

    private func cancelVoice() {
        listening = false
        if let (url, _) = recorder.stop() { try? FileManager.default.removeItem(at: url) }
    }

    // MARK: make reminder / to-do

    /// The core's medicine match for a medicine answer ([] for others); nil when it could not be read.
    private func namedMedicines(_ a: AskAnswer) async -> [Int64]? {
        guard a.medicine else { return [] }
        do { return try await CoreEngine.shared.medicinesNamed(a.answer) } catch {
            NSLog("[sve] medicines named failed: %@", String(describing: error))
            return nil
        }
    }

    /// Checks the medicines again (the profile may have changed since the answer), then opens the form.
    private func openForm(_ kind: SuggestionKind, _ a: AskAnswer, turn: UUID) async {
        var medicineID: Int64?
        if kind == .reminder && a.medicine {
            let ids = await namedMedicines(a)
            named[turn] = ids
            guard canMakeReminder(a, ids) else { return }
            medicineID = ids?.first
        }
        formRequest = AnswerFormRequest(kind: kind, text: a.answer, medicine: a.medicine, source: a.sources.first,
                                        medicineID: medicineID)
    }

    private func savePending() {
        guard let p = pendingSave else { return }
        pendingSave = nil
        Task { await save(p.request, p.draft) }
    }

    private func save(_ r: AnswerFormRequest, _ d: SuggestionDraft) async {
        func opt(_ s: String) -> String? {
            let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
            return t.isEmpty ? nil : t
        }
        let words = d.text.trimmingCharacters(in: .whitespacesAndNewlines)
        do {
            if r.kind == .reminder {
                _ = try await CoreEngine.shared.createReminder(
                    NewReminder(text: words, times: d.times, repeat: opt(d.repeatRule), startDate: nil,
                                untilDate: d.untilDate, noteID: r.source?.noteID, startSec: r.source?.startSec,
                                medicineID: r.medicineID, sourceText: r.text))
            } else {
                _ = try await CoreEngine.shared.createTask(
                    NewTask(text: words, who: opt(d.who), dueDate: d.dueDate, noteID: r.source?.noteID, startSec: r.source?.startSec))
            }
        } catch {
            NSLog("[sve] save from answer failed: %@", String(describing: error))
            errorMessage = error.localizedDescription
            return
        }
        await ReminderScheduler.shared.resync()
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        showToast(r.kind == .reminder ? String(localized: "Reminder saved.") : String(localized: "To-do saved."))
        if r.kind == .reminder { await ensureNotificationsAllowed() }
    }

    private func showToast(_ message: String) {
        withAnimation { toast = message }
        UIAccessibility.post(notification: .announcement, argument: message)
        Task {
            try? await Task.sleep(for: .seconds(3))
            if toast == message { withAnimation { toast = nil } }
        }
    }

    /// Asks once for notification permission, as Today does; says so if notifications are off.
    private func ensureNotificationsAllowed() async {
        let status = await UNUserNotificationCenter.current().notificationSettings().authorizationStatus
        switch status {
        case .notDetermined:
            if !(await ReminderScheduler.shared.requestAuthorization()) { notificationsOff = true }
        case .denied:
            notificationsOff = true
        default:
            break
        }
    }

    private var errorShown: Binding<Bool> {
        Binding(get: { errorMessage != nil }, set: { if !$0 { errorMessage = nil } })
    }

    private func openSystemSettings() {
        if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }
    }
}

/// Three dots while an answer is being written.
private struct TypingIndicator: View {
    /// Small and without its own bubble: under text that is still growing.
    var compact = false

    var body: some View {
        SwiftUI.TimelineView(.periodic(from: .now, by: 0.35)) { ctx in
            let step = Int(ctx.date.timeIntervalSinceReferenceDate / 0.35) % 3
            HStack(spacing: 6) {
                ForEach(0..<3, id: \.self) { i in
                    Circle().fill(Color.secondary).frame(width: compact ? 6 : 9, height: compact ? 6 : 9).opacity(i == step ? 1 : 0.3)
                }
            }
            .padding(.horizontal, compact ? 0 : 16).padding(.vertical, compact ? 0 : 14)
            .background(compact ? Color.clear : Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
        }
        .accessibilityElement()
        .accessibilityLabel(Text("Summaverick is writing an answer"))
    }
}
