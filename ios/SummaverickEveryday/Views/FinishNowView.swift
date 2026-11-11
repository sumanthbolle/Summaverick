// "Finish now": a dim, dark, full-screen view that keeps the app in front until a note's notes are ready. In front
// the notes are written on the GPU (about 4 times the locked phone's CPU), so a long lecture finishes in minutes.
// Offered after Stop (RecordView), on a note whose notes are still being written (NoteDetailView), and as the
// "Finish now" action of a note's notes notification (NotesNotifier, handled in ReminderScheduler → Router).
//
// It lives in a window of its own above the app's, so it covers any sheet or cover without a presentation fight, and
// it closes when the app goes to the background (locked, switched away): the work carries on there, nothing lost,
// and the app lock's cover is never under it. The brightness is dimmed and given back by FinishNowSession (ModelLogic):
// on leaving, on finishing, on going to the background, while the app is inactive and when the view goes away. The
// screen stays on through ScreenAwake, the one owner of the idle timer (the mode or a notes run in front).
import SwiftUI
import UIKit

@MainActor
@Observable
final class FinishNowController {
    static let shared = FinishNowController()

    enum Target: Equatable {
        /// A note whose id is known.
        case note(Int64)
        /// Right after a Stop: the note is not saved yet; the first note newer than this one (FinishNow.followed).
        case following(newerThan: Int64)
    }

    private(set) var target: Target?
    /// The brightness half of the mode; read by the view to show "Your notes are ready".
    private(set) var session = FinishNowSession() { didSet { ScreenAwake.update() } }

    @ObservationIgnored private var window: UIWindow?
    @ObservationIgnored private weak var previousKey: UIWindow?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var fallback: Task<Void, Never>?

