// The first-time setup (Task L2), as in the owner-approved mockup: welcome, name and language, where notes are
// written, the download, permissions. The tour over the real tabs and "try it" follow in SetupTour.swift.
// The rules (when it shows, which step comes next, which languages can be picked) are SetupFlow in ModelLogic.
import SwiftUI

/// Where the person is in the first-time setup, kept across launches. nil until decided at launch.
@MainActor
@Observable
final class SetupState {
    static let shared = SetupState()
    private(set) var progress: SetupFlow.Progress?
    /// The tour was asked for again from Settings: only the tips, nothing saved.
    private(set) var replaying = false

    /// Once per launch: the stored progress, or setup for a new install and done for an existing one.
    func decide(settings: AppSettings) async {
        guard progress == nil else { return }
        let stored = UserDefaults.standard.string(forKey: SetupFlow.progressKey)
        let hasNotes = !(await CoreEngine.shared.notes(limit: 1)).isEmpty
        let name = await CoreEngine.shared.profile()?.name
        set(SetupFlow.start(stored: stored, hasNotes: hasNotes, consentAcknowledged: settings.consentAcknowledged,
                            profileName: name))
        if progress != .done { ProfileSetupView.shownThisLaunch = true }   // the older profile sheet stays away
    }

    func set(_ p: SetupFlow.Progress) {
        progress = p
        UserDefaults.standard.set(p.rawValue, forKey: SetupFlow.progressKey)
        if p != .setup { UserDefaults.standard.removeObject(forKey: SetupFlow.stepKey) }
    }

    /// After "Delete everything": a fresh start from the welcome. The caller has cleared consent and the other keys.
    func restartAfterWipe() {
        replaying = false
        UserDefaults.standard.removeObject(forKey: SetupFlow.stepKey)
        set(SetupFlow.afterWipe)
    }

    /// "Show me around again": the four tips over the real tabs.
    func replayTour() {
        replaying = true
        progress = .tour
    }

    func finishTour() {
        let r = replaying
        replaying = false
        if r { progress = .done } else { set(SetupFlow.afterTour(replay: false)) }
    }
}

struct GuidedSetupView: View {
    /// The scheme of the app's root (Appearance, or the phone's under System). The welcome is always dark; the other
    /// steps ask for this one, never nil, so a presentation that was dark for the welcome does not stay dark.
    let baseScheme: ColorScheme
    let onFinish: () -> Void

    @Environment(AppSettings.self) private var settings
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var models = ModelManager.shared
    @State private var step: SetupFlow.Step
    @State private var name = ""
    @State private var lang = "en"
    @State private var languagesOpen: Bool
    @State private var saving = false
    @State private var saveFailed = false
    @State private var logoShown = false
    @State private var lifted = false

    /// startAt: nil resumes at the screen the person was on (kept under SetupFlow.stepKey).
    init(baseScheme: ColorScheme, startAt: SetupFlow.Step? = nil, languagesOpen: Bool = false, onFinish: @escaping () -> Void) {
        self.baseScheme = baseScheme
        _step = State(initialValue: startAt ?? SetupFlow.resumeStep(stored: UserDefaults.standard.string(forKey: SetupFlow.stepKey)))
        _languagesOpen = State(initialValue: languagesOpen)
        self.onFinish = onFinish
    }

