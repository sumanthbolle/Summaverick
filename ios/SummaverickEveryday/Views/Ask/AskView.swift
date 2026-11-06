// Ask Summaverick (spec §6.3): a chat over all recordings, typed or spoken. Answers come from the core
// (sve_assistant_ask) and are shown as given: medicine answers are quoted word for word, never reworded,
// and nothing is added to them. Every answer links to the moments it came from. The chat lives in memory
// for this session only.
import AVFoundation
import SwiftUI
import UserNotifications

/// One question and what came back.
private struct AskTurn: Identifiable {
    enum Phase {
        case waiting
        case answered(AskAnswer)
        case failed
    }
    let id = UUID()
    let question: String
    var state: Phase
}

/// What the "Make reminder" / "Make to-do" form is opened for.
private struct AnswerFormRequest: Identifiable {
    let id = UUID()
    let kind: SuggestionKind      // .reminder or .task
    let text: String
    let medicine: Bool
    let source: Source?

    /// A card that exists only on this screen: the answer text, no times or dates (never invented).
    var suggestion: Suggestion {
        var p = SuggestionPayload()
        p.text = text
        if kind == .reminder {
            p.times = []
            p.repeat = "once"
        }
        return Suggestion(local: kind, payload: p, noteID: source?.noteID, startSec: source?.startSec)
    }
}

/// A short message above the question box.
private enum AskNotice: Equatable {
    case busy, needsModel, micReady, holdLonger, notHeard, transcribeFailed, micFailed

    var text: LocalizedStringKey {
        switch self {
        case .busy: return "Busy with a recording — try again in a moment."
        case .needsModel: return "Download the AI model in Settings first."
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

    @State private var turns: [AskTurn] = []
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
    private var presenting: Bool { formRequest != nil || errorMessage != nil || notificationsOff || micDenied }

    var body: some View {
        NavigationStack {
            Group {
                if models.selected.hearsAudio {
                    chat
                } else {
                    needsGemma
                }
            }
            .navigationTitle("Ask Summaverick")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { router.showSettings = true } label: {
                        Image(systemName: "gearshape").frame(minWidth: 44, minHeight: 44)
                    }
                    .accessibilityLabel(Text("Settings"))
                }
            }
        }
        .task { await loadProfile() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await loadProfile() } }
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

    // MARK: needs Gemma 4

    private var needsGemma: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Label("Needs Gemma 4", systemImage: "waveform.badge.exclamationmark").font(.title3.bold())
                Text("Ask Summaverick needs the Gemma 4 model. Your notes and recording still work.")
                    .font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Button { router.showSettings = true } label: {
                    Text("Open Settings").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.borderedProminent)
            }
            .padding(16)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
            .padding(16)
        }
    }

    // MARK: chat

    private var chat: some View {
        VStack(spacing: 0) {
            if !models.selectedInstalled && !pipeline.isBusy {
                ModelNudge { router.showSettings = true }
            }
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
            Text("Answers come only from your recordings, with a link to the moment they came from.")
                .font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, 8)
    }

    @ViewBuilder
    private func turnView(_ turn: AskTurn) -> some View {
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
            case .answered(let a):
                answerView(a)
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
    private var notFoundText: String {
        isTelugu ? "మీ రికార్డింగ్‌లలో ఇది నేను వినలేదు." : "I didn't hear that in your recordings."
    }

    private func answerView(_ a: AskAnswer) -> some View {
        let shown = a.answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? notFoundText : a.answer
        let quoted = a.medicine && shown == a.answer
        return VStack(alignment: .leading, spacing: 10) {
            if quoted {
                // Built in the core from saved facts and recorded words: shown exactly, in full.
                VStack(alignment: .leading, spacing: 6) {
                    Label("Copied from your recordings", systemImage: "quote.opening")
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

            if !a.notFound && !a.answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 10) { makeButtons(a) }
                    VStack(alignment: .leading, spacing: 8) { makeButtons(a) }
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
        .accessibilityLabel(Text("Open the recording: \(title)"))
    }

    @ViewBuilder
    private func makeButtons(_ a: AskAnswer) -> some View {
        Button { openForm(.reminder, a) } label: {
            Label("Make reminder", systemImage: "bell").frame(minHeight: 44)
        }
        .buttonStyle(.bordered)
        Button { openForm(.task, a) } label: {
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
        guard models.selectedInstalled else { notice = .needsModel; return }
        notice = nil
        if fromField { text = "" }
        let turn = AskTurn(question: q, state: .waiting)
        turns.append(turn)
        ask(turn.id, q)
    }

    private func retry(_ turn: AskTurn) {
        guard !generating else { return }
        if pipeline.isBusy { notice = .busy; return }
        guard models.selectedInstalled else { notice = .needsModel; return }
        notice = nil
        update(turn.id, .waiting)
        ask(turn.id, turn.question)
    }

    private func ask(_ id: UUID, _ q: String) {
        generating = true
        Task {
            defer { generating = false }
            do {
                try await loadSelectedModel()
                let a = try await CoreEngine.shared.ask(q)
                update(id, .answered(a))
            } catch {
                NSLog("[sve] ask failed: %@", String(describing: error))
                update(id, .failed)
            }
        }
    }

    private func update(_ id: UUID, _ state: AskTurn.Phase) {
        guard let i = turns.firstIndex(where: { $0.id == id }) else { return }
        turns[i].state = state
    }

    private func loadSelectedModel() async throws {
        let spec = models.selected
        try await CoreEngine.shared.loadModel(at: models.fileURL(for: spec).path, contextTokens: spec.contextTokens,
                                              mmprojPath: models.mmprojURL(for: spec)?.path)
    }

    private func loadProfile() async {
        profileLang = await CoreEngine.shared.profile()?.lang == "te" ? "te" : "en"
    }

    // MARK: voice question

    private static var voiceDirectory: URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ask-voice", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func beginVoice() {
        guard !listening, !transcribing, !generating else { return }
        if pipeline.isBusy { notice = .busy; return }
        guard models.selectedInstalled else { notice = .needsModel; return }
        switch AVAudioApplication.shared.recordPermission {
        case .granted:
            recorder.start(in: Self.voiceDirectory)
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
            do {
                try await loadSelectedModel()
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

    private func openForm(_ kind: SuggestionKind, _ a: AskAnswer) {
        formRequest = AnswerFormRequest(kind: kind, text: a.answer, medicine: a.medicine, source: a.sources.first)
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
                                untilDate: d.untilDate, noteID: r.source?.noteID, startSec: r.source?.startSec))
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
    var body: some View {
        SwiftUI.TimelineView(.periodic(from: .now, by: 0.35)) { ctx in
            let step = Int(ctx.date.timeIntervalSinceReferenceDate / 0.35) % 3
            HStack(spacing: 6) {
                ForEach(0..<3, id: \.self) { i in
                    Circle().fill(Color.secondary).frame(width: 9, height: 9).opacity(i == step ? 1 : 0.3)
                }
            }
            .padding(.horizontal, 16).padding(.vertical, 14)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 18))
        }
        .accessibilityElement()
        .accessibilityLabel(Text("Summaverick is writing an answer"))
    }
}
