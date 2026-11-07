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
    static func identifier(noteID: Int64) -> String { "com.summaverick.everyday.notes.\(noteID)" }

    /// Called when the system ends the task early: cancel the run, mark the note interrupted.
    var onExpire: (() -> Void)?

    private var task: AnyObject?          // BGContinuedProcessingTask
    private var wanted = false            // submitted (or running) and work is not done yet
    private var fraction: Double = 0
    private var subtitle = ""

    private init() {}

    /// The app may keep working in the background now.
    var isHeld: Bool { task != nil }

    static var title: String { String(localized: "Writing your notes") }

    /// At launch, before the first submit. Registering twice would end the app, so only here.
    static func register() {
        guard #available(iOS 26, *) else { return }
        let ok = BGTaskScheduler.shared.register(forTaskWithIdentifier: wildcard, using: .main) { task in
            MainActor.assumeIsolated { ContinuedWork.shared.started(task) }
        }
        if !ok { NSLog("[sve] continued processing not registered (Info.plist identifiers?)") }
    }

    /// Processing starts after Stop. Submits once; a task already held or asked for is kept.
    func begin(noteID: Int64, subtitle: String) {
        guard #available(iOS 26, *) else { return }
        self.subtitle = subtitle
        guard !wanted else { return }
        fraction = 0
        let request = BGContinuedProcessingTaskRequest(identifier: Self.identifier(noteID: noteID), title: Self.title, subtitle: subtitle)
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
        guard wanted, task == nil else { t.setTaskCompleted(success: true); return }
        task = t
        t.progress.totalUnitCount = 1000
        t.expirationHandler = {
            Task { @MainActor in ContinuedWork.shared.expired() }
        }
        apply()
    }

    private func expired() {
        guard task != nil else { return }
        NSLog("[sve] continued processing expired")
        onExpire?()
        wanted = false
        complete(success: false)
    }

    private func apply() {
        guard #available(iOS 26, *), let t = task as? BGContinuedProcessingTask else { return }
        t.progress.completedUnitCount = Int64(fraction * 1000)
        t.updateTitle(Self.title, subtitle: subtitle)
    }

    private func complete(success: Bool) {
        guard let t = task as? BGTask else { return }
        task = nil
        if #available(iOS 26, *) { (t as? BGContinuedProcessingTask)?.progress.completedUnitCount = success ? 1000 : Int64(fraction * 1000) }
        t.setTaskCompleted(success: success)
    }
}