    var body: some View {
        Group {
            switch step {
            case .welcome: welcome
            case .nameLanguage: page(nameLanguage, cta: "Continue", action: saveNameAndLanguage)
            case .whereNotes: page(whereNotes, cta: "Keep it on this phone") { go(place: .phone) }
            case .download: page(download, cta: models.isReady ? "Continue" : "Continue while it downloads") { go() }
            case .permissions: page(permissions, cta: "Allow both", quiet: ("Not now", { finish() }), action: allowBoth)
            }
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.25), value: step)
        // The welcome is black in both modes: its status bar too.
        .preferredColorScheme(step == .welcome ? .dark : baseScheme)
        .onChange(of: step) { _, s in UserDefaults.standard.set(s.rawValue, forKey: SetupFlow.stepKey) }
        .alert("I couldn't save this. Please try again.", isPresented: $saveFailed) {
            Button("OK", role: .cancel) {}
        }
        .onAppear {
            lang = SetupFlow.initialLanguage(current: settings.notesLanguage, preferred: Locale.preferredLanguages)
            PresentationGate.shared.set("guidedSetup", true)
        }
        .task {
            // Back after leaving mid-setup: the name and language already saved come back.
            if let p = await CoreEngine.shared.profileRead().profile {
                if name.isEmpty, let n = p.name { name = n }
                if let l = p.lang, SetupFlow.languageSelectable(l) { lang = l }
            }
        }
        .onDisappear { PresentationGate.shared.set("guidedSetup", false) }
    }

    // MARK: layout

    /// A setup screen: its content scrolls, the dots and the main button stay at the bottom.
    private func page<V: View>(_ content: V, cta: LocalizedStringKey, quiet: (LocalizedStringKey, () -> Void)? = nil,
                               action: @escaping () -> Void) -> some View {
        VStack(spacing: 0) {
            ScrollView {
                content
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 24)
                    .padding(.top, 28)
                    .padding(.bottom, 16)
            }
            VStack(spacing: 12) {
                SetupDots(lit: SetupFlow.dots(step))
                Button(action: action) {
                    Text(cta).font(.headline).frame(maxWidth: .infinity, minHeight: 50)
                }
                .buttonStyle(.pillProminent)
                .disabled(saving)
                if let quiet {
                    Button(action: quiet.1) {
                        Text(quiet.0).font(.body.weight(.medium)).frame(maxWidth: .infinity, minHeight: 44)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Theme.link)
                    .disabled(saving)
                }
            }
            .padding(.horizontal, 24)
            .padding(.top, 8)
            .padding(.bottom, 16)
        }
        .themedPage()
        .transition(.opacity)
    }

    private func title(_ key: LocalizedStringKey) -> some View {
        Text(key).font(.title.bold()).foregroundStyle(Theme.text)
            .fixedSize(horizontal: false, vertical: true)
            .accessibilityAddTraits(.isHeader)
    }

    private func line(_ key: LocalizedStringKey) -> some View {
        Text(key).font(.body).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
    }

    // MARK: 1. welcome

    /// The owner's logo, unchanged, on black in both modes; it only fades in.
    private var welcome: some View {
        VStack(alignment: .leading, spacing: 0) {
            Image("BrandLogo")
                .resizable()
                .interpolation(.high)
                .scaledToFit()
                .frame(maxWidth: 320, maxHeight: 320)
                // As in the approved mockup: the square's edge melts into the black page. The artwork is untouched.
                .mask(RadialGradient(stops: [.init(color: .black, location: 0.74), .init(color: .clear, location: 1)],
                                    center: .center, startRadius: 0, endRadius: 160))
                .frame(maxWidth: .infinity)
                .opacity(logoShown ? 1 : 0)
                .accessibilityHidden(true)
                .padding(.top, 32)
                .padding(.bottom, 8)
            VStack(alignment: .leading, spacing: 12) {
                Text("Hi, I'm Summaverick.").font(.largeTitle.bold()).foregroundStyle(Theme.text)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
                Text("You listen. I'll take the notes, in your language, right here on your phone.")
                    .font(.title3).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            }
            .padding(.horizontal, 24)
            Spacer(minLength: 16)
            Button { go() } label: {
                Text("Nice to meet you").font(.headline).frame(maxWidth: .infinity, minHeight: 50)
            }
            .buttonStyle(.pillProminent)
            .padding(.horizontal, 24)
            .padding(.bottom, 16)
        }
        .background(Color.black.ignoresSafeArea())
        // Black in both modes, so the welcome uses the Dark colours: near-white text and main pill.
        .environment(\.colorScheme, .dark)
        .onAppear {
            if reduceMotion { logoShown = true } else { withAnimation(.easeOut(duration: 1.4)) { logoShown = true } }
        }
        .transition(.opacity)
    }

    // MARK: 2. name and language

    private var nameLanguage: some View {
        VStack(alignment: .leading, spacing: 12) {
            title("What should I call you?")
            TextField("Your name", text: $name, prompt: Theme.prompt("Your name"))
                .textContentType(.givenName)
                .textInputAutocapitalization(.words)
                .font(.title3)
                .padding(14)
                .frame(minHeight: 50)
                .background(Theme.input, in: RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.pillBorder, lineWidth: 1))
                .submitLabel(.done)
                .padding(.bottom, 12)
            Text("Which language do you prefer?").font(.headline).foregroundStyle(Theme.text)
            LanguageDropdown(selection: $lang, open: $languagesOpen)
            Text("You can change this later.").font(.subheadline).foregroundStyle(Theme.secondary)
        }
    }

    // MARK: 3. where notes are written

    private var whereNotes: some View {
        VStack(alignment: .leading, spacing: 14) {
            title("Where should I write your notes?")
            placeCard(tag: "Private, and my advice", heading: "Right here on this phone",
                      detail: "Your recordings and notes never leave your phone. Works with no internet.",
                      small: "One 5.5 GB download first", chosen: true)
            placeCard(tag: "Coming soon", heading: "With your own ChatGPT, Claude or Copilot",
                      detail: "No big download. What was said goes to that company, under their rules.",
                      small: nil, chosen: false)
        }
        .onAppear {
            if reduceMotion { lifted = true } else {
                withAnimation(.spring(response: 0.45, dampingFraction: 0.8).delay(0.15)) { lifted = true }
            }
        }
    }

    /// The chosen card lifts gently (still with Reduce Motion: it is simply drawn lifted). The other cannot be picked.
    private func placeCard(tag: LocalizedStringKey, heading: LocalizedStringKey, detail: LocalizedStringKey,
                           small: LocalizedStringKey?, chosen: Bool) -> some View {
        let up = chosen && lifted
        return VStack(alignment: .leading, spacing: 6) {
            Text(tag).font(.footnote.weight(.semibold))
                .foregroundStyle(chosen ? Theme.primaryLabel : Theme.secondary)
                .padding(.horizontal, 10).padding(.vertical, 3)
                .background(chosen ? Theme.primaryFill : Theme.chip, in: Capsule())
            HStack(alignment: .firstTextBaseline) {
                Text(heading).font(.headline).foregroundStyle(chosen ? Theme.text : Theme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
                if chosen {
                    Image(systemName: "checkmark.circle.fill").font(.title3).foregroundStyle(Theme.accent)
                        .accessibilityHidden(true)
                }
            }
            Text(detail).font(.subheadline).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            if let small {
                Text(small).font(.footnote).foregroundStyle(Theme.secondary).padding(.top, 2)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 18))
        .overlay(RoundedRectangle(cornerRadius: 18)
            .stroke(chosen ? Theme.primaryFill : Theme.pillBorder, lineWidth: chosen ? 2 : 1))
        .shadow(color: .black.opacity(up ? 0.14 : 0), radius: up ? 14 : 0, y: up ? 8 : 0)
        .offset(y: up ? -3 : 0)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(chosen ? .isSelected : [])
        .accessibilityHint(chosen ? Text(verbatim: "") : Text("Coming soon"))
    }

    // MARK: 4. download

    private var download: some View {
        let status = models.status
        let p = status.isReady ? 1 : (status.progress ?? 0)
        return VStack(alignment: .leading, spacing: 16) {
            title("One big download, then no internet needed.")
            ZStack {
                Circle().stroke(Theme.chip, lineWidth: 14)
                Circle().trim(from: 0, to: p)
                    .stroke(Theme.primaryFill, style: StrokeStyle(lineWidth: 14, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                VStack(spacing: 2) {
                    Text(verbatim: "\(Int((p * 100).rounded()))%").font(.largeTitle.bold()).foregroundStyle(Theme.text)
                    Text(verbatim: "5.5 GB").font(.footnote).foregroundStyle(Theme.secondary)
                }
            }
            .frame(width: 176, height: 176)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 4)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(status.settingsLine))
            if waiting(status) {   // Wi-Fi, a connection, space, or a stopped download: say what it waits for
                Label(status.line, systemImage: status.symbol).font(.subheadline).foregroundStyle(Theme.text)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let d = status.detail {
                Text(verbatim: d).font(.subheadline).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if status.canRetry {
                Button { models.startIfNeeded(userInitiated: true) } label: {
                    Text("Try again").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.pill)
            }
            line(status.isReady ? "All set. I don't need the internet again."
                 : "No need to wait here. Close the app if you like, and I'll tell you when I'm ready.")
            if !status.isReady {
                Divider()
                AICellularToggle().foregroundStyle(Theme.text)
            }
        }
    }

    private func waiting(_ s: AIStatus) -> Bool {
        switch s {
        case .ready, .downloading, .unsupported: return false
        default: return true
        }
    }

    // MARK: 5. permissions

    private var permissions: some View {
        VStack(alignment: .leading, spacing: 20) {
            title("Two small things I need from you")
            permission("mic.fill", "Your microphone", "So I can hear the lecture. Only when you tap record.")
            permission("bell.fill", "Notifications", "So I can tell you when your notes are ready, and ring your reminders.")
            Text("Only record people who know they are being recorded and are fine with it.")
                .font(.subheadline).foregroundStyle(Theme.text)
                .fixedSize(horizontal: false, vertical: true)
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Theme.card, in: RoundedRectangle(cornerRadius: 14))
        }
    }

    private func permission(_ icon: String, _ heading: LocalizedStringKey, _ detail: LocalizedStringKey) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: icon).font(.title3).foregroundStyle(Theme.text)
                .frame(width: 44, height: 44)
                .background(Theme.card, in: RoundedRectangle(cornerRadius: 12))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(heading).font(.headline).foregroundStyle(Theme.text)
                Text(detail).font(.subheadline).foregroundStyle(Theme.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .accessibilityElement(children: .combine)
    }

    // MARK: actions

    private func go(place: SetupFlow.NotesPlace = .phone) {
        let needed = models.status != .unsupported && !models.isReady
        if let n = SetupFlow.next(after: step, place: place, downloadNeeded: needed) { step = n } else { finish() }
    }

    /// The name goes into the profile (the existing save path, which also keeps the first name for notifications);
    /// the language is the notes language and the profile's preferred one.
    private func saveNameAndLanguage() {
        guard !saving else { return }
        saving = true
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let code = lang
        Task {
            // A new profile only when the core says there is none; a failed read saves nothing.
            let read = await CoreEngine.shared.profileRead()
            guard read.profile != nil || SetupFlow.mayCreateProfile(read.kind) else {
                saving = false
                saveFailed = true
                return
            }
            var p = read.profile ?? Profile()
            if !trimmed.isEmpty { p.name = trimmed }
            p.lang = code
            do {
                try await CoreEngine.shared.setProfile(p)
                await ProfileActions.changed()
                settings.notesLanguage = code
                saving = false
                go()
            } catch {
                saving = false
                saveFailed = true
            }
        }
    }

    /// The agreeing button: microphone, then notifications, through the existing requests. Only here is the
    /// recording reminder marked as understood.
    private func allowBoth() {
        guard !saving else { return }
        saving = true
        Task {
            _ = await AudioRecorder().requestPermission()
            _ = await ReminderScheduler.shared.requestAuthorization()
            settings.consentAcknowledged = true
            saving = false
            finish()
        }
    }

    private func finish() { onFinish() }
}

