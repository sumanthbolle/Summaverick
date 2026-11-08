// Overnight mode: a BGProcessingTask that finishes long recordings while the phone charges. iOS runs it
// when the phone is plugged in and idle (usually at night); it can end it at any time. Work runs on the CPU
// (the app is in the background), one note at a time, through the pipeline, with the heat rule: first the marked,
// cut-off and waiting notes, then the memories of notes that still need them, newest first (up to 20 notes a
// run). Every piece, section, output and section's memories is kept as it finishes, so an expired task loses at
// most the step in flight.
// Not the same as ContinuedWork (iOS 26 continued processing right after Stop, while the person waits):
// that one has its own identifiers; the pipeline's busy claim keeps the two from ever running model work at
// the same time.
import BackgroundTasks
import Foundation

@MainActor
final class OvernightWork {
    static let shared = OvernightWork()
    /// Matches Info.plist BGTaskSchedulerPermittedIdentifiers. Fixed, so it is registered at launch.
    nonisolated static let identifier = "com.summaverick.everyday.overnight"

    /// One run of the task. setTaskCompleted exactly once, from whichever side gets there first: the end
    /// of the work (main actor) or the expiration handler (any thread).
    private final class Run: @unchecked Sendable {
        let task: BGTask
        /// The system ended the task early.
        let expired = AtomicFlag()
        private let completed = AtomicFlag()
        init(_ task: BGTask) { self.task = task }
        func complete(success: Bool) {
            guard completed.setOnce() else { return }
            task.setTaskCompleted(success: success)
        }
    }
    private var run: Run?
    private static var registered = false

    private init() {}

    /// At launch, before it finishes (App init): the handler for the fixed identifier. Once only (a second
    /// registration ends the app).
    static func register() {
        guard !registered else { return }
        registered = true
        let ok = BGTaskScheduler.shared.register(forTaskWithIdentifier: identifier, using: .main) { task in
            MainActor.assumeIsolated { OvernightWork.shared.started(task) }
        }
        if !ok { NSLog("[sve] overnight task handler not registered") }
    }

    /// Asks iOS to run the task once the phone is on the charger (no network needed). Asking again replaces
    /// the request that is waiting.
    func schedule() { Self.submit() }

    /// Any thread (also from the expiration handler).
    nonisolated private static func submit() {
        let request = BGProcessingTaskRequest(identifier: identifier)
        request.requiresExternalPower = true
        request.requiresNetworkConnectivity = false
        do {
            try BGTaskScheduler.shared.submit(request)
            NSLog("[sve] overnight task scheduled")
        } catch {
            NSLog("[sve] overnight task not scheduled: %@", String(describing: error))
        }
    }

    private func started(_ task: BGTask) {
        guard task is BGProcessingTask else { task.setTaskCompleted(success: false); return }
        guard run == nil else {
            // One is already running: this launch ends, and the next is asked for so none is lost.
            Self.submit()
            task.setTaskCompleted(success: true)
            return
        }
        NSLog("[sve] overnight task started")
        let r = Run(task)
        run = r
        let pipeline = ProcessingPipeline.shared
        task.expirationHandler = {
            // Right here, on whatever thread this runs: the app may be suspended before a hop to main.
            // Cancel the run in flight (its finished work is kept), ask again, complete.
            guard r.expired.setOnce() else { return }
            NSLog("[sve] overnight task expired")
            pipeline.cancelForExpiry()
            OvernightWork.submit()
            r.complete(success: false)
        }
        Task { @MainActor in
            let workLeft = await pipeline.runOvernight(expired: r.expired)
            if !r.expired.isSet {
                if workLeft { Self.submit() }   // heat, memories left, or more than one night's work: again on the charger
                NSLog("[sve] overnight task done (work left: %@)", workLeft ? "yes" : "no")
                r.complete(success: !workLeft)
            }
            if self.run === r { self.run = nil }
        }
    }
}
