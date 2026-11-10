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
        /// setTaskCompleted exactly once, from whichever side gets here first. Always as success
        /// (TaskLifecycle.reportsSuccess): iOS shows "Failed" for success false, and work left is only paused. Paused
        /// first says so on the lock screen ("Taking a short break" / PauseWords.paused), with the progress where it was.
        func complete(_ c: TaskLifecycle.Completion) {
            guard completed.setOnce(), let t = task as? BGTask else { return }
            if c == .paused, #available(iOS 26, *), let p = t as? BGContinuedProcessingTask {
                p.updateTitle(String(localized: "Taking a short break"), subtitle: PauseWords.paused)
            }
            t.setTaskCompleted(success: TaskLifecycle.reportsSuccess(c))
        }
    }
    private var held: Held?
    private var wanted = false            // submitted (or running) and work is not done yet
    private var progress = TaskProgress()   // each note run its own 1000 units; never goes back in one task
    private var noteRunning = false         // a note run (startNote) has not ended (endNote) yet
    private var subtitle = ""

    private init() {}

    /// The app may keep working in the background now.
    var isHeld: Bool { held != nil }

    /// The last request was refused (submit threw: for example a Stop from the Live Activity or a shortcut while the
    /// app is in the background). The run then has only the plain background time, and is cut off when it ends.
    private(set) var refused = false

    /// The note's own progress (its notes), in whole percent, for the "paused at" notification and the Live Activity.
    var percent: Int { Int((progress.notesFraction * 100).rounded(.down)) }

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
    /// held or asked for while a note run is in flight is kept (and shows this subtitle): the new note joins it with
    /// its own 1000 units. A held task whose run has ended is completed first and a new one asked for
    /// (TaskLifecycle.onBegin): a finished task is never reused. From the background a request is refused, so there
    /// a held task is kept whatever its state.
    func begin(key: String, subtitle: String) {
        guard #available(iOS 26, *) else { return }
        self.subtitle = subtitle
        if wanted {
            let inFront = UIApplication.shared.applicationState == .active
            guard TaskLifecycle.onBegin(held: true, workLeft: noteRunning) == .finishThenSubmit, inFront else { apply(); return }
            NSLog("[sve] bg: old task ended, new task asked for")
            wanted = false
            complete(.done)
        }
        refused = false
        progress = TaskProgress()
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

    /// The note's notes 0…1 (units 0…900 of its own 1000) and the stage line, shown by the system.
    func update(fraction: Double? = nil, subtitle: String? = nil) {
        if let fraction { move { $0.notes(fraction) } }
        if let subtitle, !subtitle.isEmpty { self.subtitle = subtitle }
        Self.clock.alive()   // the pipeline moved on (a stage change, a step done): the work is alive
        apply()
    }

    /// After the note's notes: its memories and insight, 0…1 of them (units 900…990 of its own 1000).
    func update(afterNotes part: Double) {
        move { $0.afterNotes(part) }
        Self.clock.alive()
        apply()
    }

    private func move(_ change: (inout TaskProgress) -> Void) {
        let before = progress.completedUnits
        change(&progress)
        if progress.completedUnits != before { Self.clock.stamp() }
    }

    /// A note run starts: the first in the task uses its first 1000 units, a later one adds its own 1000, so the
    /// progress never goes back.
    func startNote() {
        progress.startNote()
        noteRunning = true
        Self.clock.stamp()
        apply()
    }

    /// The note run ended (the pipeline is about to finish the task, or to start the next note in it).
    func endNote() { noteRunning = false }

    /// On the note's own units, never past 990: a note at its end is not pushed along, the task is finished.
    private func pulse() {
        guard held != nil, progress.heartbeat() else { return }
        NSLog("[sve] bg: heartbeat, progress %lld/%lld", progress.completedUnits, progress.totalUnits)
        apply()
    }

    /// All work is done (success), or paused until later (work left: never shown as failed). Completes the task
    /// exactly once.
    func finish(success: Bool) {
        if wanted || held != nil {
            if success { NSLog("[sve] bg: task finished (work done)") }
            else { NSLog("[sve] bg: continued processing finished (work left): shown as paused") }
        }
        wanted = false
        refused = false
        complete(TaskLifecycle.completion(workLeft: !success))
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
        t.expirationHandler = {
            // Completed right here, on whatever thread this runs: the app may be suspended before a hop. Paused, not
            // failed: the note is kept and finishes by itself (note 22 showed "failed at 50%" here).
            h.complete(.paused)
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
        t.progress.totalUnitCount = progress.totalUnits
        t.progress.completedUnitCount = progress.completedUnits
        t.updateTitle(Self.title, subtitle: subtitle)
    }

    private func complete(_ c: TaskLifecycle.Completion) {
        guard let h = held else { return }
        held = nil
        if #available(iOS 26, *), c == .done, let t = h.task as? BGContinuedProcessingTask {
            t.progress.completedUnitCount = t.progress.totalUnitCount
        }
        h.complete(c)
    }
}
