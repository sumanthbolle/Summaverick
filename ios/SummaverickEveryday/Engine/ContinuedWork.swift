// iOS 26+: a continued-processing task keeps the notes being written after Stop when the phone is
// locked, with progress on the lock screen. Background runs use the CPU (no .gpu resource is requested).
// Earlier iOS versions keep the plain background task the pipeline always takes.
import BackgroundTasks
import Foundation
import UIKit

@MainActor
final class ContinuedWork {
    static let shared = ContinuedWork()
    /// Matches Info.plist BGTaskSchedulerPermittedIdentifiers.
    static let wildcard = "com.summaverick.everyday.notes.*"
    /// key: known at Stop before anything else runs (the recording's file name), not the note id.
    static func identifier(key: String) -> String {
        let safe = key.unicodeScalars.map { CharacterSet.alphanumerics.contains($0) || $0 == "-" ? String($0) : "-" }.joined()
        return "com.summaverick.everyday.notes.\(safe)"
    }

    /// Called when the system ends the task early: cancel the run, mark the note interrupted. The hold is background
    /// time taken in the expiration handler itself; the callee ends it (after taking its own for each notice).
    var onExpire: ((NotesNotifier.Hold) -> Void)?

    /// The running task and its "completed" flag, which the expiration handler sets on its own thread.
    private final class Held: @unchecked Sendable {
        let task: AnyObject              // BGContinuedProcessingTask
        let completed = AtomicFlag()
        init(_ task: AnyObject) { self.task = task }
        /// setTaskCompleted exactly once, from whichever side gets here first.
        func complete(success: Bool) {
            guard completed.setOnce(), let t = task as? BGTask else { return }
            t.setTaskCompleted(success: success)
        }
    }
    private var held: Held?
    private var wanted = false            // submitted (or running) and work is not done yet
    private var fraction: Double = 0      // the task's progress; never goes back while it is held
    private var noteBase: Double = 0      // the task's progress when the note in flight started
    private var noteDone = false          // the note in flight has all its notes written
    private var subtitle = ""

    private init() {}

    /// The app may keep working in the background now.
    var isHeld: Bool { held != nil }

    /// The last request was refused (submit threw: for example a Stop from the Live Activity or a shortcut while the
    /// app is in the background). The run then has only the plain background time, and is cut off when it ends.
    private(set) var refused = false

    /// The progress shown now, in whole percent (for the "paused at" notification).
    var percent: Int { Int((fraction * 100).rounded(.down)) }

    /// When the progress last moved, and when the work last showed it is alive (any thread: the model's callbacks).
    private final class Clock: @unchecked Sendable {
        private let lock = NSLock()
        private var lastMove = Date()
        private var lastAlive = Date()
        func stamp(_ now: Date = Date()) { lock.lock(); lastMove = now; lastAlive = now; lock.unlock() }
        func alive(_ now: Date = Date()) { lock.lock(); lastAlive = now; lock.unlock() }
        /// True once per heartbeat: the progress stood still for `NotesProgress.heartbeatSeconds` while the work is alive.
        func pulseDue(_ now: Date = Date()) -> Bool {
            lock.lock(); defer { lock.unlock() }
            guard NotesProgress.pulseDue(lastMove: lastMove, lastAlive: lastAlive, now: now) else { return false }
            lastMove = now
            return true
        }
    }
    nonisolated private static let clock = Clock()

    /// From the model's progress callbacks (any thread, every few tokens or every prefill batch): the work is alive.
    /// The heartbeat itself comes from a timer (startPulses), because callbacks can be further apart than iOS
    /// waits for progress.
    nonisolated static func beat() { clock.alive() }

    /// While a task is held: every few seconds, a progress that stood still for `heartbeatSeconds` moves one unit.
    private func startPulses(_ h: Held) {
        Self.clock.alive()
        Task { @MainActor [weak self] in
            while let self, self.held === h {
                try? await Task.sleep(for: .seconds(2))
                guard self.held === h else { return }
                if Self.clock.pulseDue() { self.pulse() }
            }
        }
    }

    static var title: String { String(localized: "Writing your notes") }

    /// Identifiers already given a handler. Registering the same one twice ends the app.
    private var registered: Set<String> = []

    /// Kept for the launch call site. On iOS 26/27 the system does not route a concrete identifier to
    /// a wildcard handler (Apple forums 799126; seen on the owner's iPhone: register returned false), so
    /// each request's own identifier is registered right before it is submitted (allowed for
    /// continued-processing tasks).
    static func register() {}

    @available(iOS 26, *)
    private func registerHandler(_ id: String) {
        guard !registered.contains(id) else { return }
        let ok = BGTaskScheduler.shared.register(forTaskWithIdentifier: id, using: .main) { task in
            MainActor.assumeIsolated { ContinuedWork.shared.started(task) }
        }
        registered.insert(id)
        if !ok { NSLog("[sve] bg: continued processing handler not registered for %@", id) }
    }