/// The language list as a dropdown: tap to open it in place. English and Telugu can be picked; the rest say
/// "Coming soon".
private struct LanguageDropdown: View {
    @Binding var selection: String
    @Binding var open: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(spacing: 0) {
            Button { toggle() } label: {
                HStack {
                    Text(verbatim: native(selection)).font(.body.weight(.semibold)).foregroundStyle(Theme.text)
                    Spacer()
                    Image(systemName: open ? "chevron.up" : "chevron.down").font(.body.weight(.semibold))
                        .foregroundStyle(Theme.secondary).accessibilityHidden(true)
                }
                .padding(.horizontal, 14)
                .frame(minHeight: 50)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Which language do you prefer?"))
            .accessibilityValue(Text(verbatim: native(selection)))
            if open {
                ForEach(SetupFlow.languages, id: \.code) { l in
                    Divider().padding(.leading, 14)
                    row(l.code, l.native)
                }
            }
        }
        .background(Theme.input, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.pillBorder, lineWidth: 1))
    }

    private func row(_ code: String, _ native: String) -> some View {
        let can = SetupFlow.languageSelectable(code)
        let on = selection == code
        return Button {
            selection = SetupFlow.choose(code, current: selection)
            toggle()
        } label: {
            HStack {
                Text(verbatim: native).font(on ? .body.weight(.semibold) : .body)
                    .foregroundStyle(can ? Theme.text : Theme.secondary)
                Spacer()
                if on {
                    Image(systemName: "checkmark").font(.body.weight(.semibold)).foregroundStyle(Theme.accent)
                        .accessibilityHidden(true)
                } else if !can {
                    Text("Coming soon").font(.footnote.weight(.semibold)).foregroundStyle(Theme.secondary)
                        .padding(.horizontal, 8).padding(.vertical, 2)
                        .background(Theme.chip, in: Capsule())
                }
            }
            .padding(.horizontal, 14)
            .frame(minHeight: 48)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!can)
        .accessibilityAddTraits(on ? .isSelected : [])
    }

    private func toggle() {
        if reduceMotion { open.toggle() } else { withAnimation(.easeInOut(duration: 0.2)) { open.toggle() } }
    }

    private func native(_ code: String) -> String { SetupFlow.languages.first { $0.code == code }?.native ?? code }
}

/// Seven dots: how far through the first-time setup the person is. Not read out (each screen's title says it).
struct SetupDots: View {
    let lit: Int
    var body: some View {
        HStack(spacing: 7) {
            ForEach(0..<SetupFlow.dotCount, id: \.self) { i in
                Circle().fill(i < lit ? Theme.text : Theme.hairline).frame(width: 7, height: 7)
            }
        }
        .accessibilityHidden(true)
    }
}
