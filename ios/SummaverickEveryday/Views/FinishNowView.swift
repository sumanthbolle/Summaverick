// "Finish now": a dim, dark, full-screen view that keeps the app in front until a note's notes are ready. In front
// the notes are written on the GPU (about 4 times the locked phone's CPU), so a long lecture finishes in minutes.
// Offered after Stop (RecordView), on a note whose notes are still being written (NoteDetailView), and as the
// "Finish now" action of a note's notes notification (NotesNotifier, handled in ReminderScheduler → Router).
//
// It lives in a window of its own above the app's, so it covers any sheet or cover without a presentation fight, and
// it closes when the app goes to the background (locked, switched away): the work carries on there, nothing lost,
// and the app lock's cover is never under it. The screen's brightness and the idle timer are set and given back by
// FinishNowSession (ModelLogic): on leaving, on finishing, on going to the background and when the view goes away.
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
    /// The screen half of the mode; read by the view to show "Your notes are ready".
    private(set) var session = FinishNowSession()

    @ObservationIgnored private var window: UIWindow?
    @ObservationIgnored private weak var previousKey: UIWindow?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []

    private init() {
        observers.append(NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification,
                                                                object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { FinishNowController.shared.wentToBackground() }
        })
    }

    var noteID: Int64? { if case .note(let id) = target { return id } else { return nil } }

    /// Opens the mode on a note.
    func start(noteID id: Int64) { open(.note(id)) }

    /// Opens the mode after a Stop, before the note exists. newest: the newest note id before the recording.
    func startAfterStop(newerThan newest: Int64) { open(.following(newerThan: newest)) }

    /// The note the pipeline started after a Stop: the mode is on it from now, and the note opens under the mode.
    func latch(_ id: Int64) {
        guard case .following = target else { return }
        target = .note(id)
        ReminderScheduler.shared.pendingRoute = .note(id: id, startSec: nil)
    }

    /// The notes are ready: the screen comes back and may sleep again; the mode stays to say so.
    func finished() { apply(session.finish()) }

    /// Leave (or Done): back to the app; the notes go on being written, on the GPU while the app is in front.
    func close() {
        apply(session.leave())
        tearDown()
    }

    /// The view went away by some other path (the window was torn down): give the screen back.
    func viewGone() { apply(session.leave()) }

    private func wentToBackground() {
        guard window != nil else { return }
        NSLog("[sve] finish now: the app went to the background; the work goes on there")
        apply(session.background())
        tearDown()
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
            w.rootViewController = host
            previousKey = scene.keyWindow
            w.makeKeyAndVisible()
            window = w
        }
        apply(session.enter(brightness: Double(window?.windowScene?.screen.brightness ?? 1)))
        NSLog("[sve] finish now: on (%@)", String(describing: t))
    }

    private func tearDown() {
        target = nil
        guard let w = window else { return }
        w.isHidden = true
        w.rootViewController = nil
        window = nil
        previousKey?.makeKey()
        NSLog("[sve] finish now: off")
    }

    private func apply(_ screen: FinishNowSession.Screen?) {
        guard let screen else { return }
        UIApplication.shared.isIdleTimerDisabled = screen.idleTimerDisabled
        if let s = window?.windowScene?.screen ?? Self.scene?.screen { s.brightness = CGFloat(screen.brightness) }
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
    @State private var percent = 0
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
                percent = ContinuedWork.shared.percent
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
            ProgressView(value: thisNote ? Double(percent) / 100 : 0)   // VoiceOver reads its percent
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

    /// "Writing your notes · 1 of 3" while this note's counted run is on; "Working on it…" before it starts.
    private var headline: String {
        if thisNote, let steps = pipeline.stepCount, steps.counts {
            return String(localized: "Writing your notes · \(steps.ready.count) of \(steps.total)")
        }
        return String(localized: "Working on it…")
    }

    /// What is being done now (the stage), or why this note waits.
    private var detail: String {
        guard noteID != nil else { return "" }
        guard thisNote else {
            return pipeline.isWorking ? ProcessingPipeline.queuedMessage : (noteID.flatMap { pipeline.lastFailure[$0] } ?? "")
        }
        if case .transcribing = pipeline.stage, let t = pipeline.transcribing,
           let line = StepCount.transcribingLine(transcribedSeconds: t.done, durationSeconds: t.duration) { return line }
        return pipeline.stage.label
    }

    private func load() async {
        guard let id = noteID else { note = nil; return }
        note = await CoreEngine.shared.note(id)
    }
}