    /// Processing starts after Stop: call synchronously in the tap, while the app is in front. A task already
    /// held or asked for with work left is kept (and shows this subtitle); a held task whose work is done is
    /// completed first and a new one asked for (TaskLifecycle.onBegin): a finished task is never reused. From the
    /// background a request is refused, so there a held task is kept whatever its state.
    func begin(key: String, subtitle: String) {
        guard #available(iOS 26, *) else { return }
        self.subtitle = subtitle
        if wanted {
            let inFront = UIApplication.shared.applicationState == .active
            guard TaskLifecycle.onBegin(held: true, workLeft: !noteDone) == .finishThenSubmit, inFront else { apply(); return }
            NSLog("[sve] bg: old task ended, new task asked for")
            wanted = false
            complete(success: true)
        }
        refused = false
        fraction = 0
        noteBase = 0
        noteDone = false
        let id = Self.identifier(key: key)
        registerHandler(id)
        let request = BGContinuedProcessingTaskRequest(identifier: id, title: Self.title, subtitle: subtitle)
        request.strategy = .fail   // now or not at all: the work is already running
        do {
            try BGTaskScheduler.shared.submit(request)
            wanted = true
            NSLog("[sve] bg: continued processing asked for (%@)", id)
        } catch {
            refused = true
            NSLog("[sve] bg: continued processing not available: %@", String(describing: error))
        }
    }

    /// The note's progress 0…1 and the stage line, shown by the system. The note fills `noteShare` of the room the
    /// task has left (TaskLifecycle.rescaled), so the task never goes back and a note that joins it still moves.
    func update(fraction: Double? = nil, subtitle: String? = nil) {
        if let fraction {
            let before = units
            if fraction >= 1 { noteDone = true }
            let task = TaskLifecycle.rescaled(taskFraction: noteBase, noteFraction: fraction * TaskLifecycle.noteShare)
            self.fraction = max(self.fraction, task)
            if units != before { Self.clock.stamp() }
        }
        if let subtitle, !subtitle.isEmpty { self.subtitle = subtitle }
        Self.clock.alive()   // the pipeline moved on (a stage change, a step done): the work is alive
        apply()
    }

    /// A new note starts in the same task: its progress goes on from where the task is, never back to 0.
    func startNote() {
        noteBase = fraction
        noteDone = false
        Self.clock.stamp()
        apply()
    }

    private var units: Int64 { Int64(fraction * 1000) }

    /// Never at done: a task at its end is not pushed along, it is finished.
    private func pulse() {
        let next = NotesProgress.heartbeatFraction(fraction)
        guard held != nil, next > fraction else { return }
        fraction = next
        NSLog("[sve] bg: heartbeat, progress %.3f", fraction)
        apply()
    }

    /// All work is done (or paused until the app is opened). Completes the task exactly once.
    func finish(success: Bool) {
        if wanted || held != nil {
            if success { NSLog("[sve] bg: task finished (work done)") }
            else { NSLog("[sve] bg: continued processing finished (work left)") }
        }
        wanted = false
        refused = false
        complete(success: success)
    }

    private func started(_ t: BGTask) {
        guard #available(iOS 26, *), let t = t as? BGContinuedProcessingTask else {
            t.setTaskCompleted(success: false)
            return
        }
        // Finished before the system started the task: nothing left to do.
        guard wanted, held == nil else { t.setTaskCompleted(success: true); return }
        NSLog("[sve] bg: continued processing started")
        let h = Held(t)
        held = h
        t.progress.totalUnitCount = 1000
        t.expirationHandler = {
            // Completed right here, on whatever thread this runs: the app may be suspended before a hop.
            h.complete(success: false)
            // Before the hop as well: without it the app may be suspended before the "paused" notice starts.
            let hold = NotesNotifier.Hold()
            Task { @MainActor in ContinuedWork.shared.expired(h, hold: hold) }
        }
        apply()
        startPulses(h)
    }

    private func expired(_ h: Held, hold: NotesNotifier.Hold) {
        guard held === h else { hold.end(); return }
        NSLog("[sve] bg: continued processing expired at %ld%%", percent)
        held = nil
        wanted = false
        if let onExpire { onExpire(hold) } else { hold.end() }
    }

    private func apply() {
        guard #available(iOS 26, *), let t = held?.task as? BGContinuedProcessingTask else { return }
        t.progress.completedUnitCount = units
        t.updateTitle(Self.title, subtitle: subtitle)
    }

    private func complete(success: Bool) {
        guard let h = held else { return }
        held = nil
        if #available(iOS 26, *), success { (h.task as? BGContinuedProcessingTask)?.progress.completedUnitCount = 1000 }
        h.complete(success: success)
    }
}