    private init() {
        let c = NotificationCenter.default
        observers.append(c.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { FinishNowController.shared.wentToBackground() }
        })
        observers.append(c.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { FinishNowController.shared.resignedActive() }
        })
        observers.append(c.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { FinishNowController.shared.becameActive() }
        })
    }

    var noteID: Int64? { if case .note(let id) = target { return id } else { return nil } }

    /// Opens the mode on a note.
    func start(noteID id: Int64) { open(.note(id)) }

    /// Opens the mode after a Stop, before the note exists. newest: the highest note id before the recording. If no
    /// new note run starts within FinishNow.followFallbackSeconds, it follows the newest note by id.
    func startAfterStop(newerThan newest: Int64) {
        open(.following(newerThan: newest))
        fallback?.cancel()
        fallback = Task { [weak self] in
            try? await Task.sleep(for: .seconds(FinishNow.followFallbackSeconds))
            guard !Task.isCancelled, case .following(let n)? = self?.target else { return }
            let id = await Self.highestNoteID()
            guard id > n, case .following? = self?.target else { return }
            NSLog("[sve] finish now: no run started after the Stop; following note %lld", id)
            self?.latch(id)
        }
    }

    /// The highest note id (ids only grow, so it is the newest note); 0 with none.
    nonisolated static func highestNoteID() async -> Int64 {
        await CoreEngine.shared.notes(limit: 200).map(\.id).max() ?? 0
    }

    /// The note the pipeline started after a Stop: the mode is on it from now, and the note opens under the mode.
    func latch(_ id: Int64) {
        guard case .following = target else { return }
        fallback?.cancel()
        target = .note(id)
        ReminderScheduler.shared.pendingRoute = .note(id: id, startSec: nil)
    }

    /// The notes are ready: the brightness comes back and the screen may sleep (unless a run in front keeps it on).
    func finished() { setBrightness(session.finish()) }

    /// Leave (or Done): back to the app; the notes go on being written, on the GPU while the app is in front.
    func close() {
        setBrightness(session.leave())
        tearDown()
    }

    /// The view went away by some other path (the window was torn down): give the screen back.
    func viewGone() { setBrightness(session.leave()) }

    private func wentToBackground() {
        guard window != nil else { return }
        NSLog("[sve] finish now: the app went to the background; the work goes on there")
        setBrightness(session.background())
        tearDown()
    }

    /// Control Center, a call, the app switcher: the person sees the phone at its own brightness.
    private func resignedActive() { setBrightness(session.resignActive()) }

    /// Back in front with the mode on: dim again.
    private func becameActive() {
        guard window != nil else { return }
        setBrightness(session.becomeActive(brightness: currentBrightness))
    }

    private func open(_ t: Target) {
        guard !RecordingController.shared.isRecording else { return }
        target = t
        if window == nil {
            guard let scene = Self.scene else { NSLog("[sve] finish now: no window scene"); target = nil; return }
            let w = UIWindow(windowScene: scene)
            w.windowLevel = .alert + 1   // above the app's sheets and covers
            w.overrideUserInterfaceStyle = .dark
            let host = UIHostingController(rootView: FinishNowView())
            host.view.backgroundColor = Theme.uiColor(.background)
            host.view.accessibilityViewIsModal = true   // VoiceOver stays in the mode, not the app under it
            w.rootViewController = host
            previousKey = scene.keyWindow
            w.makeKeyAndVisible()
            window = w
        }
        // Dimmed only in front; opened while inactive (a notification action), didBecomeActive dims it.
        if UIApplication.shared.applicationState == .active {
            setBrightness(session.enter(brightness: currentBrightness))
        } else if session.phase != .on {
            _ = session.enter(brightness: currentBrightness)   // on, not dimmed yet: nothing set
            _ = session.resignActive()
        }
        NSLog("[sve] finish now: on (%@)", String(describing: t))
    }

    private func tearDown() {
        fallback?.cancel()
        fallback = nil
        target = nil
        guard let w = window else { return }
        w.isHidden = true
        w.rootViewController = nil
        window = nil
        previousKey?.makeKey()
        NSLog("[sve] finish now: off")
    }

    private var screen: UIScreen? { window?.windowScene?.screen ?? Self.scene?.screen }
    private var currentBrightness: Double { Double(screen?.brightness ?? 1) }

    private func setBrightness(_ b: Double?) {
        guard let b else { return }
        screen?.brightness = CGFloat(b)
    }

    private static var scene: UIWindowScene? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        return scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
    }
}

struct FinishNowView: View {
    @State private var controller = FinishNowController.shared
    @State private var pipeline = ProcessingPipeline.shared
    @State private var recording = RecordingController.shared
    @State private var note: Note?
    /// Parts written of the parts the note will have: the one measure of the headline and the bar.
    @State private var parts: (done: Int, total: Int) = (0, 0)
    @State private var heat: HeatLadder.Level = .full

    var body: some View {
        VStack(spacing: 24) {
            Spacer()
            BrandMark(size: 44).opacity(0.8)
            if let title = note?.title, !title.isEmpty {
                Text(title)
                    .font(.title2.weight(.semibold))
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
            }
            if controller.session.phase == .finished || ready {
                readyBody
            } else {
                workingBody
            }
            Spacer()
            Button { controller.close() } label: {
                Text(ready ? "Done" : "Leave").frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(ready ? .pillProminent : .pill)
            .padding(.bottom, 24)
        }
        .padding(.horizontal, 32)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Theme.background.ignoresSafeArea())
        .tint(Theme.accent)
        .foregroundStyle(Theme.text, Theme.secondary, Theme.secondary)
        .preferredColorScheme(.dark)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .task(id: noteID) { await load() }
        .task {
            while !Task.isCancelled {
                await readParts()
                heat = HeatGovernor.shared.level
                try? await Task.sleep(for: .seconds(2))
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await load() } }
        .onReceive(NotificationCenter.default.publisher(for: .heatLevelChanged)) { _ in heat = HeatGovernor.shared.level }
        .onChange(of: pipeline.activeNoteID, initial: true) { _, id in
            if case .following(let newest) = controller.target, let id = FinishNow.followed(activeNoteID: id, newerThan: newest) {
                controller.latch(id)
            }
        }
        .onChange(of: ready, initial: true) { _, r in if r { controller.finished() } }
        // A recording started (a shortcut, the control): the recorder comes first.
        .onChange(of: recording.isRecording) { _, r in if r { controller.close() } }
        .onDisappear { controller.viewGone() }
    }

