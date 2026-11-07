// iOS 26+: a continued-processing task keeps the notes being written after Stop when the phone is
// locked, with progress on the lock screen. Background runs use the CPU (no .gpu resource is requested).
// Earlier iOS versions keep the plain background task the pipeline always takes.
import BackgroundTasks
import Foundation

@MainActor
final class ContinuedWork {
    static let shared = ContinuedWork()
    /// Registered once at launch; matches Info.plist BGTaskSchedulerPermittedIdentifiers.
    static let wildcard = "com.summaverick.everyday.notes.*"
    /// key: known at Stop before anything else runs (the recording's file name), not the note id.
    static func identifier(key: String) -> String {
        let safe = key.unicodeScalars.map { CharacterSet.alphanumerics.contains($0) || $0 == "-" ? String($0) : "-" }.joined()
        return "com.summaverick.everyday.notes.\(safe)"
    }

    /// Called when the system ends the task early: cancel the run, mark the note interrupted.
    var onExpire: (() -> Void)?

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
    private var fraction: Double = 0
    private var subtitle = ""

    private init() {}

    /// The app may keep working in the background now.
    var isHeld: Bool { held != nil }

    static var title: String { String(localized: "Writing your notes") }

    /// At launch, before the first submit. Registering twice would end the app, so only here.
    static func register() {
        guard #available(iOS 26, *) else { return }
        let ok = BGTaskScheduler.shared.register(forTaskWithIdentifier: wildcard, using: .main) { task in
            MainActor.assumeIsolated { ContinuedWork.shared.started(task) }
        }
        if !ok { NSLog("[sve] continued processing not registered (Info.plist identifiers?)") }
    }

    /// Processing starts after Stop: call synchronously in the tap, while the app is in front. Submits
    /// once; a task already held or asked for is kept (and shows this subtitle).
    func begin(key: String, subtitle: String) {
        guard #available(iOS 26, *) else { return }
        self.subtitle = subtitle
        guard !wanted else { apply(); return }
        fraction = 0
        let request = BGContinuedProcessingTaskRequest(identifier: Self.identifier(key: key), title: Self.title, subtitle: subtitle)
        request.strategy = .fail   // now or not at all: the work is already running
        do {
            try BGTaskScheduler.shared.submit(request)
            wanted = true
        } catch {
            NSLog("[sve] continued processing not available: %@", String(describing: error))
        }
    }

    /// Progress 0…1 and the stage line, shown by the system.
    func update(fraction: Double? = nil, subtitle: String? = nil) {
        if let fraction { self.fraction = max(self.fraction, min(1, max(0, fraction))) }
        if let subtitle, !subtitle.isEmpty { self.subtitle = subtitle }
        apply()
    }

    /// A new note starts in the same task: progress starts again from 0.
    func startNote() {
        fraction = 0
        apply()
    }

    /// All work is done (or paused until the app is opened). Completes the task exactly once.
    func finish(success: Bool) {
        wanted = false
        complete(success: success)
    }

    private func started(_ t: BGTask) {
        guard #available(iOS 26, *), let t = t as? BGContinuedProcessingTask else {
            t.setTaskCompleted(success: false)
            return
        }
        // Finished before the system started the task: nothing left to do.
        guard wanted, held == nil else { t.setTaskCompleted(success: true); return }
        let h = Held(t)
        held = h
        t.progress.totalUnitCount = 1000
        t.expirationHandler = {
            // Completed right here, on whatever thread this runs: the app may be suspended before a hop.
            h.complete(success: false)
            Task { @MainActor in ContinuedWork.shared.expired(h) }
        }
        apply()
    }

    private func expired(_ h: Held) {
        guard held === h else { return }
        NSLog("[sve] continued processing expired")
        held = nil
        wanted = false
        onExpire?()
    }

    private func apply() {
        guard #available(iOS 26, *), let t = held?.task as? BGContinuedProcessingTask else { return }
        t.progress.completedUnitCount = Int64(fraction * 1000)
        t.updateTitle(Self.title, subtitle: subtitle)
    }

    private func complete(success: Bool) {
        guard let h = held else { return }
        held = nil
        if #available(iOS 26, *), success { (h.task as? BGContinuedProcessingTask)?.progress.completedUnitCount = 1000 }
        h.complete(success: success)
    }
}