    private var readyBody: some View {
        VStack(spacing: 12) {
            Image(systemName: "checkmark.circle").font(.system(size: 44)).foregroundStyle(Theme.positiveIcon)
                .accessibilityHidden(true)
            Text("Your notes are ready").font(.title3.weight(.semibold))
        }
        .accessibilityElement(children: .combine)
    }

    private var workingBody: some View {
        VStack(spacing: 14) {
            Text(headline).font(.title3.weight(.semibold)).multilineTextAlignment(.center)
                .contentTransition(.numericText())
            ProgressView(value: FinishNow.fraction(done: parts.done, total: parts.total))   // VoiceOver reads its percent
                .tint(Theme.text)   // steel, as the app look (no blue)
            if !detail.isEmpty {
                Text(detail).font(.subheadline).foregroundStyle(Theme.secondary).multilineTextAlignment(.center)
            }
            if FinishNow.resting(heat) {
                Label("The phone is warm, so I'm resting", systemImage: "thermometer.medium")
                    .font(.subheadline).foregroundStyle(Theme.caution)
            }
            Text("Keep Summaverick open. You can put the phone face-up on the desk.")
                .font(.footnote).foregroundStyle(Theme.secondary).multilineTextAlignment(.center)
                .padding(.top, 8)
        }
    }

    private var noteID: Int64? { controller.noteID }

    /// The pipeline is working on this note now.
    private var thisNote: Bool { noteID != nil && pipeline.activeNoteID == noteID }

    /// Its notes are being written now (memories, after the notes, do not count).
    private var writing: Bool {
        guard thisNote, pipeline.isWorking else { return false }
        if case .memories = pipeline.stage { return false }
        return true
    }

    private var ready: Bool {
        guard noteID != nil else { return false }
        let steps = thisNote ? pipeline.stepCount : nil
        let hasNotes = [note?.short_summary, note?.detailed_notes, note?.revision_questions]
            .allSatisfy { !($0 ?? "").isEmpty }
        return FinishNow.notesReady(stepsReady: steps?.ready.count, stepsTotal: steps?.total, hasNotes: hasNotes, writing: writing)
    }

    /// "Writing your notes · 6 of 16 parts"; "Working on it…" before the note is known.
    private var headline: String {
        guard noteID != nil, parts.total > 0 else { return String(localized: "Working on it…") }
        return String(localized: "Writing your notes · \(parts.done) of \(parts.total) parts")
    }

    /// Only why this note is not moving: waiting behind another note, or paused (its line). Nothing while it is
    /// being written, so the screen has one measure.
    private var detail: String {
        guard let id = noteID, !writing else { return "" }
        if pipeline.isWorking && !thisNote { return ProcessingPipeline.queuedMessage }
        return pipeline.lastFailure[id] ?? ""
    }

    /// The parts from the store (sections and their outputs), as the pipeline's steps see them.
    private func readParts() async {
        guard let id = noteID, let n = note else { return }
        let sections = await CoreEngine.shared.sections(id).count
        let state = await CoreEngine.shared.sectionOutputsState(noteID: id, lang: n.lang) ?? []
        let plan = NoteSteps.plan(sections: sections, state: state, hasSummary: true)
        let p = FinishNow.partsProgress(sectionsUndone: FinishNow.sectionsUndone(plan), sectionsKnown: sections,
                                        durationSeconds: Double(n.duration_seconds))
        if p.done != parts.done || p.total != parts.total { parts = p }
    }

    private func load() async {
        guard let id = noteID else { note = nil; return }
        note = await CoreEngine.shared.note(id)
        await readParts()
    }
}
