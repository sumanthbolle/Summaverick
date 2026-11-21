// Recording → transcript → title → three outputs. Everything after recording is local.
// Transcription runs on the CPU, mostly while recording (LiveTranscriber); after Stop it continues from
// the saved cursor (TranscriptSidecar). Notes use the GPU while the app is in front, else the CPU.
// On iOS 26+ a continued-processing task keeps the work going with the phone locked. If iOS suspends the
// app mid-way, or the phone gets too hot, the note keeps whatever was finished and is resumed
// automatically later; the user can also tap "Write my notes". Cut off with the app not in front, the note goes on
// the overnight list by itself and finishes in the overnight task (on battery too), with a notification when its
// notes are ready (NotesNotifier); opening the app resumes it at once (it stays listed until it is done).
// Recordings made before the AI has downloaded wait and are processed, one at a time, once it is ready. A long
// recording can instead be finished overnight while the phone charges (OvernightWork). After the notes, memories are written one section
// at a time; they never come before the notes, never hold them up and never fail them. Older notes get theirs
// when the app is idle in front, or overnight. Same stages the device firmware will run.
import AVFoundation
import Foundation
import Observation
import UIKit
import UserNotifications

@MainActor
@Observable
final class ProcessingPipeline {
    static let shared = ProcessingPipeline()

    enum Stage: Equatable {
        case idle
        case transcribing(Double)
        case loadingModel
        case extracting
        /// Section summaries: done so far, total to write now.
        case sections(Int, Int)
        case generating(OutputKind, String, Int, Int)   // kind, phase, done, total
        case done
        case failed(String)
        /// Stopped for now (the phone is too hot); resumes by itself.
        case paused(String)
        /// The phone is warm (serious): resting before the next piece or stage, then going on.
        case cooling
        /// The person chose to finish this note overnight: stopped, waiting for the charger.
        case overnight
        /// Memories, after the notes: sections done so far, total to do now.
        case memories(Int, Int)

        var label: String {
            switch self {
            case .idle: return ""
            case .transcribing(let p): return String(localized: "Listening back… \(Int(p * 100))%")
            case .loadingModel: return String(localized: "Working on it…")
            case .extracting: return String(localized: "Looking for reminders and tasks…")
            case .sections(let d, let t):
                return String(localized: "Going through part \(min(d + 1, max(t, 1))) of \(max(t, 1))…",
                              comment: "Part number, total parts of a long recording")
            case .generating(let k, let phase, let d, let t):
                if phase == "part" && t > 1 {   // notes in small saved steps: the part being written
                    return String(localized: "Writing your \(k.title.lowercased())…") + " · "
                        + String(localized: "Part \(d + 1) of \(t)", comment: "Part number, total parts of a recording")
                }
                return phase == "chunk" && t > 1 ? String(localized: "Reading part \(d + 1) of \(t) for your \(k.title.lowercased())…") : String(localized: "Writing your \(k.title.lowercased())…")
            case .done: return String(localized: "Done")
            case .failed(let e): return String(localized: "I couldn't finish: \(e)")
            case .paused(let m): return m
            case .cooling: return String(localized: "Going slower to keep the phone cool")
            case .overnight: return ProcessingPipeline.overnightMessage
            case .memories(let d, let t):
                return String(localized: "Finding things to remember… part \(min(d + 1, max(t, 1))) of \(max(t, 1))",
                              comment: "Part number, total parts of a recording whose memories are being written")
            }
        }
        /// True while the model is working (not idle, done or failed).
        var isWorking: Bool {
            switch self { case .idle, .done, .failed, .paused, .overnight: return false; default: return true }
        }
        /// The same stage for the recording's Live Activity (LiveActivityLogic decides what it shows).
        var activityStage: LiveActivityLogic.Stage {
            switch self {
            case .idle: return .idle
            case .transcribing(let p): return .transcribing(p)
            case .loadingModel: return .loadingModel
            case .extracting: return .extracting
            case .sections(let d, let t): return .sections(d, t)
            case .generating(_, _, let d, let t): return .generating(d, t)
            case .done: return .done
            case .failed: return .failed
            case .paused: return .paused
            case .cooling: return .cooling
            case .overnight: return .overnight
            case .memories(let d, let t): return .memories(d, t)
            }
        }
    }

    private(set) var stage: Stage = .idle {
        didSet {
            speedTick()
            refreshExpiryOutcome()
            if stage.isWorking { ContinuedWork.shared.update(subtitle: progressLine) }
            guard case .failed(let msg) = stage, let id = activeNoteID else { followStage(); return }
            // Only stopShort sets .failed, and only for a real failure (FailureKind): an interruption is a cut-off
            // there and never reaches this.
            lastFailure[id] = msg
            failedLines[id] = msg
            notifyFailed(id, line: msg)
            RecordingActivities.shared.update(noteID: id, .failed, line: msg, percent: ContinuedWork.shared.percent)
        }
    }

    // MARK: speed record (SpeedRecord): active work time per note, heat and battery, for the owner's own figures.

    @ObservationIgnored private var speedOpen: (id: Int64, bucket: SpeedMath.Bucket, since: Date)?
    @ObservationIgnored private var speedChain: Task<Void, Never>?

    private func speedBucket(_ s: Stage) -> SpeedMath.Bucket {
        switch s {
        case .transcribing: return .transcribe
        case .cooling: return notesStored ? .none : .rest   // a rest after the notes are stored delays nothing
        case .loadingModel: return notesStored ? .none : .load
        case .sections, .generating: return notesStored ? .after : .notes
        case .extracting: return .after
        default: return .none
        }
    }

    private static let deviceModel: String = {
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }()

    /// Each stage change: the time since the last one goes to the note it was for; the new stage starts its own.
    /// Only a note's own notes run is timed (a regenerate or memories are not). Writes go one after another.
    private func speedTick() {
        let now = Date()
        let closing = speedOpen.map { (id: $0.id, bucket: $0.bucket, seconds: now.timeIntervalSince($0.since)) }
        let bucket = notesRun ? speedBucket(stage) : .none
        let id = activeNoteID
        speedOpen = bucket != .none ? id.map { (id: $0, bucket: bucket, since: now) } : nil
        let finished = stage == .done ? id : nil
        let notesReady = notesStored ? id : nil
        let touched = Set([closing?.id, bucket != .none ? id : nil, finished, notesReady].compactMap { $0 })
        guard !touched.isEmpty else { return }
        let thermal = ProcessInfo.processInfo.thermalState.rawValue
        let d = UIDevice.current
        let battery: Double? = d.isBatteryMonitoringEnabled && d.batteryLevel >= 0 ? Double(d.batteryLevel) : nil
        let charging = d.isBatteryMonitoringEnabled && (d.batteryState == .charging || d.batteryState == .full)
        let previous = speedChain
        speedChain = Task {
            await previous?.value
            let engine = CoreEngine.shared
            for n in touched {
                var r: SpeedRecord
                if let json = await engine.setting(SpeedRecord.key(n)), let data = json.data(using: .utf8),
                   let old = try? JSONDecoder().decode(SpeedRecord.self, from: data) {
                    r = old
                } else {
                    guard let note = await engine.note(n) else { continue }
                    r = SpeedRecord(audioSeconds: Double(note.duration_seconds), device: Self.deviceModel)
                    r.batteryStart = battery
                }
                if let c = closing, c.id == n { r = SpeedMath.adding(r, c.bucket, seconds: c.seconds) }
                r.peakThermal = max(r.peakThermal, thermal)
                if let battery { r.batteryEnd = battery; if r.batteryStart == nil { r.batteryStart = battery } }
                r.charging = r.charging || charging
                if finished == n { r.finishedAt = now.timeIntervalSince1970 }
                if notesReady == n, r.notesReadyAfter == nil {
                    r.notesReadyAfter = r.transcribeSeconds + r.loadSeconds + r.notesSeconds + r.restSeconds
                }
                guard let data = try? JSONEncoder().encode(r), let json = String(data: data, encoding: .utf8) else { continue }
                await engine.setSetting(SpeedRecord.key(n), json)
            }
        }
    }

    /// The note being worked on has a recording Live Activity: it shows this stage (failures are handled above).
    private func followStage() {
        guard let id = activeNoteID, RecordingActivities.shared.isFollowing(noteID: id) else { return }
        if case .idle = stage, steppedAside.contains(id), interrupted.contains(id) {
            // Stepped aside for a recording (LiveFirst): it waits its turn, and is not paused or cut off.
            RecordingActivities.shared.update(noteID: id, .queued, line: Self.recordingFirstMessage, percent: ContinuedWork.shared.percent)
            return
        }
        let line: String
        switch stage {
        case .idle: line = lastFailure[id] ?? Self.waitingMessage
        case .done, .memories: line = noteTitles[id] ?? ""
        default: line = progressLine
        }
        RecordingActivities.shared.update(noteID: id, stage.activityStage, line: line, percent: ContinuedWork.shared.percent)
    }

    /// A run of this note ended: an activity still showing its work ends now, unless the note waits its turn (queued,
    /// shown as such). Ready and failed have already ended it; a pause keeps it, showing the pause.
    private func activityRunEnded(_ id: Int64) {
        guard RecordingActivities.shared.isFollowing(noteID: id), !interrupted.contains(id) else { return }
        RecordingActivities.shared.update(noteID: id, .idle, line: lastFailure[id] ?? "", percent: ContinuedWork.shared.percent)
    }
    /// Notes whose processing was cut off (app in the background, phone too hot) or that waited for
    /// the AI to download; resumed one at a time while the app is active.
    private(set) var interrupted: Set<Int64> = [] { didSet { refreshExpiryOutcome() } }
    private var wentToBackground = false
    /// The last Stop came with the app not in front (Live Activity, shortcut, control).
    @ObservationIgnored private var stoppedWhileNotActive = false
    @ObservationIgnored private var backgroundObserver: NSObjectProtocol?
    @ObservationIgnored private var thermalObserver: NSObjectProtocol?
    @ObservationIgnored private var resuming = false
    /// Model work only starts while the app is in front. Read directly: a flag set from the scene
    /// phase can be late at cold launch.
    private var appIsActive: Bool { UIApplication.shared.applicationState == .active }
    /// Claimed synchronously by regenerate / findReminders before their first await, so two runs
    /// can never start at once.
    private(set) var starting = false { didSet { refreshExpiryOutcome() } }
    /// Notes the "AI is ready" pass already ran once, so a silent recording is not re-run every launch.
    @ObservationIgnored private var autoTried: Set<Int64> = ProcessingPipeline.killRestore.tried
    @ObservationIgnored private var autoTriedPruned = false
    private(set) var activeNoteID: Int64? {
        // Work on a note goes on: a pause or failure notice of it in the list is no longer true.
        didSet { if let id = activeNoteID, id != oldValue { NotesNotifier.withdrawStale(noteID: id) } }
    }
    /// Last failure message per note, so the note screen can show why nothing was generated.
    private(set) var lastFailure: [Int64: String] = [:]
    /// The line a real failure (FailureKind .failed) wrote into lastFailure: the note screen says "couldn't finish"
    /// only while lastFailure still holds it (PauseWords.banner); any other line there is a pause.
    private(set) var failedLines: [Int64: String] = [:]
    /// "1 of 3 ready" for the notes run in flight (nil for any other run).
    private(set) var stepCount: StepCount?
    /// While writing down what was said: seconds done and the recording's length, for "14 of 56 parts".
    private(set) var transcribing: (done: Double, duration: Double)?

    /// What an expiry would leave, for the continued-processing task's expiration handler (it runs on any thread and
    /// cannot ask): paused while notes are left, done when only memories or the insight run.
    private func refreshExpiryOutcome() {
        let c = TaskLifecycle.expiryCompletion(working: isWorking, memoriesOnly: memoryWork, notesStored: notesStored,
                                               otherWorkWaiting: !interrupted.isEmpty || !queued.isEmpty || regenerating != nil)
        ContinuedWork.shared.workLeftOnExpiry.set(c == .paused)
    }

    /// The progress line of the run in flight (note screen, Live Activity, lock screen): the stage, with "14 of 56
    /// parts" while writing down what was said and "1 of 3 ready" once a part of a notes run is stored.
    var progressLine: String {
        var label = stage.label
        if case .transcribing = stage, let t = transcribing,
           let line = StepCount.transcribingLine(transcribedSeconds: t.done, durationSeconds: t.duration) { label = line }
        guard notesRun, let steps = stepCount else { return label }
        return steps.workingLine(label)
    }
    /// Any model work, memories included: nothing else that runs the model may start.
    var isWorking: Bool { starting || stage.isWorking }
    /// What the person's controls (Ask, Regenerate, Try again, Generate notes) treat as busy: memory work never is,
    /// it stops for them (yieldMemories).
    var isBusy: Bool { PersonFirst.controlsBusy(working: isWorking, memoriesOnly: memoryWork) }

    private var bgTask: UIBackgroundTaskIdentifier = .invalid
    /// Set when the continued-processing task expires: cancels the model run in flight.
    @ObservationIgnored private let abort = AtomicFlag()

    /// A note's work cut off with the phone locked: it finishes by itself later (the overnight task) or when opened.
    static var cutOffMessage: String { String(localized: "Your notes are safe. I'll carry on by myself soon.") }
    static var heatMessage: String { String(localized: "I stopped so the phone can cool down. I'll carry on by myself.") }
    static var waitingMessage: String { String(localized: "Waiting for the download to finish, then I'll start") }
    /// Stopped while another recording's notes are being written.
    static var queuedMessage: String { String(localized: "I'll get to this after the earlier recording") }
    /// Stepped aside while a recording runs (LiveFirst): it carries on after that recording.
    static var recordingFirstMessage: String { String(localized: "Waiting for you to finish recording") }
    nonisolated static var overnightMessage: String { String(localized: "Waiting for the charger. I'll finish overnight.") }
    nonisolated private static var thermal: ProcessInfo.ThermalState { ProcessInfo.processInfo.thermalState }
    /// Critical: the only heat that cancels a model run in the middle.
    nonisolated private static var critical: Bool { Heat.cancelsRun(thermal) }
    /// Notes paused for heat (the ladder at paused); they carry on once it is below paused, after a minute of calm.
    @ObservationIgnored private var heatPaused: Set<Int64> = []
    /// The run in flight was paused by the heat gate (the ladder at paused; reset when a run starts).
    @ObservationIgnored private var runPausedForHeat = false
    /// True while the only work is memories (after a note's notes, or a backfill): it yields to the person.
    private(set) var memoryWork = false { didSet { refreshExpiryOutcome() } }
    /// Set by the person's action (any thread reads it): the memory section in flight is cancelled, the run stops.
    @ObservationIgnored private let memoryStop = AtomicFlag()
    /// Notes whose memory model call failed (-3) in this launch: backfills (in front and overnight) skip them, so one
    /// such note never holds up the others. Tried again at the next launch.
    @ObservationIgnored private var memoryFailed: Set<Int64> = []
    /// Taps made while another note's work ran (Regenerate, Generate notes, Try again, Find reminders), oldest first:
    /// each runs by itself in its turn, from the resume loop, before cut-off notes. The note shows the waiting line.
    private(set) var queued: [QueuedTap] = ProcessingPipeline.loadQueuedTaps() { didSet { saveQueuedTaps(); refreshExpiryOutcome() } }
    /// The tap running now (a regenerate or find reminders, tapped or from the queue), kept with the queue on disk:
    /// the app killed meanwhile runs it again next launch.
    @ObservationIgnored private var runningTap: QueuedTap? { didSet { saveQueuedTaps() } }
    /// Set by Settings while "Delete everything" or "Remove AI" runs: no memory backfill starts meanwhile.
    var memoriesHeld = false
    /// Set when the app stops being active (read on any thread): cancels a memory run on the GPU before iOS refuses
    /// GPU work in the background, where a failed reply would mark the section done with no memories.
    @ObservationIgnored private let leftFront = AtomicFlag()
    /// The app went to the background during a Whisper group (read on any thread): the group is cut short and Gemma
    /// goes on from the cursor on the CPU. Cleared before each group while the app is not in the background.
    @ObservationIgnored private let whisperLeftFront = AtomicFlag()
    @ObservationIgnored private var resignObserver: NSObjectProtocol?
    @ObservationIgnored private var activeObserver: NSObjectProtocol?
    /// Some note may still have sections without memories (unknown at launch): the overnight task is asked for.
    @ObservationIgnored private var memoriesMayBeDue = true

    // MARK: overnight

    private static let overnightKey = "pipeline.overnight"
    /// Notes the person chose to finish overnight while charging. They start only while plugged in or inside
    /// the overnight task (OvernightWork), unless "Do it now". Kept across launches.
    private(set) var overnight: [Int64] = (UserDefaults.standard.array(forKey: ProcessingPipeline.overnightKey) as? [Int64]) ?? []
    nonisolated private static let automaticKey = "pipeline.overnight.auto"
    /// Notes on the overnight list by themselves after a cut-off (not by the person): the task may then run on
    /// battery, and they resume whenever the resume loop runs (opening the app resumes them at once). They stay
    /// listed until their notes are done, so a kill or another cut-off never loses them. Kept across launches.
    @ObservationIgnored private var automatic: Set<Int64> = Set((UserDefaults.standard.array(forKey: ProcessingPipeline.automaticKey) as? [Int64]) ?? [])
    /// Any thread (OvernightWork's request): a note cut off after Stop is waiting to finish by itself, or a person's
    /// tap waits its turn (it was made to run now, so it may run on battery too; not one cut off 3 times in a row,
    /// which waits for the charger or the app in front: TapQueue.mayRun).
    nonisolated static var automaticFinishPending: Bool {
        !((UserDefaults.standard.array(forKey: automaticKey) as? [Int64]) ?? []).isEmpty
            || loadQueuedTaps().contains { TapQueue.mayRun($0, appActive: false, pluggedIn: false) }
    }

    /// The note shows this while a tap on it waits its turn.
    static var tapWaitingMessage: String { String(localized: "One moment, I'm writing another note") }
    func hasQueuedTap(_ id: Int64) -> Bool { queued.contains { $0.noteID == id } }
    /// Find reminders cut off because the phone was too warm: it waits for the phone to cool.
    static var tapTooWarmMessage: String { String(localized: "The phone's too warm right now. I'll carry on when it cools.") }
    /// Notes whose queued find reminders waits for the phone to cool (cleared when it runs again).
    private(set) var tapTooWarm: Set<Int64> = []
    /// The line a note with a queued tap shows: waiting for the AI (the tap runs once it is ready), for the phone to
    /// cool, or for another note's run.
    func tapWaitingLine(_ id: Int64) -> String {
        if !ModelManager.shared.isReady { return Self.waitingMessage }
        return tapTooWarm.contains(id) ? Self.tapTooWarmMessage : Self.tapWaitingMessage
    }

    nonisolated private static func loadQueuedTaps() -> [QueuedTap] {
        guard let data = UserDefaults.standard.data(forKey: TapQueue.key) else { return [] }
        return (try? JSONDecoder().decode([QueuedTap].self, from: data)) ?? []
    }
    /// The queue and the regenerate running (first: it was tapped before any still queued). No key when empty.
    private func saveQueuedTaps() {
        let all = queued.reduce(runningTap.map { [$0] } ?? []) { TapQueue.enqueue($0, $1) }
        if all.isEmpty { UserDefaults.standard.removeObject(forKey: TapQueue.key); return }
        UserDefaults.standard.set(try? JSONEncoder().encode(all), forKey: TapQueue.key)
    }

    /// A tap that could not start (another run works): queued for its turn, unless that run is this note's own notes
    /// and covers it.
    private func enqueueTap(_ tap: QueuedTap) {
        guard TapQueue.accepts(tap, notesRunningFor: notesRun ? activeNoteID : nil) else {
            NSLog("[sve] bg: tap on note %lld covered by its notes run", tap.noteID)
            return
        }
        queued = TapQueue.enqueue(queued, tap)
        NSLog("[sve] bg: tap queued (note %lld, %@)", tap.noteID, String(describing: tap.kind))
    }
    /// The run in flight writes a note's notes (process, or resuming them; not a regenerate or a backfill): only
    /// such a run is enrolled after a cut-off and notifies ready, paused and failed.
    @ObservationIgnored private var notesRun = false
    /// The run in flight has stored its note's notes (only the memories and the insight are left).
    @ObservationIgnored private var notesStored = false { didSet { refreshExpiryOutcome() } }
    /// Notes whose "paused" notification was posted for the cut-off of their current run (posted once).
    @ObservationIgnored private var pausedPosted: Set<Int64> = []
    /// The parts (NotifyRule.transcript, .summary, .notes) whose "ready" this note's current run already posted.
    @ObservationIgnored private var postedStages: [Int64: Set<String>] = [:]
    nonisolated private static let failureStreakKey = "pipeline.failureStreak"
    /// Unknown errors in a row per note and run kind (FailureKind.streakKey): the first is a cut-off, the second a
    /// failure. Reset on that kind's success (a part stored, a regenerate or find reminders done). Kept across launches.
    @ObservationIgnored private var failureStreak: [String: Int] =
        (UserDefaults.standard.dictionary(forKey: ProcessingPipeline.failureStreakKey) as? [String: Int]) ?? [:] {
        didSet {
            guard failureStreak != oldValue else { return }
            if failureStreak.isEmpty { UserDefaults.standard.removeObject(forKey: Self.failureStreakKey) }
            else { UserDefaults.standard.set(failureStreak, forKey: Self.failureStreakKey) }
        }
    }
    private func streak(_ id: Int64, _ run: FailureKind.Run) -> Int { failureStreak[FailureKind.streakKey(noteID: id, run: run)] ?? 0 }
    private func setStreak(_ id: Int64, _ run: FailureKind.Run, _ n: Int?) {
        failureStreak[FailureKind.streakKey(noteID: id, run: run)] = n
    }
    /// Titles of the notes this launch worked on or queued, so a notification never waits for the core (busy until
    /// the cancelled model call stops) before it takes its background time.
    @ObservationIgnored private var noteTitles: [Int64: String] = [:]
    /// The model step running (or that just failed) is on the GPU: its failure in the background is a cut-off.
    @ObservationIgnored private var stepOnGPU = false
    /// Since when the app has been in front without going to the background (nil: in the background), for
    /// StepBackend's debounce. Kept by the active and background observers.
    @ObservationIgnored private var frontSince: Date?
    @ObservationIgnored private var enterBackgroundObserver: NSObjectProtocol?
    /// After Stop: the note whose work left is long enough to offer finishing it overnight.
    private(set) var overnightOffer: Int64?
    /// "Yes" tapped while the note is being written: it stops before its next piece or stage.
    @ObservationIgnored private var parkRequested: Set<Int64> = []
    /// Notes that stopped for the night in this run (set by the heat gate, read once the run ends).
    @ObservationIgnored private var parked: Set<Int64> = []
    nonisolated private static let steppedAsideKey = "pipeline.steppedAside"
    /// Notes whose run stepped aside for a recording, and notes only waiting their turn behind it (LiveFirst): back in
    /// `interrupted`, quiet while it records (waitsQuietly). Cleared when the note's notes run starts again. Kept across
    /// launches, so a kill during the recording still resumes them (queueWaitingNotes), with no overnight mark.
    /// A notes run saved as running at launch was killed: its note joins them (restoredSteppedAside).
    @ObservationIgnored private var steppedAside: Set<Int64> = ProcessingPipeline.killRestore.steppedAside {
        didSet { if steppedAside != oldValue { Self.save(steppedAside, Self.steppedAsideKey) } }
    }
    nonisolated private static let runningNoteKey = "pipeline.runningNote"
    /// The notes run in flight, on disk: a kill after the transcript is stored leaves nothing else saying the note was
    /// running (queueWaitingNotes only finds notes with no transcript). Not a regenerate or find reminders: runningTap.
    @ObservationIgnored private var runningNote: Int64? {
        didSet {
            if let runningNote { UserDefaults.standard.set(NSNumber(value: runningNote), forKey: Self.runningNoteKey) }
            else { UserDefaults.standard.removeObject(forKey: Self.runningNoteKey) }
        }
    }
    nonisolated private static let launchResumesKey = "pipeline.launchResumes"
    /// Launches that resumed a killed run, per note (WaitingNotes.launchResumeLimit). Reset when a run ends cleanly.
    @ObservationIgnored private var launchResumes: [String: Int] = ProcessingPipeline.killRestore.resumes {
        didSet { if launchResumes != oldValue { Self.saveLaunchResumes(launchResumes) } }
    }
    nonisolated private static func saveLaunchResumes(_ r: [String: Int]) {
        if r.isEmpty { UserDefaults.standard.removeObject(forKey: launchResumesKey) } else { UserDefaults.standard.set(r, forKey: launchResumesKey) }
    }
    /// Read once at launch, before a new run can overwrite the key, and shared by the properties it sets up: their
    /// initial values cannot see each other. A killed run's note moves into the stepped-aside set: restored by
    /// queueWaitingNotes, kept through another kill until its run starts again (startNotesRun takes it out, so it never
    /// runs twice), quiet behind a recording. Past the limit it stays out and counts as tried; "Write my notes" still runs it.
    nonisolated private static let killRestore: WaitingNotes.AfterKill = {
        let d = UserDefaults.standard
        let stored = Set((d.array(forKey: steppedAsideKey) as? [Int64]) ?? [])
        let tried = Set((d.array(forKey: "pipeline.autoTried") as? [Int64]) ?? [])
        let resumes = (d.dictionary(forKey: launchResumesKey) as? [String: Int]) ?? [:]
        let r = WaitingNotes.afterKill(steppedAside: stored, tried: tried, resumes: resumes,
                                       running: (d.object(forKey: runningNoteKey) as? NSNumber)?.int64Value)
        if r.steppedAside != stored { save(r.steppedAside, steppedAsideKey) }
        if r.tried != tried { d.set(r.tried.sorted(), forKey: "pipeline.autoTried") }
        if r.resumes != resumes { saveLaunchResumes(r.resumes) }
        d.removeObject(forKey: runningNoteKey)
        if let id = r.capped {
            NSLog("[sve] bg: note %lld not resumed again after %d launches", id, Int32(WaitingNotes.launchResumeLimit))
        }
        return r
    }()
    /// The saved stepped-aside notes were queued again in this launch (queueWaitingNotes).
    @ObservationIgnored private var steppedAsideRestored = false
    nonisolated private static let insightDueKey = "pipeline.insightDue"
    /// Notes whose insight stepped aside for a recording (LiveFirst.insightStaysDue): written after Stop, from the
    /// resume loop. Kept across launches.
    @ObservationIgnored private var insightDue: Set<Int64> = Set((UserDefaults.standard.array(forKey: ProcessingPipeline.insightDueKey) as? [Int64]) ?? []) {
        didSet { if insightDue != oldValue { Self.save(insightDue, Self.insightDueKey) } }
    }
    nonisolated private static func save(_ ids: Set<Int64>, _ key: String) {
        if ids.isEmpty { UserDefaults.standard.removeObject(forKey: key) } else { UserDefaults.standard.set(ids.sorted(), forKey: key) }
    }
    /// The run in flight stepped aside for a recording (reset when a run starts).
    @ObservationIgnored private var runSteppedAside = false
    /// The overnight task's expired flag while it runs. Set: no new run starts, and the abort stays set.
    @ObservationIgnored private var overnightRun: AtomicFlag?
    /// Set while the run in flight was started by the overnight task (read on any thread): only that run is
    /// cancelled when the task expires; a run in front or kept by a continued-processing task is not.
    @ObservationIgnored private let overnightOwnsRun = AtomicFlag()
    /// Plugged in (charging or full). Battery monitoring is switched on as needed (live transcription
    /// switches it off when a recording ends).
    static var pluggedIn: Bool {
        let d = UIDevice.current
        let was = d.isBatteryMonitoringEnabled
        if !was { d.isBatteryMonitoringEnabled = true }
        let state = d.batteryState
        if !was { d.isBatteryMonitoringEnabled = false }   // as it was (live transcription manages its own)
        return state == .charging || state == .full
    }

    private init() {
        ContinuedWork.shared.onExpire = { [weak self] hold in
            guard let self else { hold.end(); return }
            self.continuedWorkExpired(hold: hold)
        }
        activeObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                if self?.frontSince == nil { self?.frontSince = Date() }
                self?.appOpened()
            }
        }
        if UIApplication.shared.applicationState == .active { frontSince = Date() }
        enterBackgroundObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.whisperLeftFront.set(true)   // Whisper's GPU never runs in the background
                self?.frontSince = nil
            }
        }
        let leftFront = self.leftFront
        resignObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in leftFront.set(true) }
        // Cooled down (the heat ladder came down from paused): carry on with the notes that were paused for heat.
        thermalObserver = NotificationCenter.default.addObserver(
            forName: .heatLevelChanged, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, HeatGovernor.shared.level < .paused, self.appIsActive else { return }
                Task { await self.resumeInterrupted() }
            }
        }
        #if DEBUG
        debugRetranscribe()
        debugReplay()
        #endif
    }

    /// The heat ladder at paused (critical heat, or almost no free memory): the note is paused and resumes once the
    /// ladder is below paused (a minute of calm).
    /// beforeSystemEnds: hot in the background (HeatLadder.pausesBeforeSystemEnds), paused by the app before iOS ends
    /// the task as "Failed".
    private func pausedForHeat(_ id: Int64, beforeSystemEnds: Bool = false) -> Bool {
        guard beforeSystemEnds || HeatGovernor.shared.level == .paused else { return false }
        NSLog("[sve] pausing note %lld: heat %@ (thermal state %ld)%@", id, HeatGovernor.shared.level.name,
              ProcessInfo.processInfo.thermalState.rawValue, beforeSystemEnds ? ", in the background, before iOS ends the task" : "")
        interrupted.insert(id)
        heatPaused.insert(id)
        runPausedForHeat = true
        lastFailure[id] = Self.heatMessage
        stage = .paused(Self.heatMessage)
        if notesRun { cutOff(id) }   // in the background: finishes by itself later, "paused" once
        return true
    }

    /// Before each piece, section or output: the heat ladder (HeatGovernor). Eased or minimal: rest 5 or 20 s (cut
    /// short by an abort or a cooler phone), then go on, so progress is always made, at a lower duty cycle; CPU runs
    /// also take fewer threads (CoreEngine). Paused: pause the note (false), quietly; it resumes by itself.
    /// A recording running: the run steps aside (stepsAside, false), also when it starts during the rest.
    /// When the heat gate last let a step through (HeatGovernor.now), to size the next rest at minimal.
    private var heatGatePassed: TimeInterval?

    /// light: before a Shruti piece, the rest is HeatLadder.lightRestSeconds (as long as the piece before, no floor).
    private func heatGate(_ id: Int64, light: Bool = false) async -> Bool {
        if parkedForNight(id) { return false }
        if stepsAside(id) { return false }
        let heat = HeatGovernor.shared
        let level = heat.level
        if level == .paused { return !pausedForHeat(id) }
        if HeatLadder.pausesBeforeSystemEnds(level, appActive: appIsActive, taskHeld: ContinuedWork.shared.isHeld) {
            return !pausedForHeat(id, beforeSystemEnds: true)
        }
        guard HeatLadder.showsCooling(level) else { heatGatePassed = HeatGovernor.now; return true }   // full: no rest
        let start = HeatGovernor.now
        // The step since the last gate let it through: at minimal the rest matches it (HeatLadder.restSeconds).
        let lastWork = heatGatePassed.map { start - $0 } ?? 0
        func rest(_ l: HeatLadder.Level) -> Double {
            light ? HeatLadder.lightRestSeconds(l, lastWork: lastWork) : HeatLadder.restSeconds(l, lastWork: lastWork)
        }
        if light, rest(level) <= 0 { heatGatePassed = HeatGovernor.now; return true }   // the first piece: no rest
        let before = stage
        stage = .cooling
        NSLog("[sve] note %lld: heat %@, resting %.0f s", id, level.name, rest(level))
        func over() -> Bool {
            let l = heat.level
            return l == .paused || HeatGovernor.now - start >= rest(l)
        }
        while !over(), !abort.isSet, !LiveTranscriber.shared.liveRunning {
            // The rest is work going on (up to 3 minutes): without a sign of life the heartbeat stops, the lock-screen
            // progress stands still and iOS ends the task as "Failed".
            ContinuedWork.beat()
            do { try await Task.sleep(nanoseconds: 500_000_000) } catch { break }
        }
        if pausedForHeat(id) { return false }
        if stepsAside(id) { return false }
        stage = before
        heatGatePassed = HeatGovernor.now
        return true
    }

    /// Before each piece, section or output (from the heat gate): a recording runs, so this run steps aside and
    /// releases the model to its live transcription (LiveFirst); the piece, section or output just finished is kept.
    /// A notes run's note goes back to `interrupted`, quietly (no notice, no failure, no overnight mark, no cut-off
    /// Live Activity: waitsQuietly), and carries on after Stop through the resume loop, which never runs while
    /// recording. A regenerate is queued again instead (runRegenerate), with no cut-off counted.
    private func stepsAside(_ id: Int64) -> Bool {
        guard LiveFirst.yields(recording: LiveTranscriber.shared.liveRunning, runIsLive: false) else { return false }
        NSLog("[sve] note %lld steps aside for the recording", id)
        runSteppedAside = true
        // Notes only waiting their turn behind this run (not cut off, not paused for heat) wait for the recording too.
        let behind = interrupted.filter {
            LiveFirst.joinsWait(waitingTurnOnly: lastFailure[$0] == nil && !heatPaused.contains($0), recording: true)
        }
        steppedAside.formUnion(behind)
        guard regenerating == nil else { return true }   // runRegenerate sets the stage and queues it again
        interrupted.insert(id)
        steppedAside.insert(id)
        stage = .idle
        return true
    }

    /// Stepped aside for the recording that is still running: nothing cuts it off meanwhile.
    private func waitsQuietly(_ id: Int64) -> Bool {
        LiveFirst.waitsQuietly(steppedAside: steppedAside.contains(id) && interrupted.contains(id),
                               recording: LiveTranscriber.shared.isRecording)
    }

    private func markAutoTried(_ id: Int64) {
        guard autoTried.insert(id).inserted else { return }
        UserDefaults.standard.set(autoTried.sorted(), forKey: "pipeline.autoTried")
    }

    /// Recordings stopped whose run has not finished setting up (their note may not exist yet): the
    /// waiting-notes pass and the sidecar sweep leave them alone.
    @ObservationIgnored private var handingOver: Set<String> = []

    /// Stop and make notes. Runs synchronously in the tap, before any await: stops live transcription
    /// starting new pieces, claims the pipeline (busy) and the background time, and asks for the
    /// continued-processing task while the app is still in front. The live piece in flight can hold the
    /// engine for up to ~30 s; nothing else can start on this recording meanwhile.
    func recordingStopped(audio: URL, duration: Int64) {
        LiveTranscriber.shared.stop(audioPath: audio.path)
        handingOver.insert(audio.path)
        ReminderScheduler.shared.requestAuthorizationIfUndetermined()   // in the tap: "notes ready" can be told
        stoppedWhileNotActive = !appIsActive   // a Stop from the Live Activity or a shortcut (backgroundTimeEnded)
        var claimed = false
        if ModelManager.shared.isReady {
            // Keyed on the file name: the note id is not known until the engine is free. A task asked for at Record
            // is kept (TaskAtRecord.workLeftAtBegin); from the background it is the only one that can run.
            ContinuedWork.shared.begin(key: audio.deletingPathExtension().lastPathComponent, subtitle: Stage.transcribing(0).label,
                                       stop: true)
            ContinuedWork.shared.recordingEnded()   // before the note starts: the bar's band ends, the note takes the rest
            if !isWorking {
                starting = true
                claimed = true
                activeNoteID = nil   // the note is about to exist; no other note shows as busy
                beginBackground()
                stage = .transcribing(0)
            }
        }
        ContinuedWork.shared.recordingEnded()   // after begin: the recording's task is the note's now (no-op if done above)
        let asksToOpen = TaskAtRecord.postsOpenAppNotice(stoppedInBackground: !appIsActive,
                                                         taskHeld: ContinuedWork.shared.isHeld,
                                                         workLeft: ModelManager.shared.isReady)
        Task { await self.process(audio: audio, duration: duration, claimed: claimed, asksToOpen: asksToOpen) }
    }

    /// Notes told "open the app" right after their Stop (TaskAtRecord.postsOpenAppNotice): the cut-off that follows
    /// when the plain background time ends does not post "paused" over it. Cleared when a run starts in front.
    @ObservationIgnored private var askedToOpen: Set<Int64> = []

    /// Stop came from the background with no background time: the person is told the recording is kept and the
    /// notes are written once the app is opened (NotifyRule.shouldNotify still applies).
    private func askToOpen(_ id: Int64) {
        // Marked only when it was posted (the app may have come to front before the note was saved).
        guard NotesNotifier.recordingSaved(noteID: id, hold: NotesNotifier.Hold()) else { return }
        askedToOpen.insert(id)
        NSLog("[sve] bg: no background time: asked the person to open the app")
    }

    /// The app came to front: "Your recording is saved" has done its job (it goes from Notification Center), and a
    /// later cut-off of those notes says "Taking a short break" again.
    private func appOpened() {
        guard !askedToOpen.isEmpty else { return }
        UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: askedToOpen.map { NotifyRule.identifier(noteID: $0) })
        askedToOpen.removeAll()
    }

    /// Full run for a fresh recording. Returns the note id as soon as it exists. claimed: recordingStopped
    /// already holds the busy claim and the background time for this run.
    @discardableResult
    func process(audio: URL, duration: Int64, claimed: Bool = false, asksToOpen: Bool = false) async -> Int64? {
        var ownsBackground = claimed
        defer {
            handingOver.remove(audio.path)
            if claimed { starting = false }
            if ownsBackground { endBackground() }
        }
        let engine = CoreEngine.shared
        let title = "Recording " + Self.titleFormatter.string(from: Date())
        let noteLang = AppSettings.shared.notesLanguage
        // The recording is always kept, even when the AI cannot run yet.
        let id = await engine.createNote(title: title, lang: noteLang, audioPath: audio.path, transcript: nil, durationSeconds: duration)
        guard id > 0 else {
            // The marker stays on purpose (ruling B7): the next launch tries to save the recording as a note again.
            // The app retries by itself (the next launch saves it), so it is a cut-off, never "failed". Only this run's
            // own claim ends its stage (so nothing stays busy). Another note running keeps its stage and its Live
            // Activity; only this recording's activity ends.
            NSLog("[sve] bg: the note for %@ was not saved; the next launch saves it", audio.lastPathComponent)
            if claimed { stage = .idle }
            RecordingActivities.shared.update(audioPath: audio.path, .cutOff, line: Self.cutOffMessage, percent: 0)
            if !claimed { Task { self.finishContinuedWork() } }   // nothing runs for it: the task asked at Record ends
            return nil
        }
        UnfinishedRecordings.remove(audio.path)   // the note has it now
        RecordingActivities.shared.attach(audioPath: audio.path, noteID: id)
        defer { activityRunEnded(id) }
        if claimed {
            activeNoteID = id
            // The notes run starts now, before the live piece in flight is handed over (up to ~30 s on the CPU):
            // a cut-off from here on enrols the note and posts "paused" (backgroundTimeEnded).
            startNotesRun(id)
        }
        defer { if claimed { notesRun = false } }
        if asksToOpen { askToOpen(id) }
        noteTitles[id] = title
        lastFailure[id] = nil
        memoriesMayBeDue = true
        NotificationCenter.default.post(name: .notesChanged, object: nil)

        // Live transcription finishes the piece in flight and hands over; this run continues from its cursor.
        await LiveTranscriber.shared.finish(audioPath: audio.path)
        // Not downloaded yet: the note waits and is processed when the AI is ready (processWaitingNotes).
        guard ModelManager.shared.isReady else {
            RecordingActivities.shared.update(noteID: id, .waitingForAI, line: Self.waitingMessage, percent: 0)
            if !claimed { Task { self.finishContinuedWork() } }   // nothing follows: the task asked at Record ends
            return id
        }
        if !claimed {
            // Another note is being written: queue this one behind it.
            guard !isWorking else {
                interrupted.insert(id)
                RecordingActivities.shared.update(noteID: id, .queued, line: Self.queuedMessage, percent: 0)
                return id
            }
            beginBackground()
            ownsBackground = true
            activeNoteID = id
            startNotesRun(id)
        }
        defer { notesRun = false }
        defer { if !interrupted.contains(id) { markAutoTried(id) } }
        defer { settleOvernight(id) }
        offerOvernightIfLong(id, audio: audio, duration: duration)
        stepCount = StepCount.notesRun(writesTranscript: true, outputsToWrite: OutputKind.allCases.map { _ in true })
        if !(await heatGate(id)) { return id }

        // 1. transcript
        stage = .transcribing(0)
        let done: TranscriptSidecar
        do {
            done = try await transcribe(audio, noteID: id, durationSeconds: duration)
        } catch {
            if parked.contains(id) || runSteppedAside { return id }   // stopped for the night or a recording; the cursor is kept
            NSLog("[sve] transcription stopped: %@", String(describing: error))
            if !pausedForHeat(id) { stopShort(error.localizedDescription, error: error) }
            return id
        }
        let transcript = await store(done, noteID: id, transcript: Self.joined(TranscriptRun.pieces(done)))
        TranscriptSidecar.remove(audioPath: audio.path)   // complete and saved to the note
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        if transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            stopShort(String(localized: "I couldn't hear anyone speaking in this recording."), noSpeech: true)
            return id
        }
        if TranscriptRule.tooShortForNotes(transcript) {
            NSLog("[sve] note %lld: %ld words, too short for notes", id, TranscriptRule.words(transcript))
            stopShort(TranscriptRule.tooShortMessage, noSpeech: true)
            return id
        }
        transcriptStored(id)

        // 2. notes
        await generateOutputs(noteID: id, transcript: transcript, lang: noteLang, setTitle: true)
        return id
    }

    /// A recording was cancelled (no Stop, so no run of its own follows): the runs that stepped aside for it carry on.
    /// With the app not in front, the overnight task is asked for too, so nothing waits until the next open.
    func recordingCancelled() {
        // Nothing waits: the task asked for at Record ends now (a run in flight ends it when it is done).
        guard !(interrupted.isEmpty && queued.isEmpty && insightDue.isEmpty) else { finishContinuedWork(); return }
        if !appIsActive { OvernightWork.shared.schedule() }
        guard !isWorking, !resuming else { return }
        Task { await self.resumeInterrupted() }
    }

    /// "Write my notes" on a note: memory work yields first; other work still running: the tap waits its turn.
    func regenerate(noteID: Int64) async {
        let asked = personTapped(noteID, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(asked: asked) }
        guard await personMayStart(QueuedTap(noteID: noteID, kind: .generate)) else { return }
        await rerun(noteID: noteID)
    }

    /// Re-runs the AI stage for an existing note (resume after interruption, or after a model change). Internal
    /// callers (the resume loop, a queued tap, Do it now, Try again) come here directly: nothing is queued.
    private func rerun(noteID: Int64) async {
        guard !isWorking else { return }
        starting = true
        defer { starting = false }
        guard let note = await CoreEngine.shared.note(noteID) else { unmarkOvernight(noteID); return }   // deleted
        noteTitles[noteID] = note.title
        defer { activityRunEnded(noteID) }
        guard ModelManager.shared.isReady else { lastFailure[noteID] = Self.waitingMessage; return }
        defer { settleOvernight(noteID) }
        // Already finished (for example queued twice): nothing to do, and no duplicate suggestions.
        guard WaitingNotes.needsWork(hasTranscript: !(note.transcript ?? "").isEmpty,
                                     outputs: OutputKind.allCases.map { note.output($0) != nil }) || partsRetry.contains(noteID) else {
            lastFailure[noteID] = nil   // nothing to say: it is done (a cut-off line here read as "couldn't finish")
            RecordingActivities.shared.update(noteID: noteID, .ready, line: note.title, percent: 100)
            // Its notes are stored, but a kill during the extract after them left it never extracted: queued now.
            if ExtractAfterNotes.queueOnResume(outputsStored: OutputKind.allCases.allSatisfy { note.output($0) != nil },
                                               alreadyExtracted: await CoreEngine.shared.noteExtracted(noteID)) {
                requeueFindReminders(noteID)
            }
            return
        }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        startNotesRun(noteID)
        stepCount = StepCount.notesRun(writesTranscript: (note.transcript ?? "").isEmpty,
                                       outputsToWrite: WaitingNotes.outputsToWrite(stored: OutputKind.allCases.map { note.output($0) }))
        defer { notesRun = false }
        if !(await heatGate(noteID)) { return }

        // No transcript yet (earlier transcription failed): redo it from the audio if we still have it.
        var t = note.transcript ?? ""
        if t.isEmpty {
            guard let path = AudioRecorder.existingAudioPath(note.audio_path) else {
                stopShort(String(localized: "I have nothing written down for this, and the audio is gone."), audioGone: true); return
            }
            stage = .transcribing(0)
            let done: TranscriptSidecar
            do {
                done = try await transcribe(URL(fileURLWithPath: path), noteID: noteID, durationSeconds: note.duration_seconds)
                t = Self.joined(TranscriptRun.pieces(done))
            } catch {
                if parked.contains(noteID) || runSteppedAside { return }   // stopped for the night or a recording; the cursor is kept
                NSLog("[sve] transcription stopped: %@", String(describing: error))
                if !pausedForHeat(noteID) { stopShort(error.localizedDescription, error: error) }
                return
            }
            if t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                stopShort(String(localized: "I couldn't hear anyone speaking in this recording."), noSpeech: true); return
            }
            t = await store(done, noteID: noteID, transcript: t)
            TranscriptSidecar.remove(audioPath: path)   // complete and saved to the note
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            transcriptStored(noteID)
        } else if let path = AudioRecorder.existingAudioPath(note.audio_path) {
            TranscriptSidecar.remove(audioPath: path)   // left over from before the transcript was saved
        }
        if TranscriptRule.tooShortForNotes(t) {
            NSLog("[sve] note %lld: %ld words, too short for notes", noteID, TranscriptRule.words(t))
            stopShort(TranscriptRule.tooShortMessage, noSpeech: true); return
        }
        // The note keeps its language: a resumed or retried run writes in note.lang, as a regenerate does (its parts,
        // the Try again lines and the note screen all read that one language).
        await generateOutputs(noteID: noteID, transcript: t, lang: note.lang, setTitle: note.title.hasPrefix("Recording "))
    }

    // MARK: regenerate one output, or all

    /// A "Write again" asked for: one output, or (kind nil) the title and all three outputs.
    struct Regeneration: Equatable {
        let noteID: Int64
        let kind: OutputKind?
        var kinds: [OutputKind] { kind.map { [$0] } ?? OutputKind.allCases }
    }

    /// What a regenerate replaced, kept by the note screen for Undo.
    struct Previous: Equatable {
        var title: String?
        var outputs: [OutputKind: String]
    }

    /// The regenerate running now (its note shows the progress and Stop under the text).
    private(set) var regenerating: Regeneration? { didSet { refreshExpiryOutcome() } }
    /// The last regenerate of a note that failed for real: the note shows "Try again". One interrupted (expiry, heat,
    /// the GPU stopped in the background) is queued again instead (TapQueue.requeuesRegenerate).
    private(set) var regenerateFailed: [Int64: Regeneration] = [:]
    @ObservationIgnored private var regenerateStopped = false

    /// "Write again" (one output) or "Write it all again" (kind nil: title and the three outputs; not reminders and
    /// tasks, and the sections are reused, not rewritten, unless they read as another language). The notes and the
    /// questions are written part by part (their parts cleared at the tap), then put together. Same source rule, heat rule and backend as the first
    /// time, with sampling, so it gives a new version. The old text stays until everything asked for is written,
    /// then it is replaced at once; after a failure or Stop nothing changes. Returns the replaced text (for Undo)
    /// on success.
    /// Another run still working: the tap waits its turn (nil returned; the queued run offers no Undo).
    @discardableResult
    func regenerateOutput(noteID id: Int64, kind: OutputKind?) async -> Previous? {
        let asked = personTapped(id, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(asked: asked) }
        guard await personMayStart(QueuedTap(noteID: id, kind: .regenerate(output: kind?.rawValue))) else { return nil }
        return await runRegenerate(QueuedTap(noteID: id, kind: .regenerate(output: kind?.rawValue)))
    }

    /// A regenerate, from the tap or the queue (its turn came; tap.cutOffs: its cut-offs in a row so far).
    private func runRegenerate(_ tap: QueuedTap) async -> Previous? {
        let id = tap.noteID
        guard case .regenerate(let output) = tap.kind else { return nil }
        let kind = output.flatMap(OutputKind.init(rawValue:))
        guard !isWorking else { return nil }
        starting = true
        defer { starting = false }
        let request = Regeneration(noteID: id, kind: kind)
        regenerateFailed[id] = nil
        let engine = CoreEngine.shared
        guard let note = await engine.note(id) else { return nil }   // deleted
        let transcript = note.transcript ?? ""
        // No speech: a real failure ("Try again" shows why nothing changed).
        // Too few words: Gemma would make the notes up (TranscriptRule).
        guard !TranscriptRule.tooShortForNotes(transcript) else { regenerateFailed[id] = request; return nil }
        guard ModelManager.shared.isReady else {
            // The AI is not ready: queued (the resume loop runs taps only once it is), the note shows the waiting line.
            NSLog("[sve] bg: note %lld: regenerate waits for the AI", id)
            queued = TapQueue.enqueue(queued, tap)
            return nil
        }
        beginBackground()
        defer { endBackground() }
        activeNoteID = id
        regenerating = request
        regenerateStopped = false
        // The parts of what is written again are cleared once, when this tap's first turn starts (the old text stays
        // until the new is put together): a turn queued again or resumed after a kill goes on from the parts stored.
        var tap = tap
        if !tap.partsCleared {
            if let kind {
                if let part = NoteSteps.Kind(kind) { await engine.clearSectionOutputs(noteID: id, kind: part) }
            } else {
                await engine.clearSectionOutputs(noteID: id, kind: nil)
            }
            tap.partsCleared = true
        }
        runningTap = tap   // on disk: a kill keeps it
        defer { runningTap = nil }
        // A regenerate never queues the note for resuming nor changes its resume banner: put those back after.
        let wasQueued = interrupted.contains(id), wasHeatPaused = heatPaused.contains(id), failure = lastFailure[id]
        // In the language the note was written in (as section backfill does).
        let written = await writeAgain(request, transcript: transcript, lang: note.lang)
        if !wasQueued { interrupted.remove(id) }
        if !wasHeatPaused { heatPaused.remove(id) }
        lastFailure[id] = failure
        regenerating = nil

        let outcome = RegenerateRules.outcome(written: written != nil, stopped: regenerateStopped)
        let parkedNow = parked.remove(id) != nil   // stopped for the night (the heat gate); runs when that holds
        if outcome == .replaced, let written, let now = await engine.note(id) {   // deleted meanwhile: nothing is written
            var previous = Previous(title: nil, outputs: [:])
            if let t = written.title { previous.title = now.title; await engine.setTitle(id, t); noteTitles[id] = t }
            for k in request.kinds {
                guard let text = written.outputs[k] else { continue }
                if let old = now.output(k) { previous.outputs[k] = old }
                await engine.setOutput(id, k, text)
            }
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            setStreak(id, .regenerate, nil)
            if noteTitles[id] == nil { noteTitles[id] = now.title }
            // Written with the app closed: "ready", as for the first notes (nothing is posted with the app in front).
            if NotifyRule.regenerateStage(summaryOnly: kind == .shortSummary) == NotifyRule.summary {
                notify(id) { NotesNotifier.summaryReady(noteID: id, title: $0, hold: $1) }
            } else {
                notify(id) { NotesNotifier.notesReady(noteID: id, title: $0, hold: $1) }
            }
            stage = .done
            return previous
        }
        if outcome == .failed, runSteppedAside {
            // Stepped aside for a recording: queued again as it was (no cut-off in its streak); it runs after Stop.
            NSLog("[sve] bg: note %lld: regenerate stepped aside for the recording; the old text is kept and it is queued again", id)
            queued = TapQueue.enqueue(queued, LiveFirst.requeued(tap))
        } else if outcome == .failed, parkedNow {
            // Stopped for the night: queued again as it was (no cut-off in its streak); it waits for the charger or
            // the overnight task (TapQueue.waitsForNight), as the note does.
            NSLog("[sve] bg: note %lld: regenerate stopped for the night; the old text is kept and it is queued again", id)
            queued = TapQueue.enqueue(queued, tap)
        } else if outcome == .failed {
            let gpuCutOff = BackendChoice.failureIsCutOff(aborted: false, wentToBackground: wentToBackground || !appIsActive,
                                                          gpuStep: stepOnGPU, continuedTaskHeld: ContinuedWork.shared.isHeld)
            // The note's streak of unknown errors (FailureKind): the first is queued again, the second is "Try again".
            let streak = streak(id, .regenerate)
            if FailureKind.isUnknown(cancelled: false, aborted: abort.isSet, cutOffByBackground: gpuCutOff, heat: runPausedForHeat,
                                     noSpeech: false, audioGone: false) {
                setStreak(id, .regenerate, streak + 1)
            }
            if TapQueue.requeuesRegenerate(stoppedByPerson: regenerateStopped, aborted: abort.isSet, heatPaused: runPausedForHeat,
                                           parkedForNight: false, gpuCutOff: gpuCutOff, unknownStreak: streak) {
                // Interrupted, not failed: the same request runs again by itself (the old text stays until the new
                // is complete), and its "ready" notification says so.
                let again = TapQueue.requeued(tap)
                NSLog("[sve] bg: note %lld: regenerate cut off at %ld%% (%ld in a row); the old text is kept and it is queued again",
                      id, ContinuedWork.shared.percent, again.cutOffs)
                queued = TapQueue.enqueue(queued, again)
                if !appIsActive, overnightRun == nil { OvernightWork.shared.schedule() }
            } else {
                regenerateFailed[id] = request
            }
        }
        stage = .idle
        return nil
    }

    /// The new text for a regenerate, nothing stored. nil: failed, too hot (critical), stopped or cut off.
    private func writeAgain(_ request: Regeneration, transcript: String, lang: String) async -> Previous? {
        let id = request.noteID, engine = CoreEngine.shared
        if !(await heatGate(id)) { return nil }
        do {
            try await loadSelectedModel()
        } catch {
            NSLog("[sve] regenerate: model not loaded: %@", String(describing: error))
            return nil
        }
        let pieces = await engine.pieces(id)
        // Part headings in the other language (a language switch): the sections are written again first.
        await rewriteSectionsIfOtherLanguage(id, lang: lang)
        if !SectionWriter.coversAll(await engine.sections(id), pieceCount: pieces.count) {
            guard await writeSections(noteID: id, lang: lang, quiet: true) != nil else { return nil }
        }
        if abort.isSet { return nil }
        let covered = SectionWriter.coversAll(await engine.sections(id), pieceCount: pieces.count)
        if covered { await markRewrittenSections(id, lang: lang) }
        let material = Self.notesMaterial(pieces, transcript: transcript)
        let fromSections = await notesFromSections(covered: covered, transcript: material, lang: lang)
        var result = Previous(title: nil, outputs: [:])
        // Lock-screen progress: the title (regenerate all), then each output; the heartbeat runs inside each output.
        let steps = RegenerateRules.steps(all: request.kind == nil, outputs: request.kinds.count)
        var done = 0
        ContinuedWork.shared.update(fraction: RegenerateRules.fraction(done: 0, part: 0, steps: steps))
        if request.kind == nil {
            if abort.isSet { return nil }
            if !(await heatGate(id)) { return nil }
            result.title = await writeTitle(noteID: id, transcript: transcript, lang: lang, fromSections: fromSections)
            done += 1
            ContinuedWork.shared.update(fraction: RegenerateRules.fraction(done: done, part: 0, steps: steps))
        }
        for kind in request.kinds {
            if abort.isSet { return nil }
            if !(await heatGate(id)) { return nil }
            if covered, let part = NoteSteps.Kind(kind) {
                // In small saved steps: the parts cleared at the tap, then put together. The old text stays until then.
                let step = done, count = await engine.sections(id).count
                let state = await engine.sectionOutputsState(noteID: id, lang: lang) ?? []
                let left = NoteSteps.parts(NoteSteps.plan(sections: count, state: state, hasSummary: true), kind: part).count
                let end = await writeParts(noteID: id, lang: lang, kinds: [part], done: count - left, total: count + 1,
                                           fraction: { RegenerateRules.fraction(done: step, part: $0, steps: steps) })
                guard end == .done else { NSLog("[sve] regenerate: %@ parts stopped (%@)", kind.title, String(describing: end)); return nil }
                let now = await engine.sectionOutputsState(noteID: id, lang: lang) ?? []
                guard NoteSteps.gaveUpParts(now, kind: part).isEmpty else {
                    NSLog("[sve] regenerate: a %@ part failed 3 times; the old text stays", kind.title)
                    return nil
                }
                guard let text = await engine.assembleOutput(noteID: id, kind: part, lang: lang), RegenerateRules.accepts(text) else {
                    NSLog("[sve] regenerate: nothing to put together for %@", kind.title)
                    return nil
                }
                NSLog("[sve] regenerate: %@ put together from %ld parts", kind.title, count)
                result.outputs[kind] = text
                done += 1
                ContinuedWork.shared.update(fraction: RegenerateRules.fraction(done: done, part: 0, steps: steps))
                continue
            }
            do {
                let step = done
                let text = try await writeOutput(kind, noteID: id, transcript: material, lang: lang, fromSections: fromSections,
                                                 fraction: { RegenerateRules.fraction(done: step, part: $0, steps: steps) })
                guard RegenerateRules.accepts(text) else { NSLog("[sve] regenerate: empty %@", kind.title); return nil }
                result.outputs[kind] = text
                done += 1
                ContinuedWork.shared.update(fraction: RegenerateRules.fraction(done: done, part: 0, steps: steps))
            } catch {
                NSLog("[sve] regenerate failed: %@", String(describing: error))
                return nil
            }
        }
        return abort.isSet ? nil : result
    }

    /// Stop on a regenerate: the run is cancelled and the old text kept.
    func stopRegenerating(noteID id: Int64) {
        guard regenerating?.noteID == id else { return }
        regenerateStopped = true
        abort.set(true)
    }

    /// Undo after a regenerate: the previous text (and title) back, saved.
    func restore(_ previous: Previous, noteID id: Int64) async {
        let engine = CoreEngine.shared
        guard !(isBusy && activeNoteID == id), await engine.note(id) != nil else { return }
        if let t = previous.title { await engine.setTitle(id, t) }
        for (k, text) in previous.outputs { await engine.setOutput(id, k, text) }
        NotificationCenter.default.post(name: .notesChanged, object: nil)
    }

    /// "Try again" on a note's failed or paused banner: the usual resume path, now.
    func tryAgain(noteID id: Int64) async {
        let asked = personTapped(id, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(asked: asked) }
        // Parts that failed 3 times are tried again by the note's next run (this one, or its queued turn); with the
        // notes already stored, only those parts are written and the notes put together again.
        // Both languages, as clearPartsLeft clears them (parts left by a run from before runs kept the note's language).
        var left: [Int] = []
        for lang in ["en", "te"] { left += await Self.partsLeft(noteID: id, lang: lang, kind: nil) }
        if !left.isEmpty { partsRetry.insert(id) }
        guard await personMayStart(QueuedTap(noteID: id, kind: .generate)) else { return }
        interrupted.remove(id)
        heatPaused.remove(id)
        await rerun(noteID: id)
    }

    /// Audio → text, piece by piece, from where live transcription (or an interrupted run) stopped (FirstPassFlow):
    /// Shruti on the CPU first; the pieces it misses, and the rest of a note it fails on, go to Gemma on the backend
    /// for right now (StepBackend: the GPU in front, the CPU in the background), which hears the recording itself,
    /// so any language mix works. Stops (the progress callback cancels) as
    /// soon as the phone gets too hot or the background time runs out; the caller treats that as a pause,
    /// and the next run continues from the saved cursor.
    private func transcribe(_ audio: URL, noteID: Int64, durationSeconds: Int64) async throws -> TranscriptSidecar {
        stepOnGPU = false
        let duration = Double(max(1, durationSeconds))
        func report(_ s: TranscriptSidecar) {
            // Pending pieces count as not done until Gemma fills them (doneSeconds), so the bar and the Live
            // Activity do not reach the end while Gemma still has their audio.
            let doneSeconds = s.doneSeconds
            // The fraction and the pieces first: the stage change sends them to the recording's Live Activity.
            ContinuedWork.shared.update(fraction: NotesProgress.fraction(transcribedSeconds: doneSeconds, durationSeconds: duration))
            transcribing = (doneSeconds, duration)
            stage = .transcribing(min(1, doneSeconds / duration))
        }
        var done = TranscriptSidecar.load(audioPath: audio.path)
        report(done)   // from where live transcription got to
        let abort = self.abort
        let shouldStop: () -> Bool = { ContinuedWork.beat(); return Self.critical || abort.isSet }
        let pieceBackend: () async -> ModelBackend = {
            let b: ModelBackend = StepBackend.piecesOnGPU ? await self.stepBackend(piece: true) : .cpu
            self.stepOnGPU = b == .gpu
            return b
        }
        // Shruti writes first while it may; a failure with it leaves the rest of this note to Gemma, and so does a
        // restart from the start (gemma_from_start, kept across pauses).
        var shrutiOff = done.gemma_from_start == true
        var restarted = false
        var atEnd = false
        // Asked before every group (ShrutiChoice): CPU only, so the background does not matter.
        func shrutiMay() -> Bool {
            ShrutiChoice.use(shrutiReady: !shrutiOff && ShrutiRecognizer.shared.ready,
                             heatPaused: HeatGovernor.shared.level == .paused)
        }
        var lastLine: String?
        func logStep(_ line: String) {
            guard line != lastLine else { return }
            lastLine = line
            NSLog("[sve] note %lld transcript: %@", noteID, line)
        }
        while true {
            let step = FirstPassFlow.next(shruti: !atEnd && shrutiMay(), cursorAtEnd: atEnd,
                                          pending: done.pendingIndices.count)
            switch step {
            case .shruti:
                logStep("shruti")
                stepOnGPU = false
                // Each Shruti piece rests as long as the piece before it (the first: from the last gate pass, which
                // this clears, so it does not rest for a Gemma step that came before).
                heatGatePassed = nil
                do {
                    let (d, stop) = try await TranscriptRun.runShruti(audioPath: audio.path, isFinal: true,
                                                                      shouldStop: shouldStop,
                                                                      threads: { ShrutiThreads.count(HeatGovernor.shared.level) },
                                                                      beforePiece: { await self.heatGate(noteID, light: true) },
                                                                      onPiece: { report($0) })
                    done = d
                    if stop == .yielded { throw CancellationError() }   // paused for heat; the cursor is kept
                    atEnd = true
                } catch {
                    if error is CancellationError || shouldStop() || Task.isCancelled {
                        await ShrutiRecognizer.unloadOffMain()   // paused: its memory is not held meanwhile
                        throw error
                    }
                    NSLog("[sve] shruti stopped (%@); Gemma goes on from the saved cursor", String(describing: error))
                    shrutiOff = true
                    done = TranscriptSidecar.load(audioPath: audio.path)
                }
            case .gemmaRemainder:
                logStep(shrutiOff ? "gemma (shruti failed)" : "gemma (shruti not ready or heat paused)")
                var backToShruti = false
                let (d, stop) = try await TranscriptRun.run(audioPath: audio.path, isFinal: true, shouldStop: shouldStop,
                                                            backend: pieceBackend,
                                                            beforePiece: {
                                                                // Shruti may run again (heat came down from paused):
                                                                // it takes the next group, after its own heat gate.
                                                                if shrutiMay() { backToShruti = true; return false }
                                                                return await self.heatGate(noteID)
                                                            },
                                                            onPiece: { report($0) })
                done = d
                if stop == .yielded, backToShruti { continue }
                if stop == .yielded { throw CancellationError() }   // paused for heat; the cursor is kept
                atEnd = true
            case .fillPending:
                logStep("gemma fills the pieces shruti left")
                await ShrutiRecognizer.unloadOffMain()
                done = try await TranscriptRun.fillPending(audioPath: audio.path, shouldStop: shouldStop,
                                                           backend: pieceBackend,
                                                           beforePiece: { await self.heatGate(noteID) },
                                                           onPiece: { report($0) })
            case .done:
                // Shruti heard no text in the whole recording, though some of it was not silent: quiet speech may
                // be there (below needsGemma's peak), so Gemma transcribes the recording from the start, once.
                let empty = Self.joined(TranscriptRun.pieces(done)).trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                if !restarted, done.gemma_from_start != true,
                   ShrutiPiece.retryWithGemma(transcriptEmpty: empty, maxPeak: done.shruti_peak) {
                    NSLog("[sve] note %lld: shruti heard no text (loudest piece %.4f); gemma from the start",
                          noteID, Double(done.shruti_peak))
                    restarted = true
                    shrutiOff = true
                    atEnd = false
                    done = TranscriptSidecar.gemmaFromStart()
                    done.save(audioPath: audio.path)   // the Gemma run reads the sidecar: cursor 0
                    report(done)
                    continue
                }
                await ShrutiRecognizer.unloadOffMain()   // Gemma writes the rest of the note
                return done
            }
        }
    }

    /// A finished transcription into the note: pieces, transcript, and the sections written while recording
    /// (those that still fit the pieces; replacing whatever the note had, which belonged to older pieces).
    /// Returns the transcript as stored: the saved words written in where the transcriber misheard them (Shruti never
    /// sees them), in the pieces and so in the transcript (sve_vocab_correct; never a medicine).
    @discardableResult
    private func store(_ done: TranscriptSidecar, noteID id: Int64, transcript heard: String) async -> String {
        let engine = CoreEngine.shared
        let raw = TranscriptRun.pieces(done)
        let pieces = await engine.vocabCorrected(raw)
        let transcript = Self.joined(raw) == heard.trimmingCharacters(in: .whitespacesAndNewlines) ? Self.joined(pieces) : heard
        if transcript != heard {
            let changed = zip(raw, pieces).filter { $0.text != $1.text }.count
            NSLog("[sve] note %lld: saved words fixed %ld of %ld pieces", id, changed, raw.count)
        }
        await engine.setPieces(id, pieces)
        await engine.setTranscript(id, transcript)
        await engine.setSections(id, NoteSection.validPrefix(done.sections, pieceCount: pieces.count))
        // Notes and questions written while recording (LiveParts): into the note when its section still has the
        // range they were written for; the others are dropped and written again by the normal steps.
        guard !done.parts.isEmpty else { return transcript }
        var put = 0, dropped = 0
        for part in done.parts {
            if await engine.putSectionOutput(noteID: id, part) == 1 { put += 1 } else { dropped += 1 }
        }
        NSLog("[sve] live parts: %ld put into note %lld at Stop, %ld dropped (range changed)", put, id, dropped)
        return transcript
    }

    private static func joined(_ pieces: [TranscriptPiece]) -> String {
        pieces.map(\.text).joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// The text the summary, notes and questions are written from (and the fit check counts): the transcript with
    /// the pieces heard with conf < 0.80 marked "[unclear] " (N7 rev 1), when the stored pieces are that transcript;
    /// otherwise the transcript as it is. The title and the stored transcript stay unmarked.
    private static func notesMaterial(_ pieces: [TranscriptPiece], transcript: String) -> String {
        guard !pieces.isEmpty, joined(pieces) == transcript.trimmingCharacters(in: .whitespacesAndNewlines),
              let marked = CoreEngine.shared.notesMaterial(pieces), !marked.isEmpty else { return transcript }
        return marked
    }

    private func loadSelectedModel() async throws {
        stage = .loadingModel
        let backend = await stepBackend()
        stepOnGPU = backend == .gpu   // before the load, which may throw
        try await ModelManager.shared.loadIntoCore(backend: backend)
    }

    /// The backend for the next step of a notes run (a piece, a section summary, a part, an output), picked at the step
    /// boundary and never during a step (StepBackend): the GPU once the app has been in front a few seconds with the
    /// heat full or eased, the CPU in the background, while recording or when warmer. A move is logged.
    private func stepBackend(piece: Bool = false) async -> ModelBackend {
        let c = await stepChoice(piece: piece)
        if let line = StepBackend.moveLine(from: c.current, to: c.backend, why: c.why) { NSLog("[sve] %@", line) }
        return c.backend
    }

    /// StepBackend's choice for now, with what is loaded; not logged.
    private func stepChoice(piece: Bool = false) async -> (backend: ModelBackend, why: StepBackend.Why, current: ModelBackend?) {
        let state: ThreadChoice.AppState
        switch UIApplication.shared.applicationState {
        case .active: state = .active
        case .background: state = .background
        default: state = .inactive
        }
        if state == .background { frontSince = nil } else if frontSince == nil { frontSince = Date() }
        let current = await CoreEngine.shared.loadedBackend
        let c = StepBackend.choose(state: state, frontSeconds: frontSince.map { Date().timeIntervalSince($0) } ?? 0,
                                   heat: HeatGovernor.threads.currentLevel, recording: LiveTranscriber.shared.isRecording,
                                   current: current, piece: piece)
        return (c.backend, c.why, current)
    }

    /// One model step on the backend for right now (StepBackend: GPU in front, else CPU). A GPU run cut off by the
    /// app going to the background is retried once on the CPU while a continued-processing task is held.
    private func onBackend<T>(_ body: () async throws -> T) async throws -> T {
        let backend = await stepBackend()
        stepOnGPU = backend == .gpu
        try await ModelManager.shared.loadIntoCore(backend: backend)
        do {
            return try await body()
        } catch {
            guard BackendChoice.retryOnCPU(failedOn: backend, wentToBackground: wentToBackground || !appIsActive,
                                           continuedTaskHeld: ContinuedWork.shared.isHeld, cancelled: abort.isSet,
                                           alreadyRetried: false) else { throw error }
            NSLog("[sve] GPU run stopped in the background; retrying on the CPU")
            stepOnGPU = false
            try await ModelManager.shared.loadIntoCore(backend: .cpu)
            return try await body()
        }
    }

    /// The system ended the continued-processing task: stop the run; the note resumes from where it was, by itself
    /// in the overnight task (cutOff) or when the app is opened.
    /// hold: background time taken in the expiration handler itself, before the hop here; it ends once every
    /// notification and Live Activity end below has taken its own.
    private func continuedWorkExpired(hold: NotesNotifier.Hold) {
        defer { hold.end() }
        abort.set(true)
        // Memories and the insight run after the notes are stored: stopping them leaves nothing to resume or show.
        var running: Int64?
        // A regenerate in flight is queued again once its run returns (runRegenerate: abort is set).
        // Only a notes run is cut off here: a regenerate (regenerating) is queued again by runRegenerate, and its Live
        // Activity is not turned into a cut-off one.
        if notesRun, TaskLifecycle.expiryCutsOff(working: isWorking, memoriesOnly: memoryWork, notesStored: notesStored),
           let id = activeNoteID {
            interrupted.insert(id)
            lastFailure[id] = Self.cutOffMessage
            cutOff(id)
            running = id
        }
        // Notes waiting their turn (queued behind this one, waiting for the AI, cut off or paused for heat earlier):
        // every queued note is notes work, and nothing else would run them now.
        // Not a note that stepped aside for the recording still running: it waits quietly (LiveFirst).
        for id in interrupted.sorted() where id != running && !waitsQuietly(id) { cutOff(id, queued: true) }
        // Taps waiting their turn: nothing runs them now either; the overnight task (on battery too) or the next open does.
        if !(queued.isEmpty && regenerating == nil), !appIsActive {
            NSLog("[sve] bg: %ld queued taps wait for the overnight task or the next open", queued.count)
            OvernightWork.shared.schedule()
        }
    }

    /// A notes run starts for this note: it may be cut off (and notify) again.
    private func startNotesRun(_ id: Int64) {
        notesRun = true
        runningNote = id
        stepCount = nil
        transcribing = nil
        pausedPosted.remove(id)
        postedStages[id] = nil
        steppedAside.remove(id)
    }

    /// A notes run cut off with the app not in front (the continued-processing task expired, the app left the
    /// front, or the overnight task ended): the note goes on the overnight list by itself (a mark of the person's
    /// stays theirs), the overnight task is asked for (on battery too) and, once per run, "paused" is posted.
    /// The run in flight was cancelled; every piece, section and output already finished is kept.
    /// queued: the note was waiting its turn, not running (its progress is not known: 0%).
    private func cutOff(_ id: Int64, queued: Bool = false) {
        RecordingActivities.shared.update(noteID: id, .cutOff, line: lastFailure[id] ?? Self.cutOffMessage,
                                          percent: queued ? 0 : ContinuedWork.shared.percent)
        guard OvernightList.enrolsAfterCutOff(notesRun: queued || notesRun, memoriesOnly: !queued && memoryWork,
                                              appActive: appIsActive) else { return }
        let percent = queued ? 0 : ContinuedWork.shared.percent
        if OvernightList.marksAutomatic(alreadyMarked: overnight.contains(id)) {
            markOvernight(id)
            automatic.insert(id)
            saveAutomatic()
        }
        NSLog("[sve] bg: note %lld cut off at %ld%%; it finishes by itself in the overnight task", id, percent)
        OvernightWork.shared.schedule()
        guard NotifyRule.postsPaused(cutOffByOvernightTask: overnightOwnsRun.isSet), pausedPosted.insert(id).inserted else { return }
        guard !askedToOpen.contains(id) else { return }   // "Your recording is saved" already says what happens next
        notify(id) { NotesNotifier.paused(noteID: id, title: $0, percent: percent, hold: $1) }
    }

    /// The one decision (FailureKind) for a run that stopped short, from what is known now: cancelled (a
    /// CancellationError, e.g. the transcription stopping), aborted (an expiry, the overnight task ended), the GPU cut
    /// off in the background (BackendChoice.failureIsCutOff), heat (paused at critical in this run, or too warm to
    /// start), no speech, the audio gone. An unknown error counts in the note's streak (failureStreak).
    private func classify(_ id: Int64?, run: FailureKind.Run, error: Error?, heat: Bool, noSpeech: Bool,
                          audioGone: Bool) -> FailureKind.Outcome {
        let cancelled = error is CancellationError || Task.isCancelled
        let aborted = abort.isSet
        let background = BackendChoice.failureIsCutOff(aborted: false, wentToBackground: wentToBackground || !appIsActive,
                                                       gpuStep: stepOnGPU, continuedTaskHeld: ContinuedWork.shared.isHeld)
        let hot = heat || runPausedForHeat
        let streak = id.map { self.streak($0, run) } ?? 0
        if let id, FailureKind.isUnknown(cancelled: cancelled, aborted: aborted, cutOffByBackground: background, heat: hot,
                                         noSpeech: noSpeech, audioGone: audioGone) {
            setStreak(id, run, streak + 1)
        }
        return FailureKind.classify(cancelled: cancelled, aborted: aborted, cutOffByBackground: background, heat: hot,
                                    noSpeech: noSpeech, audioGone: audioGone, unknownStreak: streak)
    }

    /// Every run of a note that stops short comes here (the only place that sets .failed). A cut-off keeps whatever
    /// was finished and continues by itself: a notes run's note is queued again (interrupted) with the "continues by
    /// itself" line and, with the app not in front, goes on the overnight list and posts "paused" once (cutOff); a
    /// tap's caller queues the tap again. Only a real failure (no speech, the audio gone, an unknown error twice in a
    /// row) shows as failed (the stage didSet: the note's line, the failed notification, the Live Activity).
    @discardableResult
    private func stopShort(_ line: String, error: Error? = nil, heat: Bool = false, noSpeech: Bool = false,
                           audioGone: Bool = false) -> FailureKind.Outcome {
        let id = activeNoteID
        let outcome = classify(id, run: notesRun ? .notes : .findReminders, error: error, heat: heat, noSpeech: noSpeech,
                               audioGone: audioGone)
        guard outcome == .cutOff, let id else {
            if let id { NSLog("[sve] bg: note %lld failed: %@", id, line) }
            stage = .failed(line)
            return .failed
        }
        NSLog("[sve] bg: note %lld stopped short (%@); a cut-off, it continues by itself", id,
              error.map { String(describing: $0) } ?? line)
        guard notesRun else { stage = .idle; return .cutOff }   // a tap: queued again by its caller
        let message = heatPaused.contains(id) ? Self.heatMessage : Self.cutOffMessage
        interrupted.insert(id)
        lastFailure[id] = message
        stage = .paused(message)
        cutOff(id)
        return .cutOff
    }

    /// Find reminders cut off (too warm, an interruption, an unknown error the first time): it runs again by itself.
    private func requeueFindReminders(_ id: Int64) {
        NSLog("[sve] bg: note %lld: find reminders cut off; it is queued again", id)
        queued = TapQueue.enqueue(queued, QueuedTap(noteID: id, kind: .findReminders))
    }

    /// The transcript is stored after Stop (a notes run): "Transcript ready", once for this run. Not with the
    /// placeholder title: "Summary ready" follows with the real one.
    private func transcriptStored(_ id: Int64) {
        setStreak(id, .notes, nil)
        let step = partStored(.transcript)
        notifyStage(id, NotifyRule.transcript) { title, hold in
            guard NotifyRule.saysTranscriptReady(title: title, counting: step != nil) else {
                NSLog("[sve] bg: note %lld: transcript stored; the title is still the placeholder, no notification", id)
                hold.end()
                return
            }
            NotesNotifier.transcriptReady(noteID: id, title: title, step: step, hold: hold)
        }
    }

    /// A part of this notes run is stored: counted (StepCount), and the note screen and Live Activity show "1 of 3
    /// ready" now. Returns the "ready" notification's step line (nil without a counter).
    private func partStored(_ part: StepCount.Part) -> String? {
        guard notesRun, var steps = stepCount, let n = steps.stored(part) else { return nil }
        stepCount = steps
        NSLog("[sve] note %lld: %@ ready, %ld of %ld", activeNoteID ?? 0, part.rawValue, n, steps.total)
        if case .transcribing = stage { transcribing = nil } else { followStage() }   // the next stage shows it
        if stage.isWorking { ContinuedWork.shared.update(subtitle: progressLine) }
        return steps.readyLine(step: n, part: part)
    }

    /// A part of the note's notes is ready: posted at most once per stage in this run (NotifyRule.next), under the
    /// note's one identifier, so each replaces the one before. Nothing shows with the app in front (NotesNotifier).
    private func notifyStage(_ id: Int64, _ part: String, _ post: @escaping (String, NotesNotifier.Hold) -> Void) {
        guard NotifyRule.next(posted: postedStages[id] ?? [], stage: part) else { return }
        postedStages[id, default: []].insert(part)
        NSLog("[sve] bg: note %lld: %@ ready (app %@)", id, part, appIsActive ? "in front" : "not in front")
        notify(id, post)
    }

    /// Takes the background time now, before any await, then posts with the note's title: the one known from this
    /// launch at once, else read from the core.
    private func notify(_ id: Int64, _ post: @escaping (String, NotesNotifier.Hold) -> Void) {
        // This notice replaces "Your recording is saved" (one identifier per note): opening the app must not remove it.
        askedToOpen.remove(id)
        let hold = NotesNotifier.Hold()
        if let title = noteTitles[id] { post(title, hold); return }
        Task { post(await CoreEngine.shared.note(id)?.title ?? "", hold) }
    }

    /// A notes run failed for real (not a cut-off) with the app not in front: the note's failure line, as a
    /// notification.
    private func notifyFailed(_ id: Int64, line: String) {
        guard notesRun, !appIsActive else { return }
        NSLog("[sve] bg: note %lld: notes failed in the background: %@", id, line)
        notify(id) { NotesNotifier.failed(noteID: id, title: $0, line: line, hold: $1) }
    }

    /// Steps after transcription and before the notes are stored, for the lock-screen progress: sections, title, the
    /// three outputs. The reminders come after the notes (ExtractAfterNotes), in the after-notes progress.
    private static let notesSteps = 2 + OutputKind.allCases.count
    private static let sectionsStep = 0
    private static let firstOutputStep = 2

    private func generateOutputs(noteID id: Int64, transcript: String, lang: String, setTitle: Bool) async {
        let engine = CoreEngine.shared
        guard ModelManager.shared.isReady else {
            lastFailure[id] = Self.waitingMessage
            stage = .idle
            return
        }
        if !(await heatGate(id)) { return }
        do {
            try await loadSelectedModel()
        } catch {
            stopShort(error.localizedDescription, error: error)
            return
        }
        // Loading reads the model from storage and makes little heat: it is not work the next rest pays for
        // (a hot phone rested as long as the load, note 15, 2026-11-21).
        heatGatePassed = HeatGovernor.now

        // The notes first; reminders, tasks and profile changes after them (ExtractAfterNotes), so the summary never
        // waits behind them.
        let abort = self.abort
        let steps = Self.notesSteps   // sections, title, three outputs
        ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 0, outputsTotal: steps))
        if await engine.pieces(id).isEmpty {   // note from before pieces existed
            let dur = await engine.note(id)?.duration_seconds ?? 0
            await engine.setPieces(id, PiecesBackfill.pieces(transcript: transcript, durationSeconds: dur))
        }
        if abort.isSet { stopShort(CoreError.generationFailed.localizedDescription); return }   // cut off (aborted)

        // Section summaries the note does not have yet (all of them for a note from before sections; the chapters
        // come from them; written again when they read as another language). The title comes from the transcript
        // whenever it fits one prompt, else from the sections; the summary, notes and questions are written in steps
        // from the sections (writeInSteps), and from the transcript only when the sections could not be written.
        // One pass: when the transcript fits one prompt, the title, summary, notes and questions come from one
        // generation. Not for a refill, a resumed run with some outputs stored, or a title that is not to be set; a
        // reply that does not split falls back to the steps below. A note that takes it and has no section summaries
        // yet gets its notes first, its sections after (NotesOrder); one with sections stored keeps them first.
        let refill = partsRetry.contains(id)   // Try again on parts that were left: only those, then put together
        let material = Self.notesMaterial(await engine.pieces(id), transcript: transcript)
        let storedNow = await engine.note(id).map { n in OutputKind.allCases.map { n.output($0) } }
            ?? OutputKind.allCases.map { _ in nil }
        var onePassEligible = false
        if setTitle, !refill, WaitingNotes.outputsToWrite(stored: storedNow).allSatisfy({ $0 }), Self.onePassAllowed {
            onePassEligible = (try? await onBackend({ await engine.allFits(material) })) ?? false
        }
        let order = NotesOrder.order(onePassEligible: onePassEligible, sectionsStored: !(await engine.sections(id)).isEmpty)
        var onePassDone = false
        if order == .notesFirst {
            NSLog("[sve] note %lld: notes first, sections after", id)
            // Right after the gate and the load: still paused or stepped aside if it must, but no second rest.
            if !(await heatGate(id, light: true)) { return }
            onePassDone = await writeAllInOnePass(noteID: id, transcript: material, lang: lang, steps: steps)
            if !onePassDone, abort.isSet { stopShort(CoreError.generationFailed.localizedDescription); return }
            // Not split: the sections are written and the steps run, as for any other note.
        }

        var covered = false, fromSections = false
        if !onePassDone {
            await rewriteSectionsIfOtherLanguage(id, lang: lang)
            guard let c = await writeSections(noteID: id, lang: lang, quiet: false) else { return }
            covered = c
            if covered { await markRewrittenSections(id, lang: lang) }
            ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 1, outputsTotal: steps))
            fromSections = await notesFromSections(covered: covered, transcript: material, lang: lang)
            if setTitle, !(await heatGate(id)) { return }
            // Sections first (stored already, e.g. written while recording): the one pass after them, as before.
            if order == .sectionsFirst, onePassEligible {
                onePassDone = await writeAllInOnePass(noteID: id, transcript: material, lang: lang, steps: steps)
                if !onePassDone, abort.isSet { stopShort(CoreError.generationFailed.localizedDescription); return }
            }
        }

        if !onePassDone, setTitle, let t = await writeTitle(noteID: id, transcript: transcript, lang: lang, fromSections: fromSections) {
            await engine.setTitle(id, t)
            noteTitles[id] = t
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }
        if !onePassDone { ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 2, outputsTotal: steps)) }

        // A resumed run writes only what is not stored yet: a summary already announced is not rewritten or announced again.
        let stored = await engine.note(id).map { n in OutputKind.allCases.map { n.output($0) } }
            ?? OutputKind.allCases.map { _ in nil }
        let toWrite = WaitingNotes.outputsToWrite(stored: stored)
        // Every note with sections (a short recording has one) is written in small saved steps, so a cut-off costs
        // one step. The whole-transcript path below is kept only for a note whose sections could not be written.
        if onePassDone {
            NSLog("[sve] note %lld: notes from one pass; no steps to write", id)
        } else if covered {
            if refill, await clearPartsLeft(id) { NSLog("[sve] note %lld: the parts that were left are tried again", id) }
            guard await writeInSteps(noteID: id, lang: lang, material: material, toWrite: toWrite, refill: refill) else { return }
            partsRetry.remove(id)
        } else {
            NSLog("[sve] note %lld: no sections to write from; the notes come from the whole transcript", id)
        }
        for (i, kind) in OutputKind.allCases.enumerated() where !covered && !onePassDone {
            guard toWrite[i] else {
                NSLog("[sve] note %lld: %@ already stored; not written again", id, kind.title)
                ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: Self.firstOutputStep + 1 + i, outputsTotal: steps))
                continue
            }
            if abort.isSet { stopShort(CoreError.generationFailed.localizedDescription); return }   // cut off (aborted)
            if !(await heatGate(id)) { return }
            do {
                let step = Self.firstOutputStep + i, total = Self.notesSteps
                let text = try await writeOutput(kind, noteID: id, transcript: material, lang: lang, fromSections: fromSections,
                                                 fraction: { NotesProgress.fraction(outputsDone: step, part: $0, outputsTotal: total) })
                // Stored as soon as written: the summary is readable before the notes.
                await engine.setOutput(id, kind, text)
                setStreak(id, .notes, nil)
                NotificationCenter.default.post(name: .notesChanged, object: nil)
                ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: Self.firstOutputStep + 1 + i, outputsTotal: steps))
                if kind == .shortSummary {   // the title was written just before it
                    let step = partStored(.summary)
                    notifyStage(id, NotifyRule.summary) { NotesNotifier.summaryReady(noteID: id, title: $0, step: step, hold: $1) }
                }
            } catch {
                stopShort(error.localizedDescription, error: error)
                return
            }
        }
        // The detailed notes and the questions are stored: "ready" now (when the app is not in front), not after the
        // memories.
        let step = partStored(.notes)
        notifyStage(id, NotifyRule.notes) { NotesNotifier.notesReady(noteID: id, title: $0, step: step, hold: $1) }
        RecordingActivities.shared.update(noteID: id, .ready, line: noteTitles[id] ?? "", percent: 100)
        notesStored = true   // from here an expiry only stops the run (TaskLifecycle.expiryCutsOff)
        let after = NotesOrder.afterNotes(onePassDone && order == .notesFirst ? .notesFirst : .sectionsFirst)
        await extractAfterNotes(id, span: after.extract)
        // Notes first: the section summaries now (the memories come from them), after the notes and like the
        // memories: a stop or failure never touches the notes, and what is left is finished later (sectionsDue).
        memoryStop.set(false)
        if let span = after.sections {
            await rewriteSectionsIfOtherLanguage(id, lang: lang)
            if await writeSections(noteID: id, lang: lang, quiet: true, afterNotes: span) == true {
                await markRewrittenSections(id, lang: lang)
            } else if !(await engine.pieces(id)).isEmpty {   // no pieces: no sections to finish later
                NSLog("[sve] note %lld: sections stopped after the notes; the notes stay, the rest is finished later", id)
                markSectionsDue(id)
            }
        }
        // Memories now, never before them; a stop or failure here never fails the notes
        // (sections left are done later, idle in front or overnight).
        _ = await writeMemories(noteID: id, quiet: false, span: after.memories)
        ContinuedWork.shared.update(afterNotes: after.insight)   // the insight: the note's last part, under the same task
        if !refill { await writeInsight(id) }   // a part written again does not change the insight
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        stage = .done
        Task { await self.compressAudio(noteID: id) }   // the notes are done; the WAV can shrink
    }

    /// Reminders, tasks and profile changes, after the note's notes are stored (ExtractAfterNotes). A failure never
    /// touches the notes; a stop or cut-off queues it again (find reminders), so it runs later by itself.
    private func extractAfterNotes(_ id: Int64, span: ClosedRange<Double>) async {
        let engine = CoreEngine.shared
        let abort = self.abort
        switch ExtractAfterNotes.action(alreadyExtracted: await engine.noteExtracted(id),
                                        recordingLive: LiveTranscriber.shared.liveRunning) {
        case .skip:
            // A resumed run or a refill: extracted already (its cards may be accepted or rejected).
            NSLog("[sve] note %lld: reminders and tasks already extracted; not extracted again", id)
        case .queue:
            NSLog("[sve] note %lld: reminders and tasks wait for the recording", id)
            requeueFindReminders(id)
        case .run:
            // Too warm: queued, it runs by itself once cooler (the notes are stored already).
            guard await quietHeatGate({ abort.isSet }), !Self.critical else {
                NSLog("[sve] note %lld: reminders and tasks wait for the phone to cool", id)
                requeueFindReminders(id)
                return
            }
            stage = .extracting
            ContinuedWork.shared.update(afterNotes: span.lowerBound)
            let started = Date()
            do {
                let n = try await onBackend {
                    try await engine.extract(noteID: id, shouldStop: { ContinuedWork.beat(); return abort.isSet || Self.critical })
                }
                NSLog("[sve] note %lld: extract after the notes added %d suggestions in %.0f s", id, n,
                      Date().timeIntervalSince(started))
            } catch {
                NSLog("[sve] extract failed: %@", String(describing: error))
                if ExtractAfterNotes.afterStop(stopped: abort.isSet, hot: Self.critical) { requeueFindReminders(id) }
            }
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            ContinuedWork.shared.update(afterNotes: span.upperBound)
        }
    }

    /// False only in a debug build launched with SVE_ONEPASS=0 (device check: the steps without the one pass).
    private static var onePassAllowed: Bool {
        #if DEBUG
        return ProcessInfo.processInfo.environment["SVE_ONEPASS"] != "0"
        #else
        return true
        #endif
    }

    /// The title and the three outputs from one generation, stored as soon as written, with the notices the stepped
    /// path posts. False when the reply did not split or the run was stopped (the caller then writes in steps, or
    /// stops on the abort flag); nothing is stored then.
    private func writeAllInOnePass(noteID id: Int64, transcript: String, lang: String, steps: Int) async -> Bool {
        let engine = CoreEngine.shared
        let abort = self.abort
        let started = Date()
        stage = .generating(.shortSummary, "final", 0, 1)
        let from = NotesProgress.fraction(outputsDone: 1, outputsTotal: steps)
        let to = NotesProgress.fraction(outputsDone: steps, outputsTotal: steps)
        let all: (title: String, summary: String, notes: String, questions: String)?
        do {
            all = try await onBackend {
                try await engine.writeAll(transcript: transcript, lang: lang, shouldStop: { ContinuedWork.beat(); return abort.isSet },
                                          progress: { p in
                    ContinuedWork.beat()
                    Task { @MainActor in ContinuedWork.shared.update(fraction: from + (to - from) * p) }
                })
            }
        } catch {
            if !abort.isSet { NSLog("[sve] note %lld: one pass stopped: %@", id, String(describing: error)) }
            return false
        }
        guard let all else {
            NSLog("[sve] note %lld: one pass did not split; notes in steps", id)
            return false
        }
        guard await engine.note(id) != nil else { NSLog("[sve] note %lld is gone; its notes are dropped", id); return false }
        await engine.setTitle(id, all.title)
        noteTitles[id] = all.title
        await engine.setOutput(id, .shortSummary, all.summary)
        setStreak(id, .notes, nil)
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        let step = partStored(.summary)
        notifyStage(id, NotifyRule.summary) { NotesNotifier.summaryReady(noteID: id, title: $0, step: step, hold: $1) }
        await engine.setOutput(id, .detailedNotes, all.notes)
        await engine.setOutput(id, .revisionQuestions, all.questions)
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        ContinuedWork.shared.update(fraction: to)
        NSLog("[sve] note %lld: notes in one pass, %.0f s", id, Date().timeIntervalSince(started))
        return true
    }

    /// The note's insight, after its notes and memories, on the backend for right now. The model calls back while it
    /// writes, so a long CPU insight keeps the heartbeat going; an expiry stops it (nothing to resume: the notes are
    /// stored). Live transcription running steps it aside (LiveFirst), before it or in the middle (it is cheap to
    /// stop): it then stays due and is written after Stop (insightDue, from the resume loop).
    /// due: the insight was already due (runDueInsight): stopped by an expiry, it stays due.
    private func writeInsight(_ id: Int64, due: Bool = false) async {
        let abort = self.abort, live = LiveTranscriber.shared.liveRunningNow, yielded = AtomicFlag()
        var written = false
        if LiveFirst.yields(recording: live.isSet, runIsLive: false) {
            yielded.set(true)
        } else if !abort.isSet, (try? await ModelManager.shared.loadIntoCore(backend: await stepBackend())) != nil {
            ContinuedWork.beat()   // a CPU reload calls back nothing
            written = await CoreEngine.shared.generateInsight(noteID: id, shouldStop: {
                ContinuedWork.beat()
                if LiveFirst.yields(recording: live.isSet, runIsLive: false) { yielded.set(true); return true }
                return abort.isSet
            }) != nil
        }
        if LiveFirst.insightStaysDue(yielded: yielded.isSet, written: written, stoppedWhileDue: due && abort.isSet) {
            NSLog(yielded.isSet ? "[sve] note %lld steps aside for the recording" : "[sve] bg: note %lld: the due insight was stopped; it stays due", id)
            insightDue.insert(id)
        } else {
            insightDue.remove(id)
        }
    }

    /// A due insight's turn (from the resume loop, after Stop): the insight alone, under the same LiveFirst and heat
    /// rules (the heat ladder's rest; paused leaves it due). No note shows busy and no stage is shown.
    private func runDueInsight(_ id: Int64) async {
        guard !isWorking, ModelManager.shared.isReady else { return }
        starting = true
        defer { starting = false }
        guard await CoreEngine.shared.note(id) != nil else { insightDue.remove(id); return }   // deleted
        beginBackground()
        defer { endBackground() }
        activeNoteID = nil
        notesStored = true   // an expiry only stops it
        let abort = self.abort
        func yields() -> Bool { LiveFirst.yields(recording: LiveTranscriber.shared.liveRunning, runIsLive: false) }
        let heat = HeatGovernor.shared, start = HeatGovernor.now
        while !HeatLadder.restOver(level: heat.level, started: start, now: HeatGovernor.now), !abort.isSet, !yields() {
            do { try await Task.sleep(nanoseconds: 500_000_000) } catch { return }
        }
        guard !abort.isSet, !yields(), heat.level != .paused else { return }   // stays due
        NSLog("[sve] note %lld: insight after the recording", id)
        await writeInsight(id, due: true)
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
    }

    /// The notes' source rule: from the sections only when they cover the whole recording and the transcript does
    /// not fit one prompt; otherwise from the transcript, exactly as before sections.
    private func notesFromSections(covered: Bool, transcript: String, lang: String) async -> Bool {
        guard covered, let fits = try? await onBackend({ await CoreEngine.shared.transcriptFits(transcript, lang: lang) }) else {
            return false
        }
        return fits == false
    }

    /// A title for the note from the same source as its notes; nil when none could be written.
    private func writeTitle(noteID id: Int64, transcript: String, lang: String, fromSections: Bool) async -> String? {
        let engine = CoreEngine.shared
        return try? await onBackend { () async throws -> String in
            if fromSections, let t = await engine.titleFromSections(noteID: id, lang: lang) { return t }
            guard let t = await engine.suggestTitle(transcript: transcript, lang: lang) else { throw CoreError.generationFailed }
            return t
        }
    }

    /// One output (not stored): from the sections when `fromSections`, else (or when the sections cannot be used)
    /// from the transcript. Throws when it fails or is cancelled (the abort flag). fraction: the lock-screen progress
    /// for `part` (0…1) of this output (its place in the notes steps, or in a regenerate's); it also turns on the
    /// heartbeat.
    private func writeOutput(_ kind: OutputKind, noteID id: Int64, transcript: String, lang: String,
                             fromSections: Bool, fraction: @escaping (Double) -> Double) async throws -> String {
        let engine = CoreEngine.shared
        let abort = self.abort
        stage = .generating(kind, "final", 0, 1)
        // Every few tokens (core thread): the stage line, the progress inside the output, and the heartbeat. The core
        // reports whole chunks only, so the callbacks seen in the chunk in flight estimate how far it is
        // (OutputProgress); without that the lock-screen progress stood still for a whole output.
        let counter = CallbackCounter()
        let expected = OutputProgress.expectedTokens(kind: kind.rawValue)
        let progress: (String, Int, Int) -> Void = { phase, done, total in
            ContinuedWork.beat()
            let seen = counter.next(key: "\(phase).\(done)")
            let part = OutputProgress.part(done: done, total: total, callbacks: seen, expectedTokens: expected)
            Task { @MainActor in
                self.stage = .generating(kind, phase, done, total)
                ContinuedWork.shared.update(fraction: fraction(part))
            }
        }
        return try await onBackend { () async throws -> String in
            if fromSections, let t = await engine.generateFromSections(noteID: id, kind: kind, lang: lang, shouldStop: { abort.isSet },
                                                                       onProgress: progress) { return t }
            // nil: cancelled (our flag) or the sections could not be used, then the transcript as before.
            if abort.isSet { throw CoreError.generationFailed }
            guard await engine.note(id) != nil else { throw CoreError.notFound }   // deleted meanwhile: nothing to write
            return try await engine.generate(transcript: transcript, kind: kind, lang: lang, shouldStop: { abort.isSet }, onProgress: progress)
        }
    }

    // MARK: notes in small saved steps

    /// Notes whose parts that were left ("I couldn't write part %d") are tried again by the next run of the note,
    /// even though its outputs are stored: only those parts, then the notes are put together again. On disk, so a
    /// cut-off or a kill in the middle still finishes them; removed once that run puts the notes together.
    private var partsRetry: Set<Int64> {
        get { Set((UserDefaults.standard.array(forKey: "partsRetry") as? [NSNumber] ?? []).map(\.int64Value)) }
        set { UserDefaults.standard.set(newValue.sorted().map { NSNumber(value: $0) }, forKey: "partsRetry") }
    }

    /// Notes-first notes whose section summaries were stopped after their notes were stored (NotesOrder): their
    /// sections, then their memories, are finished by the memory backfill (idle in front, overnight), or when the
    /// note is opened (backfillSections). On disk, so a kill keeps them; removed once the sections cover the note.
    private var sectionsDue: Set<Int64> {
        get { Set((UserDefaults.standard.array(forKey: Self.sectionsDueKey) as? [NSNumber] ?? []).map(\.int64Value)) }
        set { UserDefaults.standard.set(newValue.sorted().map { NSNumber(value: $0) }, forKey: Self.sectionsDueKey) }
    }
    nonisolated private static let sectionsDueKey = "sectionsDue"

    /// Tried once per launch by the backfill (a note whose sections keep failing waits for the next launch).
    @ObservationIgnored private var sectionsTried: Set<Int64> = []

    /// A notes-first note's sections were stopped after its notes: finished later (sectionsDue).
    private func markSectionsDue(_ id: Int64) {
        sectionsDue.insert(id)
        memoriesMayBeDue = true   // the overnight task is asked for while they are left
    }

    /// Sections due that the backfill may take in this launch, newest first.
    private var sectionsDueNow: [Int64] { sectionsDue.subtracting(sectionsTried).sorted(by: >) }

    /// Under a part's heading while that part is not written.
    static var notWrittenYet: String { String(localized: "(not written yet)") }

    /// A note's line for a part that failed 3 times for a real reason.
    static func partLeftLine(_ part: Int) -> String { String(localized: "I couldn't write part \(part). Try again") }

    /// 0…1 of the note's steps (summary, parts, assembles) as the lock-screen progress: the share of the three outputs.
    private static func stepsFraction(_ p: Double) -> Double {
        let span = notesSteps - firstOutputStep
        let x = min(1, max(0, p)) * Double(span)
        let whole = min(Int(x), span)
        return NotesProgress.fraction(outputsDone: firstOutputStep + whole, part: x - Double(whole), outputsTotal: notesSteps)
    }

    /// How a run of part steps ended.
    private enum PartsEnd: Equatable {
        /// No part step left (parts that failed 3 times for a real reason are left out).
        case done
        /// Cut off (an expiry, the overnight task ended, Stop): the running part is dropped, nothing stored.
        case cutOff
        /// The heat gate stopped it (it set the stage and queued the note).
        case paused
        /// The note was deleted meanwhile: the run ends quietly.
        case gone
        /// A part stopped again and again this run without the core counting a real failure (a GPU in front): the
        /// run stops, and the failure rules decide (FailureKind).
        case failed
    }

    /// The summary, then each section's notes and questions, then the two outputs put together, each step stored
    /// before the next (NoteSteps.plan, from what is stored). False: the run stopped (the caller returns). refill:
    /// a Try again on parts that were left, so both outputs are put together again even though they are stored.
    private func writeInSteps(noteID id: Int64, lang: String, material: String, toWrite: [Bool], refill: Bool) async -> Bool {
        let engine = CoreEngine.shared
        let sections = await engine.sections(id).count
        let state = await engine.sectionOutputsState(noteID: id, lang: lang) ?? []
        // Parts only of the outputs this run puts together (a resumed run with the notes stored writes no notes parts).
        let kinds = [NoteSteps.Kind.notes, .questions].filter { toWrite[$0.outputKind.rawValue] || refill }
        let plan = NoteSteps.plan(sections: sections, state: state, hasSummary: !toWrite[0])
        let total = 1 + kinds.count * (sections + 1)   // the summary, each kind's parts and its putting together
        let stepsLeft = (toWrite[0] ? 1 : 0) + kinds.map { NoteSteps.parts(plan, kind: $0).count + 1 }.reduce(0, +)
        var done = total - stepsLeft
        ContinuedWork.shared.update(fraction: Self.stepsFraction(NoteSteps.progress(done: done, total: total)))
        NSLog("[sve] note %lld: notes in steps: %ld parts, %ld of %ld steps done", id, sections, done, total)

        // 1. The short summary, from the section summaries.
        if toWrite[OutputKind.shortSummary.rawValue] {
            if abort.isSet { stopShort(CoreError.generationFailed.localizedDescription); return false }   // cut off
            if !(await heatGate(id)) { return false }
            do {
                let step = done
                let text = try await writeOutput(.shortSummary, noteID: id, transcript: material, lang: lang, fromSections: true,
                                                 fraction: { Self.stepsFraction((Double(step) + $0) / Double(total)) })
                guard await engine.note(id) != nil else { NSLog("[sve] note %lld is gone; its summary is dropped", id); stage = .idle; return false }
                await engine.setOutput(id, .shortSummary, text)
                setStreak(id, .notes, nil)
                NotificationCenter.default.post(name: .notesChanged, object: nil)
                done += 1
                ContinuedWork.shared.update(fraction: Self.stepsFraction(NoteSteps.progress(done: done, total: total)))
                NSLog("[sve] note %lld: step %ld of %ld: summary stored", id, done, total)
                let n = partStored(.summary)
                notifyStage(id, NotifyRule.summary) { NotesNotifier.summaryReady(noteID: id, title: $0, step: n, hold: $1) }
            } catch {
                stopShort(error.localizedDescription, error: error)
                return false
            }
        }

        // 2. Each section's notes, then its questions.
        switch await writeParts(noteID: id, lang: lang, kinds: kinds, done: done, total: total,
                                fraction: { Self.stepsFraction($0) }) {
        case .done: break
        case .cutOff: stopShort(CoreError.generationFailed.localizedDescription); return false
        case .paused: return false
        case .gone: stage = .idle; return false
        case .failed: stopShort(CoreError.generationFailed.localizedDescription, error: CoreError.generationFailed); return false
        }

        // 3. Put together (no model): a part that was left shows its heading and "(not written yet)".
        let now = await engine.sectionOutputsState(noteID: id, lang: lang) ?? []
        var missing: [NoteSteps.Kind] = []
        for kind in kinds {
            guard let text = await engine.assembleOutput(noteID: id, kind: kind, lang: lang, placeholder: Self.notWrittenYet) else {
                NSLog("[sve] note %lld: no %@ part written; nothing to put together", id, kind.outputKind.title)
                missing.append(kind)
                continue
            }
            guard await engine.note(id) != nil else { NSLog("[sve] note %lld is gone; nothing put together", id); stage = .idle; return false }
            await engine.setOutput(id, kind.outputKind, text)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            NSLog("[sve] note %lld: step: %@ put together (%ld parts left for Try again)", id, kind.outputKind.title,
                  NoteSteps.gaveUpParts(now, kind: kind).count)
        }
        ContinuedWork.shared.update(fraction: Self.stepsFraction(1))
        let left = NoteSteps.gaveUpParts(now)
        if !missing.isEmpty, let first = left.first {
            // Every part of an output failed 3 times for a real reason: a real failure, with Try again.
            let line = Self.partLeftLine(first)
            NSLog("[sve] bg: note %lld failed: no part of an output could be written", id)
            partsRetry.remove(id)   // a real failure: the person's Try again marks it again
            stage = .failed(line)   // sets the note's failure line and posts the failed notification
            return false
        }
        if !missing.isEmpty {   // nothing to put together and no part left: not expected; the failure rules decide
            stopShort(CoreError.generationFailed.localizedDescription, error: CoreError.generationFailed)
            return false
        }
        return true
    }

    /// The part steps still to do of `kinds`, one at a time, each stored by the core before the next. The plan is
    /// read again from the store after every step, so a part that failed is tried again (the core counts real
    /// failures; after 3 it is left) and sections re-planned meanwhile are followed. done/total: the steps for
    /// `fraction` (0…1 of them, to the lock-screen progress).
    private func writeParts(noteID id: Int64, lang: String, kinds: [NoteSteps.Kind], done start: Int, total: Int,
                            fraction: @escaping (Double) -> Double) async -> PartsEnd {
        let engine = CoreEngine.shared, abort = self.abort
        var done = start
        var tries: [String: Int] = [:]   // this run's tries of each part (a cut-off is not counted by the core)
        while true {
            if abort.isSet { return .cutOff }
            guard await engine.note(id) != nil else { NSLog("[sve] note %lld is gone; its parts stop", id); return .gone }
            let count = await engine.sections(id).count
            guard let state = await engine.sectionOutputsState(noteID: id, lang: lang) else { return .failed }
            let plan = NoteSteps.plan(sections: count, state: state, hasSummary: true)
            let next = plan.first {
                if case .part(_, let k) = $0 { return kinds.contains(k) }
                return false
            }
            guard case .part(let idx, let kind) = next else { return .done }
            let key = "\(idx).\(kind.rawValue)", tried = tries[key, default: 0]
            guard tried < NoteSteps.maxAttempts else {
                NSLog("[sve] note %lld: part %ld %@ stopped %ld times this run; the run stops", id, idx + 1, kind.outputKind.title, tried)
                return .failed
            }
            let qKey = "\(idx).\(NoteSteps.Kind.questions.rawValue)"
            let bothDue = kind == .notes && kinds.contains(.questions) && plan.contains(.part(idx: idx, kind: .questions)) &&
                tries[qKey, default: 0] < NoteSteps.maxAttempts
            tries[key] = tried + 1
            if !(await heatGate(id)) { return .paused }
            if abort.isSet { return .cutOff }
            // Both kinds of this part due: written together, sharing one reading of the part, only with pair writing
            // on and the heat at full or eased (the ladder stays in charge: minimal or paused means single calls).
            let level = HeatGovernor.shared.level
            let pair = bothDue && CoreEngine.pairWriting && (level == .full || level == .eased)
            if pair { tries[qKey, default: 0] += 1 }
            stage = .generating(kind.outputKind, "part", idx, count)
            NSLog("[sve] note %lld: step %ld of %ld: part %ld of %ld, %@ (try %ld this run)", id, done + 1, total, idx + 1, count,
                  pair ? "notes and questions" : kind.outputKind.title, tried + 1)
            let steps = pair ? 2 : 1
            let step = done, counter = CallbackCounter(), expected = kind == .notes ? 440 : 300
            let progress: (String, Int, Int) -> Void = { phase, d, t in
                ContinuedWork.beat()
                let seen = counter.next(key: "\(phase).\(d)")
                let part = OutputProgress.part(done: d, total: t, callbacks: seen, expectedTokens: expected)
                Task { @MainActor in
                    ContinuedWork.shared.update(fraction: fraction((Double(step) + Double(steps) * part) / Double(max(total, 1))))
                }
            }
            let started = Date()
            let what = pair ? "notes and questions" : kind.outputKind.title
            do {
                if pair {
                    let stored = try await onBackend {
                        try await engine.sectionOutputsPair(noteID: id, idx: idx, lang: lang, shouldStop: { abort.isSet },
                                                            onProgress: progress)
                    }
                    done += stored
                    if Self.timingLogs {
                        NSLog("[sve] timing: pair section %ld %.1f s", idx + 1, Date().timeIntervalSince(started))
                    }
                    NSLog("[sve] note %lld: part %ld notes and questions: %ld of 2 stored", id, idx + 1, stored)
                } else {
                    let text = try await onBackend {
                        try await engine.sectionOutput(noteID: id, idx: idx, kind: kind, lang: lang, shouldStop: { abort.isSet },
                                                       onProgress: progress)
                    }
                    done += 1
                    if Self.timingLogs {
                        NSLog("[sve] timing: single section %ld %@ %.1f s", idx + 1, kind.outputKind.title,
                              Date().timeIntervalSince(started))
                    }
                    NSLog("[sve] note %lld: part %ld %@ stored (%ld characters)", id, idx + 1, kind.outputKind.title, text.count)
                }
                ContinuedWork.shared.update(fraction: fraction(NoteSteps.progress(done: done, total: total)))
            } catch {
                if abort.isSet {
                    NSLog("[sve] bg: note %lld: part %ld %@ cut off; what was not stored is written next run", id, idx + 1, what)
                    return .cutOff
                }
                NSLog("[sve] note %lld: part %ld %@ not written (%@); read the store again", id, idx + 1, what,
                      String(describing: error))
            }
        }
    }

    /// SVE_TIMING=1 (in the scheme's environment): per-part seconds in the log, pair against single.
    private static let timingLogs = ProcessInfo.processInfo.environment["SVE_TIMING"] == "1"

    /// After a language switch the section summaries (the part headings) are still in the old language: they are
    /// written again in `lang` (one summary per section), which also clears the parts written for the old ranges.
    private func rewriteSectionsIfOtherLanguage(_ id: Int64, lang: String) async {
        let engine = CoreEngine.shared
        let secs = await engine.sections(id)
        guard !secs.isEmpty else { return }
        let other = lang == "te" ? "en" : "te"
        let here = await engine.sectionOutputsState(noteID: id, lang: lang)?.filter(\.done).count ?? 0
        let there = await engine.sectionOutputsState(noteID: id, lang: other)?.filter(\.done).count ?? 0
        let key = Self.rewriteMarkKey(id), mark = UserDefaults.standard.string(forKey: key)
        // Being written again for this language already (a run cut off in the middle): the missing ones follow.
        guard mark != Self.rewritePendingMark(lang) else { return }
        guard NoteSteps.rewritesSections(summaryTexts: secs.flatMap { [$0.summary.title, $0.summary.topic] }, lang: lang,
                                         doneHere: here, doneInOther: there, mark: mark,
                                         current: Self.rewriteMark(secs, lang: lang)) else { return }
        NSLog("[sve] note %lld: the part headings read as another language; the sections are written again in %@", id, lang)
        await engine.setSections(id, [])
        // Matches no sections; marked for real once they are written again (markRewrittenSections). On disk, so a
        // run cut off or killed in between still marks them when it finishes.
        UserDefaults.standard.set(Self.rewritePendingMark(lang), forKey: key)
    }

    private static func rewritePendingMark(_ lang: String) -> String { lang + "|" }

    /// Settings key of a note's "sections written again for lang" mark (NoteSteps.rewriteMark).
    nonisolated private static func rewriteMarkKey(_ id: Int64) -> String { "sections.rewritten.\(id)" }

    private static func rewriteMark(_ secs: [NoteSection], lang: String) -> String {
        NoteSteps.rewriteMark(lang: lang, ranges: secs.map { ($0.firstPiece, $0.lastPiece) })
    }

    /// After writeSections: the sections written again for a language switch now cover the recording, so they are
    /// marked (once per switch; a later change of the sections makes the mark not hold).
    private func markRewrittenSections(_ id: Int64, lang: String) async {
        let key = Self.rewriteMarkKey(id)
        guard UserDefaults.standard.string(forKey: key) == Self.rewritePendingMark(lang) else { return }
        UserDefaults.standard.set(Self.rewriteMark(await CoreEngine.shared.sections(id), lang: lang), forKey: key)
    }

    /// Section summaries that failed in a row for a note (a stop each time; the third sends it to the transcript).
    private static func sectionFailures(_ id: Int64) -> Int { UserDefaults.standard.integer(forKey: "sectionFailures.\(id)") }
    private static func setSectionFailures(_ id: Int64, _ n: Int) {
        if n == 0 { UserDefaults.standard.removeObject(forKey: "sectionFailures.\(id)") }
        else { UserDefaults.standard.set(n, forKey: "sectionFailures.\(id)") }
    }

    /// A note deleted: its section-failure count and its rewrite mark go with it.
    nonisolated static func forgetNoteMarks(_ id: Int64) {
        UserDefaults.standard.removeObject(forKey: "sectionFailures.\(id)")
        UserDefaults.standard.removeObject(forKey: rewriteMarkKey(id))
        let retry = (UserDefaults.standard.array(forKey: "partsRetry") as? [NSNumber] ?? []).filter { $0.int64Value != id }
        UserDefaults.standard.set(retry, forKey: "partsRetry")
        let due = (UserDefaults.standard.array(forKey: sectionsDueKey) as? [NSNumber] ?? []).filter { $0.int64Value != id }
        UserDefaults.standard.set(due, forKey: sectionsDueKey)
    }

    /// The parts of a note that failed 3 times for a real reason, in either notes language, cleared so the next run
    /// tries them again. True when there were any.
    private func clearPartsLeft(_ id: Int64) async -> Bool {
        let engine = CoreEngine.shared
        var cleared = false
        for lang in ["en", "te"] {
            for s in (await engine.sectionOutputsState(noteID: id, lang: lang) ?? []) where NoteSteps.gaveUp(s) {
                await engine.clearSectionOutputs(noteID: id, kind: s.kind, idx: s.idx)
                cleared = true
            }
        }
        return cleared
    }

    /// Parts of a note that were left, in the note's language, for its "I couldn't write part %d. Try again" lines:
    /// kind nil, either kind.
    nonisolated static func partsLeft(noteID: Int64, lang: String, kind: NoteSteps.Kind?) async -> [Int] {
        NoteSteps.gaveUpParts(await CoreEngine.shared.sectionOutputsState(noteID: noteID, lang: lang) ?? [], kind: kind)
    }

    /// Writes the note's missing section summaries from its stored pieces, one at a time, each stored before the
    /// next (a note deleted meanwhile refuses the write, which ends this). nil: stopped, the caller returns —
    /// paused for heat (critical), cut off, or the note is gone; quiet: a recording started, the screen was
    /// left, or the phone got warm, and nothing is shown or queued. Otherwise whether the sections now cover
    /// every piece (false after a failure: the notes then come from the transcript, as before sections).
    /// quiet (backfill when an older note is opened): no failure banner, no pause queue, stops at critical.
    /// afterNotes (with quiet; a notes-first note's own run, its notes stored): the heartbeat and the lock-screen
    /// progress go on in this span of what follows the notes, and the person asking for something stops it, as it
    /// stops the memories (memoryWork, yieldMemories).
    private func writeSections(noteID id: Int64, lang: String, quiet: Bool, afterNotes: ClosedRange<Double>? = nil) async -> Bool? {
        let engine = CoreEngine.shared
        let pieces = await engine.pieces(id)
        guard !pieces.isEmpty else { sectionsDue.remove(id); return false }   // nothing to write sections from
        let existing = await engine.sections(id)
        if SectionWriter.coversAll(existing, pieceCount: pieces.count) { sectionsDue.remove(id); return true }
        let duration = await engine.note(id)?.duration_seconds ?? 0
        let from = NoteSection.covered(NoteSection.validPrefix(existing, pieceCount: pieces.count))
        let total = SectionWriter.plan(pieces: Array(pieces[from...]), endSec: duration, isFinal: true).count
        let abort = self.abort, recording = LiveTranscriber.shared.recordingNow, memoryStop = self.memoryStop
        let afterNotesRun = afterNotes != nil, beats = !quiet || afterNotesRun
        let yields = quiet && (afterNotesRun || memoryWork)   // with the memories: stops when the person asks
        var written = 0, gone = false, stopped = false
        // Inside a note's own run (after its notes) the stage stays a working one until that run ends (.done): an
        // idle stage would let other model work start while its memories and insight are still to run.
        func idleUnlessInRun() { if !afterNotesRun { stage = .idle } }
        let wasMemoryWork = memoryWork
        if afterNotesRun { memoryWork = true }   // after the notes: the person's action goes first
        defer { memoryWork = wasMemoryWork }
        /// The lock-screen progress inside the sections step: one share per section.
        func reportSections() {
            let part = Double(written) / Double(max(total, 1))
            if let span = afterNotes {
                ContinuedWork.shared.update(afterNotes: span.lowerBound + (span.upperBound - span.lowerBound) * part)
            } else if !quiet {
                ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: Self.sectionsStep, part: part, outputsTotal: Self.notesSteps))
            }
        }
        func stop() -> Bool {
            abort.isSet || (quiet && LiveTranscriber.shared.isRecording) || Task.isCancelled || (yields && memoryStop.isSet)
        }
        do {
            let done = try await SectionWriter.writeDue(
                pieces: pieces, existing: existing, isFinal: true, lang: lang, endSec: duration,
                beforeEach: {
                    if stop() { stopped = true; return false }
                    if quiet {
                        if !(await quietHeatGate(stop)) { stopped = true; return false }
                    } else if !(await heatGate(id)) {
                        stopped = true; return false
                    }
                    stage = .sections(written, total)
                    reportSections()
                    return true
                },
                summarize: { ps, lang in
                    try await onBackend {
                        try await engine.sectionSummarize(pieces: ps, lang: lang, shouldStop: {
                            if beats { ContinuedWork.beat() }
                            return abort.isSet || Self.critical || (quiet && recording.isSet) || (yields && memoryStop.isSet)
                        })
                    }
                },
                onSection: { secs in
                    guard await engine.setSections(id, secs) else { gone = true; return false }
                    written += 1
                    reportSections()
                    NotificationCenter.default.post(name: .notesChanged, object: nil)   // chapters appear as they come
                    return true
                })
            if gone {
                NSLog("[sve] note %lld is gone; not writing its sections", id)
                idleUnlessInRun()
                return nil
            }
            if stopped {
                if quiet {
                    idleUnlessInRun()
                } else if case .paused = stage {
                    // paused for heat: heatGate set the stage and queued the note
                } else if parked.contains(id) {
                    // stopped for the night: heatGate set the stage
                } else if runSteppedAside {
                    // stepped aside for a recording: heatGate set the stage and queued the note
                } else {
                    stopShort(CoreError.generationFailed.localizedDescription)   // cut off (abort: resumed later)
                }
                return nil
            }
            let covers = SectionWriter.coversAll(done, pieceCount: pieces.count)
            if covers {
                Self.setSectionFailures(id, 0)
                sectionsDue.remove(id)
            }
            return covers
        } catch {
            if quiet {
                NSLog("[sve] section backfill stopped: %@", String(describing: error))
                idleUnlessInRun()
                return nil
            }
            if abort.isSet { stopShort(CoreError.generationFailed.localizedDescription, error: error); return nil }
            if pausedForHeat(id) { return nil }
            // One failed section summary is a stop (the failure rules decide: queued again, or Try again); only a
            // section that failed 3 times for a real reason sends the note to the whole-transcript path. A GPU run
            // stopped in the background is a cut-off and is not counted.
            let gpuCutOff = BackendChoice.failureIsCutOff(aborted: false, wentToBackground: wentToBackground || !appIsActive,
                                                          gpuStep: stepOnGPU, continuedTaskHeld: ContinuedWork.shared.isHeld)
            if gpuCutOff {
                NSLog("[sve] section summary cut off on the GPU in the background; not counted: %@", String(describing: error))
                stopShort(error.localizedDescription, error: error)
                return nil
            }
            let fails = Self.sectionFailures(id) + 1
            Self.setSectionFailures(id, fails)
            guard fails >= NoteSteps.maxAttempts else {
                NSLog("[sve] section summary failed (%ld of %ld): %@", fails, NoteSteps.maxAttempts, String(describing: error))
                stopShort(error.localizedDescription, error: error)
                return nil
            }
            NSLog("[sve] section summaries failed %ld times, notes come from the transcript: %@", fails, String(describing: error))
            Self.setSectionFailures(id, 0)   // the transcript path is chosen: the count starts again
            return false
        }
    }

    /// The heat rule for quiet work: the ladder's rest (cut short by `stop` or a cooler phone); paused stops.
    private func quietHeatGate(_ stop: () -> Bool) async -> Bool {
        let heat = HeatGovernor.shared
        let level = heat.level
        if level == .paused { return false }
        guard HeatLadder.showsCooling(level) else { return true }
        stage = .cooling
        let start = HeatGovernor.now
        while !HeatLadder.restOver(level: heat.level, started: start, now: HeatGovernor.now), !stop() {
            do { try await Task.sleep(nanoseconds: 500_000_000) } catch { return false }
        }
        return !stop() && heat.level != .paused
    }

    /// An older note opened (made before sections, or its sections were cut off) that already has all its
    /// notes: writes its section summaries from the stored pieces so it gets chapters. Only while nothing else
    /// runs, the app is in front, nothing is recording and the AI is ready; stops quietly when a recording
    /// starts, the phone gets too hot or the caller's task is cancelled (the screen was left), keeping every
    /// finished section. A note that still needs its notes gets its sections from regenerate instead.
    func backfillSections(noteID: Int64) async {
        guard !isWorking, appIsActive, ModelManager.shared.isReady, !LiveTranscriber.shared.isRecording,
              !interrupted.contains(noteID), HeatGovernor.shared.level != .paused else { return }
        starting = true
        defer { starting = false }
        let engine = CoreEngine.shared
        guard let note = await engine.note(noteID), let t = note.transcript,
              !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !WaitingNotes.needsWork(hasTranscript: true, outputs: OutputKind.allCases.map { note.output($0) != nil }) else { return }
        if await engine.pieces(noteID).isEmpty {   // note from before pieces existed
            await engine.setPieces(noteID, PiecesBackfill.pieces(transcript: t, durationSeconds: note.duration_seconds))
        }
        let pieces = await engine.pieces(noteID)
        guard !pieces.isEmpty, !SectionWriter.coversAll(await engine.sections(noteID), pieceCount: pieces.count),
              !Task.isCancelled, !LiveTranscriber.shared.isRecording else { return }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        do {
            try await loadSelectedModel()
        } catch {
            NSLog("[sve] section backfill: model not loaded: %@", String(describing: error))
            stage = .idle
            return
        }
        // In the language of the notes it already has.
        guard await writeSections(noteID: noteID, lang: note.lang, quiet: true) != nil else { return }
        stage = .idle
    }

    // MARK: memories

    /// Writes the memories of the note's sections that do not have them yet (sve_memories_from_section), one section
    /// at a time, each kept before the next, each after the heat rule for quiet work (the ladder's rest,
    /// paused stops), on the backend for right now. Memories never hold up notes and never fail them: before each section
    /// it stops when a recording runs, another note's notes may start, the run was cut off, or the person chose
    /// overnight for this note; a recording starting, critical heat or the cut-off cancels the section in flight
    /// (nothing of it is stored; it stays due). It never sets a failure, pauses or queues the note.
    /// quiet: a backfill, not the end of the note's own run (the stage goes back to idle when it ends).
    /// Returns .finished when every section has its memories; .gone when the note is gone (deleted meanwhile: the
    /// run ends quietly); .failed when a section's model call failed (-3: the note is skipped for the rest of this
    /// launch, other notes go on); .stopped for a real stop, or no model (the sections left are done later).
    @discardableResult
    /// span: where the memories sit in what follows the notes on the lock-screen progress (the note's own run).
    private func writeMemories(noteID id: Int64, quiet: Bool, span: ClosedRange<Double> = 0...0.5) async -> MemoryPass.Outcome {
        let engine = CoreEngine.shared
        let due = await engine.memorySectionsDue(id)
        guard !due.isEmpty else { return .finished }
        let wasMemoryWork = memoryWork
        memoryWork = true
        defer {
            memoryWork = wasMemoryWork
            if quiet { stage = .idle }
        }
        func stop() -> Bool {
            abort.isSet || memoryStop.isSet || LiveTranscriber.shared.isRecording || Task.isCancelled || parkRequested.contains(id)
                || (overnightRun?.isSet ?? false) || nextToResume(tried: [id]) != nil   // another note's notes first
        }
        var finished = 0
        stage = .memories(0, due.count)
        let (outcome, _) = await MemoryPass.run(
            due: due,
            beforeEach: { _ in
                guard !stop(), await quietHeatGate(stop) else { return false }
                stage = .memories(finished, due.count)
                return true
            },
            write: { idx in await memorySection(noteID: id, idx: idx) },
            afterEach: { done, _ in
                finished = done
                stage = .memories(done, due.count)
                // The note's own run: its memories are the first half of what follows its notes on the task.
                if !quiet {
                    let part = Double(done) / Double(due.count)
                    ContinuedWork.shared.update(afterNotes: span.lowerBound + (span.upperBound - span.lowerBound) * part)
                }
                NotificationCenter.default.post(name: .assistantChanged, object: nil)
            })
        switch outcome {
        case .finished:
            memoryFailed.remove(id)
            return .finished
        case .stopped:
            NSLog("[sve] note %lld: memories stopped after %ld of %ld sections; the rest wait", id, finished, due.count)
            memoriesMayBeDue = true
            return .stopped
        case .failed:
            NSLog("[sve] note %lld: the memory call failed; skipped until the next launch", id)
            memoryFailed.insert(id)
            return .failed
        case .gone:
            if await engine.note(id) == nil {
                NSLog("[sve] note %lld is gone; not writing its memories", id)
                return .gone
            }
            NSLog("[sve] note %lld: memories could not be written now", id)
            memoriesMayBeDue = true
            return .stopped
        }
    }

    /// One section's memories, on the GPU while the app is active and nothing records, else the CPU (picked right
    /// before the run). A GPU run is cancelled as soon as the app stops being active, and tried once more on the
    /// CPU while a continued-processing or the overnight task keeps the app running; otherwise it stops.
    private func memorySection(noteID id: Int64, idx: Int) async -> MemoryPass.Step {
        let engine = CoreEngine.shared
        let abort = self.abort, recording = LiveTranscriber.shared.recordingNow, leftFront = self.leftFront
        let memoryStop = self.memoryStop
        var retried = false
        while true {
            let choice = retried ? (backend: ModelBackend.cpu, why: StepBackend.Why.background, current: ModelBackend?.none)
                : await stepChoice()
            var backend = choice.backend, why = choice.why
            if backend == .gpu {
                leftFront.set(false)
                if !appIsActive { backend = .cpu; why = .background }   // left the front just now
            }
            // Logged after the override, so the line says where the memories really run.
            if let line = StepBackend.moveLine(from: choice.current, to: backend, why: why) { NSLog("[sve] %@", line) }
            let gpu = backend == .gpu
            do {
                try await ModelManager.shared.loadIntoCore(backend: backend)
                let n = try await engine.memoriesFromSection(noteID: id, idx: idx, backend: backend, shouldStop: {
                    abort.isSet || memoryStop.isSet || Self.critical || recording.isSet || (gpu && leftFront.isSet)
                })
                NSLog("[sve] note %lld section %ld: %ld memories", id, idx, n)
                return .stored(n)
            } catch CoreEngine.MemoryError.unavailable {
                return .gone
            } catch let e as CoreEngine.MemoryError {
                // Cancelled (-2) or the model call failed (-3): nothing stored, the section stays due.
                if gpu, leftFront.isSet,
                   BackendChoice.retryOnCPU(failedOn: .gpu, wentToBackground: true,
                                            continuedTaskHeld: ContinuedWork.shared.isHeld || overnightRun != nil,
                                            cancelled: abort.isSet || memoryStop.isSet || recording.isSet || Self.critical,
                                            alreadyRetried: retried) {
                    NSLog("[sve] memories: the app left the front; section %ld again on the CPU", idx)
                    retried = true
                    continue
                }
                // A -3 while stopping (left the front on the GPU, cut off, the person, a recording, critical heat) is a
                // stop, not a failing note: it never lands in memoryFailed.
                let step = MemoryPass.stepAfterError(
                    modelFailed: e == .modelFailed,
                    stopping: (gpu && leftFront.isSet) || abort.isSet || memoryStop.isSet || recording.isSet || Self.critical)
                if step == .failed { NSLog("[sve] memories: the model call failed for section %ld; it stays due", idx) }
                return step
            } catch {
                NSLog("[sve] memories: model not ready: %@", String(describing: error))
                return .cancelled   // stops; the section stays due
            }
        }
    }

    /// Memories for notes that still need them (made before memories existed, or stopped part-way), newest first:
    /// one note per pass in front (the next idle pass goes on), up to 20 in the overnight task. Only while nothing
    /// else runs and no note's notes may start, the AI is ready, nothing records, the phone is not too hot, and the
    /// app is in front or the overnight task runs. Quiet: stops (keeping every finished section) when a recording
    /// starts, a note's notes may start, the phone gets too hot, the run is cut off, or the person asks for something
    /// (yieldMemories). Never keeps the screen on.
    func backfillMemories() async {
        guard !isWorking, !resuming, !memoriesHeld, ModelManager.shared.isReady, !LiveTranscriber.shared.isRecording,
              appIsActive || overnightRun != nil, !(overnightRun?.isSet ?? false),
              HeatGovernor.shared.level != .paused, nextToResume(tried: []) == nil else { return }
        starting = true
        memoryWork = true
        memoryStop.set(false)
        defer { starting = false; memoryWork = false }
        let limit = PersonFirst.backfillNotes(inFront: overnightRun == nil)
        let needing = await CoreEngine.shared.notesNeedingMemories(limit: limit + memoryFailed.count)
        let ids = MemoryPass.backfillNotes(needing, skipping: memoryFailed, limit: limit)
        let sectionNotes = Array(sectionsDueNow.prefix(limit))
        guard !ids.isEmpty || !sectionNotes.isEmpty else { memoriesMayBeDue = false; return }
        guard !LiveTranscriber.shared.isRecording, !Task.isCancelled, !memoryStop.isSet else { return }
        beginBackground()
        ScreenAwake.pipeline(wants: false)   // background work: the phone may lock (the GPU run stops)
        defer { endBackground() }
        // First the sections stopped after a notes-first note's notes (sectionsDue), each note's memories after them.
        for id in sectionNotes {
            guard await finishDueSections(id) else { stage = .idle; return }
        }
        // Still one note per pass in front: a note whose sections were written takes a memory note's place.
        let memoryIDs = Array(ids.prefix(max(0, limit - sectionNotes.count)))
        var allDone = memoryIDs.count == ids.count && ids.count < limit && sectionsDueNow.isEmpty
        for id in memoryIDs {
            activeNoteID = id
            // A failed note is skipped (memoryFailed) and the next goes on; a real stop ends the backfill.
            guard MemoryPass.backfillGoesOn(after: await writeMemories(noteID: id, quiet: true)) else { allDone = false; break }
        }
        if allDone { memoriesMayBeDue = false }
        stage = .idle
    }

    /// A notes-first note's sections left after its notes (sectionsDue), then its memories, quietly in the memory
    /// backfill. False when the backfill should stop (a stop; nothing is lost, the rest stays due). A note whose
    /// sections fail for a real reason is not tried again in this launch.
    private func finishDueSections(_ id: Int64) async -> Bool {
        let engine = CoreEngine.shared
        guard let note = await engine.note(id) else { sectionsDue.remove(id); return true }   // deleted
        activeNoteID = id
        NSLog("[sve] note %lld: the sections left after its notes are written now", id)
        guard await writeSections(noteID: id, lang: note.lang, quiet: true) != nil else {
            let stopped = abort.isSet || memoryStop.isSet || LiveTranscriber.shared.isRecording || Task.isCancelled
                || Self.critical || HeatGovernor.shared.level == .paused
            if await engine.note(id) == nil { sectionsDue.remove(id); return true }
            if !stopped {
                NSLog("[sve] note %lld: its sections could not be written; tried again next launch", id)
                sectionsTried.insert(id)
                return true
            }
            return false
        }
        if sectionsDue.contains(id) { sectionsTried.insert(id) }   // ran through without covering: next launch
        return MemoryPass.backfillGoesOn(after: await writeMemories(noteID: id, quiet: true))
    }

    /// The person asked for something (Ask, Regenerate, Try again, Generate notes, a topic summary): memory work
    /// stops at once (the section in flight is cancelled, stores nothing and stays due) and the run it belongs to
    /// is waited for, at most 30 s, so the action then goes ahead. Nothing happens for any other work. False only
    /// when it waited and the work had not ended after 30 s (e.g. a model reload that cannot be cancelled): callers
    /// then show their busy notice. Callers still check `isWorking` themselves before their model call.
    @discardableResult
    func yieldMemories() async -> Bool {
        guard PersonFirst.action(working: isWorking, memoriesOnly: memoryWork) == .stopMemoriesThenStart else { return true }
        NSLog("[sve] the person asked for something: memories stop")
        memoryStop.set(true)
        let end = Date().addingTimeInterval(30)
        while isWorking, Date() < end {
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        return !isWorking
    }

    /// Before a person's action on a note: memory work yields. False when other work still runs: the tap is then
    /// queued (not while the AI is not ready: the note waits for the AI as before) and runs by itself in its turn.
    private func personMayStart(_ tap: QueuedTap) async -> Bool {
        await yieldMemories()
        guard isWorking else { return true }
        // A regenerate is queued also while the AI is not ready (it runs once it is); other taps wait with the note.
        var regenerate = false
        if case .regenerate = tap.kind { regenerate = true }
        if ModelManager.shared.isReady || regenerate { enqueueTap(tap) }
        return false
    }

    /// "Find reminders and tasks" on a note that was never extracted (an older recording): extraction
    /// only, with the same busy guard as a recording.
    func findReminders(noteID: Int64) async {
        let asked = personTapped(noteID, subtitle: Stage.extracting.label)   // before any await: the app is in front
        defer { personRunEnded(asked: asked) }
        guard await personMayStart(QueuedTap(noteID: noteID, kind: .findReminders)) else { return }
        await runFindReminders(noteID: noteID)
    }

    /// Find reminders, from the tap or the queue (its turn came).
    private func runFindReminders(noteID: Int64) async {
        guard !isWorking else { return }
        starting = true
        defer { starting = false }
        guard ModelManager.shared.isReady else { lastFailure[noteID] = Self.waitingMessage; return }
        // A queued tap and a person's tap on the same note: the second finds it extracted and does nothing.
        guard ExtractAfterNotes.tapRuns(alreadyExtracted: await CoreEngine.shared.noteExtracted(noteID)) else {
            NSLog("[sve] note %lld: reminders and tasks already extracted; not extracted again", noteID)
            return
        }
        if LiveFirst.yields(recording: LiveTranscriber.shared.liveRunning, runIsLive: false) {
            // One model call with no boundary inside: it waits for the recording, queued (it runs after Stop).
            NSLog("[sve] note %lld steps aside for the recording", noteID)
            queued = TapQueue.enqueue(queued, QueuedTap(noteID: noteID, kind: .findReminders))
            return
        }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        let abort = self.abort
        func yields() -> Bool { LiveFirst.yields(recording: LiveTranscriber.shared.liveRunning, runIsLive: false) }
        // The heat ladder: eased or minimal rests first (cut short by a cooler phone, an expiry or a recording).
        if !(await quietHeatGate({ abort.isSet || yields() })) {
            if yields() {   // a recording started during the rest: it waits, queued, as above
                NSLog("[sve] note %lld steps aside for the recording", noteID)
                queued = TapQueue.enqueue(queued, QueuedTap(noteID: noteID, kind: .findReminders))
                stage = .idle
            } else if HeatGovernor.shared.level == .paused {   // a cut-off; it runs again by itself once cooler
                if stopShort(Self.tapTooWarmMessage, heat: true) == .cutOff {
                    requeueFindReminders(noteID)
                    tapTooWarm.insert(noteID)
                }
            } else if stopShort(Self.cutOffMessage) == .cutOff {   // an expiry during the rest
                requeueFindReminders(noteID)
            }
            return
        }
        if case .cooling = stage { stage = .idle }
        tapTooWarm.remove(noteID)
        let engine = CoreEngine.shared
        guard let note = await engine.note(noteID) else { return }   // deleted (before its turn came): nothing to do
        runningTap = QueuedTap(noteID: noteID, kind: .findReminders)   // on disk: a kill keeps it
        defer { runningTap = nil }
        guard let t = note.transcript, !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            stopShort(String(localized: "I couldn't hear anyone speaking in this recording."), noSpeech: true); return
        }
        do {
            try await loadSelectedModel()
        } catch {
            if stopShort(error.localizedDescription, error: error) == .cutOff { requeueFindReminders(noteID) }
            return
        }
        if await engine.pieces(noteID).isEmpty {
            await engine.setPieces(noteID, PiecesBackfill.pieces(transcript: t, durationSeconds: note.duration_seconds))
        }
        stage = .extracting
        do {
            // On the backend for right now, retried once on the CPU when the app is closed mid-run (GPU stopped).
            let n = try await onBackend { try await engine.extract(noteID: noteID, shouldStop: { ContinuedWork.beat(); return abort.isSet || Self.critical }) }
            NSLog("[sve] extract added %d suggestions", n)
            setStreak(noteID, .findReminders, nil)
            stage = .done
        } catch {
            NSLog("[sve] extract failed: %@", String(describing: error))
            let hot = Self.critical
            if stopShort(error.localizedDescription, error: error, heat: hot) == .cutOff {
                requeueFindReminders(noteID)
                if hot { tapTooWarm.insert(noteID) }
            }
        }
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
    }

    private func beginBackground() {
        notesStored = false
        runPausedForHeat = false
        runSteppedAside = false
        wentToBackground = false
        stepOnGPU = false
        // A run of the overnight task that starts after it expired is cancelled at once; any other run starts clean.
        abort.set(overnightOwnsRun.isSet && (overnightRun?.isSet ?? false))
        ContinuedWork.shared.startNote()   // lock-screen progress starts again for this note
        // Keep the screen on while the model works: a locked phone stops GPU work and the run fails.
        ScreenAwake.pipeline(wants: true)
        if backgroundObserver == nil {
            backgroundObserver = NotificationCenter.default.addObserver(
                forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { if self?.isWorking == true { self?.wentToBackground = true } }
            }
        }
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "sve.process") { [weak self] in
            self?.backgroundTimeEnded()
        }
    }

    /// The plain background time ran out. When iOS refused the continued-processing task (a Stop from the Live
    /// Activity or a shortcut with the app in the background) and nothing else keeps the app running, the app is
    /// about to be suspended: the run is cut off now, as when a continued-processing task expires, so the note goes on
    /// the overnight list by itself, the overnight task is asked for and "paused" is posted once.
    /// Before iOS 26 there is no continued-processing task: a Stop with the app not in front is treated the same.
    /// When the note does not exist yet (the engine had not saved it), the recording is still an unfinished one:
    /// the overnight task (or the next launch) turns it into a note first (recoverUnfinishedRecordings).
    private func backgroundTimeEnded() {
        var noContinuedTask = ContinuedWork.shared.refused
        if #unavailable(iOS 26) { noContinuedTask = stoppedWhileNotActive }
        if isWorking, !appIsActive, noContinuedTask, !ContinuedWork.shared.isHeld, overnightRun == nil {
            if let id = activeNoteID {
                NSLog("[sve] bg: background time ended without continued processing; note %lld finishes in the overnight task", id)
            } else {
                NSLog("[sve] bg: background time ended without continued processing before the note was saved; the overnight task saves and finishes it")
            }
            continuedWorkExpired(hold: NotesNotifier.Hold())
            OvernightWork.shared.schedule()   // also when no note was cut off (not saved yet)
        }
        endBackground()
    }
    private func endBackground() {
        // Ended, not killed: nothing left to restore next launch, unless the note is queued to go on (the queue is not saved).
        if let id = runningNote, !WaitingNotes.keepsRunningNote(queued: interrupted.contains(id)) {
            runningNote = nil
            launchResumes[String(id)] = nil
        }
        ContinuedWork.shared.endNote()   // a Stop or tap from here on finds this note's run over
        stoppedWhileNotActive = false   // this run's; a later run is not one stopped from the background
        ScreenAwake.pipeline(wants: false)   // the Finish now mode may still keep the screen on
        if bgTask != .invalid { UIApplication.shared.endBackgroundTask(bgTask); bgTask = .invalid }
        // A tap or a recording queued behind this run (or a paused note) goes next.
        if !(interrupted.isEmpty && queued.isEmpty && insightDue.isEmpty) && !resuming { Task { await self.resumeInterrupted() } }
        // After the caller's other defers (its busy claim) have run.
        else if !resuming { Task { self.finishContinuedWork() } }
    }

    /// Nothing is running and nothing more can run now: release the continued-processing task. Not while a person's
    /// run that asked for it has not ended (it may be waiting for memories to yield, so nothing runs for a moment),
    /// nor while a Stop is still being handed over (it asked for the task and runs, or queues, once handed over).
    /// Nor while queued taps or notes may start now (waitingToRun): those run first, under this task. tried, triedTaps:
    /// the notes and taps the resume loop has just tried (its own call, which never starts another pass); nil from
    /// anywhere else.
    private func finishContinuedWork(tried: Set<Int64>? = nil, triedTaps: [QueuedTap] = [], triedInsights: Set<Int64> = []) {
        let waiting = waitingToRun(tried: tried ?? [], triedTaps: triedTaps, triedInsights: triedInsights)
        guard TaskLifecycle.finishes(working: isWorking || !handingOver.isEmpty, personRunsPending: personRunsPending,
                                     queued: waiting) else {
            if tried == nil, waiting > 0, !isWorking, !resuming, personRunsPending == 0, handingOver.isEmpty {
                Task { await self.resumeInterrupted() }
            }
            return
        }
        // Success also when all that is left only waits for the recording (it goes on after Stop, under its own task).
        ContinuedWork.shared.finish(success: LiveFirst.finishSucceeds(
            otherNotesLeft: interrupted.filter { !waitsQuietly($0) }.count, tapsLeft: queued.count,
            insightsLeft: insightDue.count, recording: LiveTranscriber.shared.isRecording))
    }

    /// Queued taps and notes that may start now, as the resume loop takes them: none while the app could not keep
    /// running them, a recording runs, the AI is not ready, the phone is too hot to start, or the overnight task
    /// has expired.
    private func waitingToRun(tried: Set<Int64>, triedTaps: [QueuedTap], triedInsights: Set<Int64> = []) -> Int {
        guard appIsActive || ContinuedWork.shared.isHeld || overnightRun != nil, !(overnightRun?.isSet ?? false),
              !LiveTranscriber.shared.isRecording, ModelManager.shared.isReady,
              HeatGovernor.shared.level != .paused else { return 0 }
        return runnableTaps(tried: triedTaps).count + resumable(tried: tried).count + insightDue.subtracting(triedInsights).count
    }

    /// The queued taps the resume loop may take next (each once per pass), oldest first.
    /// A tap cut off 3 times in a row waits for the app in front or the charger (TapQueue.mayRun).
    /// A tap on a note the person chose to finish overnight (a regenerate stopped for the night) waits as the note does.
    private func runnableTaps(tried: [QueuedTap]) -> [QueuedTap] {
        let active = appIsActive, plugged = Self.pluggedIn, inTask = overnightRun != nil, onPower = OvernightWork.shared.runOnPower
        return queued.filter {
            !tried.contains($0) && TapQueue.mayRun($0, appActive: active, pluggedIn: plugged)
                && !TapQueue.waitsForNight(markedByPerson: isOvernight($0.noteID), pluggedIn: plugged,
                                           inOvernightTask: inTask, taskOnPower: onPower)
        }
    }

    /// Person's runs (personTapped) that asked for the continued-processing task and have not ended yet.
    @ObservationIgnored private var personRunsPending = 0

    /// A person's tap on a note (Generate notes, Regenerate, Try again, Do it now, Find reminders), called before the
    /// entry point's first await, while the app is in front: asks for the continued-processing task, as
    /// recordingStopped does, so the run goes on (with lock-screen progress) after the app is closed. Not when the run
    /// cannot start (the AI is not ready, another note's notes run: that run keeps its task). A task already held or
    /// asked for is kept. True when it asked: the caller passes it to personRunEnded.
    private func personTapped(_ id: Int64, subtitle: String) -> Bool {
        guard PersonRun.asksForContinuedTask(appActive: appIsActive, aiReady: ModelManager.shared.isReady,
                                             working: isWorking, memoriesOnly: memoryWork) else { return false }
        personRunsPending += 1
        ContinuedWork.shared.begin(key: "note-\(id)", subtitle: subtitle)
        return true
    }

    /// Every exit of a person's entry point (done, failed, stopped, busy, AI not ready, the note deleted): the task
    /// asked for in the tap is released once nothing is left, after the caller's other defers (its busy claim), as
    /// endBackground does. Notes still queued go next (the resume loop releases it when it ends); another run still
    /// working keeps it.
    private func personRunEnded(asked: Bool) {
        if asked { personRunsPending = max(0, personRunsPending - 1) }
        guard !resuming else { return }   // the resume loop releases it when it ends
        if interrupted.isEmpty && queued.isEmpty && insightDue.isEmpty { Task { self.finishContinuedWork() } } else { Task { await self.resumeInterrupted() } }
    }

    /// Runs the queued taps, then re-runs cut-off and waiting notes, oldest first, one at a time, while the app is
    /// active (or a continued-processing or the overnight task keeps it running), not recording, the AI is ready and
    /// the phone is not too hot. Each tap and note is tried once per pass, so one that is cut off or paused again
    /// (heat, background) waits for the next pass instead of looping. Notes marked overnight join only while plugged
    /// in or inside the overnight task.
    func resumeInterrupted() async {
        guard !resuming else { return }
        resuming = true
        var tried: Set<Int64> = []
        var triedTaps: [QueuedTap] = []
        var triedInsights: Set<Int64> = []
        defer {
            resuming = false
            finishContinuedWork(tried: tried, triedTaps: triedTaps, triedInsights: triedInsights)
            endWaitingActivities()
        }
        while !isWorking, appIsActive || ContinuedWork.shared.isHeld || overnightRun != nil,
              !(overnightRun?.isSet ?? false), !LiveTranscriber.shared.isRecording,
              ModelManager.shared.isReady, HeatGovernor.shared.level != .paused {
            let tap = TapQueue.next(runnableTaps(tried: triedTaps))
            let id: Int64
            var insightOnly = false
            if let tap { id = tap.noteID } else if let n = nextToResume(tried: tried) { id = n }
            else if let i = insightDue.subtracting(triedInsights).min() { id = i; insightOnly = true } else { break }
            let owned = OvernightList.ownsRun(inOvernightTask: overnightRun != nil, appActive: appIsActive,
                                              continuedTaskHeld: ContinuedWork.shared.isHeld)
            if insightOnly {
                // Last: insights that stepped aside for a recording, each once per pass.
                triedInsights.insert(id)
                overnightOwnsRun.set(owned)
                await runDueInsight(id)
                overnightOwnsRun.set(false)
                continue
            }
            let how = ContinuedWork.shared.isHeld ? "continued processing" : (overnightRun != nil ? "overnight task" : "background time")
            if let tap {
                // Before cut-off notes: the person asked for it. A note deleted meanwhile: dropped (each run checks).
                triedTaps.append(tap)
                NSLog("[sve] bg: queued tap runs (note %lld)", id)
                overnightOwnsRun.set(owned)
                await runQueued(tap)
                overnightOwnsRun.set(false)
                continue
            }
            tried.insert(id)
            interrupted.remove(id)
            heatPaused.remove(id)
            if !appIsActive { NSLog("[sve] bg: resuming note %lld (%@)", id, how) }
            overnightOwnsRun.set(owned)
            await rerun(noteID: id)
            overnightOwnsRun.set(false)
            if !interrupted.contains(id) { markAutoTried(id) }
        }
    }

    /// A queued tap's turn: the same run the tap would have started.
    /// It is held as the running tap before it leaves the queue, so a kill at any moment keeps it on disk.
    private func runQueued(_ tap: QueuedTap) async {
        let id = tap.noteID
        runningTap = tap
        queued.removeAll { $0 == tap }
        defer { runningTap = nil }
        switch tap.kind {
        case .regenerate:
            _ = await runRegenerate(tap)
        case .generate:
            interrupted.remove(id)   // runs now (as Try again does), not again as a cut-off note
            heatPaused.remove(id)
            await rerun(noteID: id)
            if !interrupted.contains(id) { markAutoTried(id) }
        case .findReminders:
            await runFindReminders(noteID: id)
        }
    }

    /// The resume loop stopped with the app not in front and nothing running: notes still waiting their turn will not
    /// start until the app is opened (or the overnight task runs), so their Live Activities end as paused instead of
    /// staying on "I'll get to this after the earlier recording".
    private func endWaitingActivities() {
        guard !appIsActive, !isWorking else { return }
        for id in interrupted.sorted() where RecordingActivities.shared.isFollowing(noteID: id) && !waitsQuietly(id) {
            RecordingActivities.shared.update(noteID: id, .cutOff, line: lastFailure[id] ?? Self.cutOffMessage, percent: 0)
        }
    }

    /// The oldest note that may start now: cut off or waiting, or marked overnight while plugged in or inside
    /// the overnight task; never one paused for heat until the ladder is below paused.
    private func nextToResume(tried: Set<Int64>) -> Int64? { resumable(tried: tried).first }

    /// The notes the resume loop may take next, oldest first.
    private func resumable(tried: Set<Int64>) -> [Int64] {
        let cool = HeatGovernor.shared.level < .paused
        let inTask = overnightRun != nil, plugged = Self.pluggedIn, onPower = OvernightWork.shared.runOnPower
        let marked = Set(overnight)
        let automaticMarked = marked.intersection(automatic)   // cut-off notes kept across launches: always candidates
        return interrupted.union(automaticMarked).union(inTask || plugged ? marked : []).sorted().filter({
            !tried.contains($0)
                && OvernightList.mayStart(mark: OvernightList.mark(onList: marked.contains($0), automatic: automatic.contains($0)),
                                          pluggedIn: plugged, inOvernightTask: inTask, taskOnPower: onPower)
                && (!heatPaused.contains($0) || cool)
        })
    }

    /// Once the AI is ready: every note with audio and no transcript (recorded while it downloaded)
    /// is queued and processed one at a time. When nothing else is left, notes that still need memories get them.
    func processWaitingNotes() async {
        await queueWaitingNotes()
        await resumeInterrupted()
        await backfillMemories()   // then, idle in front, memories for older notes
    }

    /// Queues the notes with audio and no transcript that were never tried (not those marked overnight:
    /// resumeInterrupted takes them only while plugged in or inside the overnight task).
    private func queueWaitingNotes() async {
        guard ModelManager.shared.isReady else { return }
        let engine = CoreEngine.shared
        var candidates: [WaitingNotes.Candidate] = []
        let limit: Int32 = 10_000
        let all = await engine.notes(limit: limit)
        if !autoTriedPruned {   // once per launch: ids of deleted notes leave the saved list
            autoTriedPruned = true
            let pruned = WaitingNotes.prunedTried(autoTried, existing: Set(all.map(\.id)), listComplete: all.count < Int(limit))
            if pruned != autoTried {
                autoTried = pruned
                UserDefaults.standard.set(autoTried.sorted(), forKey: "pipeline.autoTried")
            }
        }
        for n in all where n.has_audio && !autoTried.contains(n.id) {
            guard let note = await engine.note(n.id) else { continue }
            noteTitles[n.id] = note.title
            let path = AudioRecorder.existingAudioPath(note.audio_path)
            // A recording just stopped is already being handed over to its own run.
            if let path, isHandingOver(path) || isHandingOver(note.audio_path ?? "") { continue }
            candidates.append(.init(id: n.id,
                                    hasAudio: path != nil,
                                    hasTranscript: !(note.transcript ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty))
        }
        let ids = WaitingNotes.toProcess(candidates, alreadyTried: autoTried, running: isWorking ? activeNoteID : nil)
            .filter { !overnight.contains($0) }
        if !ids.isEmpty { NSLog("[sve] %ld notes waiting for the AI", ids.count) }
        interrupted.formUnion(ids)
        // Once per launch: notes that stepped aside for a recording (kept across launches: the app may have been
        // killed meanwhile) and still need their notes: queued again, with no overnight mark. A note on the overnight
        // list waits there.
        guard !steppedAsideRestored else { return }
        steppedAsideRestored = true
        for id in steppedAside.sorted() where !interrupted.contains(id) {
            guard !overnight.contains(id), let note = await engine.note(id), steppedAside.contains(id),   // its run may have started meanwhile
                  WaitingNotes.needsWork(hasTranscript: !(note.transcript ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
                                         outputs: OutputKind.allCases.map { note.output($0) != nil }) else {
                steppedAside.remove(id)
                launchResumes[String(id)] = nil
                continue
            }
            noteTitles[id] = note.title
            NSLog("[sve] note %lld stepped aside for a recording earlier; it is queued again", id)
            interrupted.insert(id)
        }
    }

    private func isHandingOver(_ path: String) -> Bool {
        handingOver.contains(path) || LiveTranscriber.shared.isTranscribing(audioPath: path)
    }

    /// At launch, before the waiting notes are queued: a recording that was going when the app was closed (killed, or
    /// ended by iOS), started in the app or by a shortcut, becomes a note now. Its audio and transcription progress
    /// (sidecar) are kept, and it is processed like any recording whose work was interrupted: transcription goes on
    /// from the saved cursor, then the notes.
    func recoverUnfinishedRecordings() async {
        let unfinished = UnfinishedRecordings.paths
        // One pass at a time (another window or a scene reconnect may call this too).
        guard !unfinished.isEmpty, !recovering else { return }
        recovering = true
        defer { recovering = false }
        let engine = CoreEngine.shared
        var known: Set<String> = []
        for n in await engine.notes(limit: 10_000) where n.has_audio {
            if let p = await engine.note(n.id)?.audio_path { known.insert((p as NSString).lastPathComponent) }
        }
        for stored in unfinished {
            if RecordingController.shared.isRecording(audioPath: stored) || isHandingOver(stored) { continue }   // this launch's
            let name = (stored as NSString).lastPathComponent
            guard !known.contains(name), let path = AudioRecorder.existingAudioPath(stored) else {
                UnfinishedRecordings.remove(stored)   // already a note, or the file is gone
                continue
            }
            // Still unfinished after the awaits above (a Stop or another pass may have taken it meanwhile).
            guard UnfinishedRecordings.paths.contains(stored), !isHandingOver(stored) else { continue }
            let duration = UnfinishedRecordings.finishWAV(atPath: path) ?? 0
            let ms = Double(name.dropFirst("rec-".count).prefix { $0.isNumber }) ?? 0
            let started = ms > 0 ? Date(timeIntervalSince1970: ms / 1000) : Date()
            let title = "Recording " + Self.titleFormatter.string(from: started)
            let id = await engine.createNote(title: title, lang: AppSettings.shared.notesLanguage, audioPath: path,
                                             transcript: nil, durationSeconds: duration)
            // Not saved: the marker stays on purpose, so the next launch tries again (ruling B7).
            guard id > 0 else { NSLog("[sve] unfinished recording %@: note not saved", name); continue }
            UnfinishedRecordings.remove(stored)
            noteTitles[id] = title
            memoriesMayBeDue = true
            NSLog("[sve] unfinished recording %@ (%lld s) saved as note %lld; it is processed as interrupted", name, duration, id)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }
    }

    /// A recovery pass is running (recoverUnfinishedRecordings).
    @ObservationIgnored private var recovering = false

    /// At launch: removes transcription progress files whose recording is gone or belongs to no note. The recordings
    /// themselves are left alone, and so is the progress of a recording that was going when the app was closed (it
    /// becomes a note: recoverUnfinishedRecordings).
    func sweepOrphanSidecars() async {
        // Read first: a recording recovered meanwhile is a note by the time the notes are read below.
        let unfinished = UnfinishedRecordings.names
        let dir = AudioRecorder.recordingsDirectory
        let files = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        let sidecars = files.filter { $0.hasSuffix(".live.json") }
        guard !sidecars.isEmpty else { return }
        let engine = CoreEngine.shared
        var known: Set<String> = []
        for n in await engine.notes(limit: 10_000) where n.has_audio {
            if let p = await engine.note(n.id)?.audio_path { known.insert((p as NSString).lastPathComponent) }
        }
        for f in sidecars {
            let audioName = String(f.dropLast(".live.json".count))
            let audioPath = dir.appendingPathComponent(audioName).path
            if known.contains(audioName) || unfinished.contains(audioName) || isHandingOver(audioPath) { continue }
            NSLog("[sve] removing orphan transcription progress %@", f)
            TranscriptSidecar.remove(audioPath: audioPath)
        }
    }

    // MARK: compress audio

    @ObservationIgnored private var compressing: Set<Int64> = []

    /// Nothing is converted during a recording, while this note is being written, or while its WAV is
    /// still being handed from live transcription to the pipeline.
    private func mayCompress(_ id: Int64, path: String? = nil) -> Bool {
        if LiveTranscriber.shared.isRecording { return false }
        if isWorking && activeNoteID == id { return false }
        if starting && activeNoteID == id { return false }
        if let path, isHandingOver(path) { return false }
        return true
    }

    /// Once a note has its transcript and all three outputs, its WAV becomes an AAC file. Order: convert,
    /// check the .m4a, point the note at it, only then delete the WAV (and its progress file). Any failure
    /// removes the .m4a and keeps the WAV; the next launch tries again, so a kill at any point leaves a
    /// recording that plays.
    func compressAudio(noteID id: Int64) async {
        guard !compressing.contains(id), mayCompress(id) else { return }
        let engine = CoreEngine.shared
        guard let note = await engine.note(id), let stored = note.audio_path,
              (stored as NSString).pathExtension.lowercased() == "wav",
              !WaitingNotes.needsWork(hasTranscript: !(note.transcript ?? "").isEmpty,
                                      outputs: OutputKind.allCases.map { note.output($0) != nil }),
              let path = AudioRecorder.existingAudioPath(stored),
              (path as NSString).pathExtension.lowercased() == "wav",
              mayCompress(id, path: path) else { return }
        compressing.insert(id)
        defer { compressing.remove(id) }
        let wav = URL(fileURLWithPath: path)
        let m4a: URL
        do { m4a = try await AudioCompressor.compress(wav: wav) } catch {
            NSLog("[sve] audio compression failed: %@", String(describing: error))
            return
        }
        func discard() { try? FileManager.default.removeItem(at: m4a) }
        // The length to expect is the WAV's own; the note's stored seconds are only a rounded timer.
        guard let source = try? AVAudioFile(forReading: wav), source.processingFormat.sampleRate > 0,
              AudioCompressor.verify(m4a: m4a, expectedSeconds: Double(source.length) / source.processingFormat.sampleRate) else {
            NSLog("[sve] compressed audio did not check out; keeping the WAV")
            discard(); return
        }
        // Things may have changed while converting: a recording started, the note was deleted or redone.
        guard mayCompress(id, path: path), let now = await engine.note(id), now.audio_path == stored,
              !WaitingNotes.needsWork(hasTranscript: !(now.transcript ?? "").isEmpty,
                                      outputs: OutputKind.allCases.map { now.output($0) != nil }),
              await engine.setAudioPath(id, m4a.path) else { discard(); return }
        AudioRecorder.removeAudio(atPath: path)
        NotificationCenter.default.post(name: .notesChanged, object: nil)
    }

    /// At launch: removes half-written .m4a.part files, finishes a swap an app kill cut short (note already
    /// on the .m4a, WAV still there), and compresses the finished notes that still have a WAV.
    func sweepCompression() async {
        if compressing.isEmpty { AudioCompressor.sweepParts(in: AudioRecorder.recordingsDirectory) }
        let engine = CoreEngine.shared
        for n in await engine.notes(limit: 10_000) where n.has_audio {
            guard !LiveTranscriber.shared.isRecording, let note = await engine.note(n.id),
                  let stored = note.audio_path else { continue }
            if (stored as NSString).pathExtension.lowercased() == "m4a" {
                let wav = AudioCompressor.wavSibling(ofM4A: AudioRecorder.existingAudioPath(stored) ?? stored)
                if FileManager.default.fileExists(atPath: wav), !isHandingOver(wav), AudioRecorder.existingAudioPath(stored) != nil {
                    AudioRecorder.removeAudio(atPath: wav)
                }
            } else {
                await compressAudio(noteID: n.id)
            }
        }
    }

    // MARK: overnight

    /// After Stop, before the run goes on: when the work left (the audio not yet transcribed, the sections not
    /// yet written, the three outputs; never the memories) is long, the note offers to finish overnight.
    private func offerOvernightIfLong(_ id: Int64, audio: URL, duration: Int64) {
        let s = TranscriptSidecar.load(audioPath: audio.path)
        let written = NoteSection.validPrefix(s.sections, pieceCount: s.pieces.count)
        let dur = Int(clamping: max(0, duration))
        let audioLeft = dur - Int(s.transcribedSeconds)
        let sectionsLeft = WorkEstimate.sectionsLeft(durationSeconds: dur, coveredSeconds: written.last?.endSec ?? 0)
        let outputs = OutputKind.allCases.count
        let minutes = WorkEstimate.minutesLeft(audioSecondsLeft: audioLeft, sectionsLeft: sectionsLeft, outputsLeft: outputs)
        NSLog("[sve] note %lld: about %ld min of work left before memories", id, minutes)
        // Memory sections never count toward the offer (K43): they run later, idle in front or overnight.
        if WorkEstimate.offersOvernight(audioSecondsLeft: audioLeft, sectionsLeft: sectionsLeft, outputsLeft: outputs) {
            overnightOffer = id
        }
    }

    /// "Yes" to "Finish overnight while charging?": the note is marked, the run stops before its next piece or
    /// stage (what is finished is kept) and the overnight task is asked for.
    func finishOvernight(noteID id: Int64) {
        if overnightOffer == id { overnightOffer = nil }
        markOvernight(id)
        if automatic.remove(id) != nil { saveAutomatic() }   // now the person's mark: it waits for the charger
        if isWorking, activeNoteID == id {
            parkRequested.insert(id)
        } else {
            interrupted.remove(id)   // queued: it now waits for the charger instead
            steppedAside.remove(id)
            heatPaused.remove(id)
        }
        OvernightWork.shared.schedule()
    }

    /// "Keep going": the offer goes away and the run carries on.
    func keepGoing(noteID id: Int64) {
        if overnightOffer == id { overnightOffer = nil }
    }

    /// "Do it now" on a note waiting for the charger: the mark is cleared and it runs now (or next, when
    /// another note is being written).
    func doItNow(noteID id: Int64) async {
        let asked = personTapped(id, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(asked: asked) }
        unmarkOvernight(id)
        parkRequested.remove(id)
        lastFailure[id] = nil
        await yieldMemories()
        guard !isWorking else { interrupted.insert(id); return }
        await rerun(noteID: id)
    }

    /// Waiting for the charger by the person's choice (an automatic mark is not shown: it resumes in front at once).
    func isOvernight(_ id: Int64) -> Bool { overnight.contains(id) && !automatic.contains(id) }

    /// A note deleted: off the overnight list (and every other queue).
    func forget(noteID id: Int64) {
        RecordingActivities.shared.update(noteID: id, .deleted, line: "", percent: 0)
        unmarkOvernight(id)
        noteTitles[id] = nil
        parkRequested.remove(id)
        interrupted.remove(id)
        steppedAside.remove(id)
        if runningNote == id { runningNote = nil }
        launchResumes[String(id)] = nil
        queued = TapQueue.forgetting(noteID: id, in: queued)
        ChatStore.shared.forget(noteID: id)   // its quotes, titles and links in Ask's chats
        insightDue.remove(id)
        heatPaused.remove(id)
        lastFailure[id] = nil
        failedLines[id] = nil
        regenerateFailed[id] = nil
        tapTooWarm.remove(id)
        failureStreak = FailureKind.forgetting(noteID: id, in: failureStreak)
        if overnightOffer == id { overnightOffer = nil }
    }

    /// The app went to the background: if any note still has work (marked overnight or not, running or cut
    /// off, or memories to write), ask for the overnight task so it can finish while the phone charges.
    func appWentToBackground() {
        if isWorking || !interrupted.isEmpty || !overnight.isEmpty || memoriesMayBeDue { OvernightWork.shared.schedule() }
    }

    /// Before each piece or stage (from the heat gate): "Yes" was tapped for this note, so it stops here.
    private func parkedForNight(_ id: Int64) -> Bool {
        guard parkRequested.remove(id) != nil, overnight.contains(id) else { return false }
        NSLog("[sve] note %lld: stopping for the night", id)
        parked.insert(id)
        interrupted.remove(id)
        heatPaused.remove(id)
        lastFailure[id] = nil
        stage = .overnight
        return true
    }

    /// A run of this note ended (process / regenerate). The overnight mark stays while work is left: stopped
    /// for the night, or cut off / too hot (queued). Finished, or a real failure the note now shows: unmarked.
    private func settleOvernight(_ id: Int64) {
        parkRequested.remove(id)
        let stoppedForNight = parked.remove(id) != nil
        if overnightOffer == id { overnightOffer = nil }
        if stoppedForNight, case .overnight = stage { stage = .idle }   // the note's own banner shows the wait
        guard overnight.contains(id),
              !OvernightList.keepsMark(marked: true, parked: stoppedForNight, queued: interrupted.contains(id)) else { return }
        unmarkOvernight(id)
    }

    private func markOvernight(_ id: Int64) {
        guard !overnight.contains(id) else { return }
        overnight.append(id)
        UserDefaults.standard.set(overnight, forKey: Self.overnightKey)
    }

    private func unmarkOvernight(_ id: Int64) {
        if automatic.remove(id) != nil { saveAutomatic() }
        guard overnight.contains(id) else { return }
        overnight.removeAll { $0 == id }
        UserDefaults.standard.set(overnight, forKey: Self.overnightKey)
    }

    /// Only notes still on the overnight list are automatic.
    private func saveAutomatic() {
        automatic.formIntersection(overnight)
        UserDefaults.standard.set(automatic.sorted(), forKey: Self.automaticKey)
    }

    /// Drops notes from the overnight list that were deleted or have nothing left to do.
    private func pruneOvernight() async {
        let snapshot = overnight
        guard !snapshot.isEmpty else { return }
        var keep: Set<Int64> = []   // still there, with work left
        for id in snapshot {
            guard let n = await CoreEngine.shared.note(id) else { continue }   // deleted
            let hasTranscript = !(n.transcript ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            if WaitingNotes.needsWork(hasTranscript: hasTranscript, outputs: OutputKind.allCases.map { n.output($0) != nil }) {
                keep.insert(id)
            }
        }
        // Against the list as it is now: a note marked meanwhile is kept, one forgotten meanwhile stays gone.
        let pruned = OvernightList.pruned(overnight, checked: Set(snapshot), keep: keep)
        if pruned != overnight {
            overnight = pruned
            UserDefaults.standard.set(overnight, forKey: Self.overnightKey)
            saveAutomatic()
        }
    }

    /// The overnight task (OvernightWork): once the pipeline is free, the overnight notes, the cut-off notes and
    /// the recordings that waited for the AI, one at a time, then the memories of notes that still need them
    /// (newest first), on the CPU (the app is in the background), with the heat rule. `expired`: the system ended
    /// the task; the run in flight was cancelled (cancelForExpiry) and nothing new starts. Returns whether work is left (the task is asked for again).
    func runOvernight(expired: AtomicFlag) async -> Bool {
        overnightRun = expired
        defer { overnightRun = nil }
        // A run still going (a continued-processing task, or the app in front) finishes first.
        while (isWorking || resuming) && !expired.isSet {
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
        if !expired.isSet {
            // A recording whose note was never saved (the app was suspended or closed first) becomes a note now.
            await recoverUnfinishedRecordings()
            await pruneOvernight()   // deleted notes are skipped
            await queueWaitingNotes()
            await resumeInterrupted()
        }
        // After every note's notes: memories for the notes that still need them, newest first.
        if !expired.isSet {
            overnightOwnsRun.set(OvernightList.ownsRun(inOvernightTask: true, appActive: appIsActive,
                                                       continuedTaskHeld: ContinuedWork.shared.isHeld))
            await backfillMemories()
            overnightOwnsRun.set(false)
        }
        await pruneOvernight()
        var memoriesLeft = memoriesMayBeDue
        if !expired.isSet {   // notes whose memory call failed in this launch do not count: they wait for the next one
            let needing = await CoreEngine.shared.notesNeedingMemories(limit: 1 + memoryFailed.count)
            memoriesLeft = !MemoryPass.backfillNotes(needing, skipping: memoryFailed, limit: 1).isEmpty || !sectionsDueNow.isEmpty
        }
        return !overnight.isEmpty || !interrupted.isEmpty || !queued.isEmpty || !insightDue.isEmpty || memoriesLeft
    }

    /// The overnight task expired (any thread): cancel the model run in flight if the task started it. The
    /// note keeps every piece, section and output already finished and is queued again (the stage's failure
    /// handler, abort set). A run in front, or one a continued-processing task keeps going, is left alone.
    nonisolated func cancelForExpiry() { if overnightOwnsRun.isSet { abort.set(true) } }

    private static let titleFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "d MMM, h:mm a"
        return f
    }()
}

extension Notification.Name {
    static let notesChanged = Notification.Name("sve.notesChanged")
    /// Posted after "Delete everything": screens drop what they hold in memory (Ask's chat).
    static let assistantWiped = Notification.Name("sve.assistantWiped")
}

/// Transcript pieces for notes recorded before pieces existed, so Ask and Deep dive can search them.
enum PiecesBackfill {
    /// Pieces are at most this many Unicode scalars (stricter than characters), so each fits one
    /// extraction window whole (core/tests/test_suggest.cpp backfill_pieces_fit_extraction_windows).
    static let maxScalars = 1_500
    private static let doneKey = "backfill.pieces.v1"

    /// Splits at sentence boundaries into pieces of at most maxScalars; a longer sentence is cut at
    /// spaces (or hard, as a last resort). start_sec is estimated in proportion to the duration.
    static func pieces(transcript: String, durationSeconds: Int64) -> [TranscriptPiece] {
        let text = transcript.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return [] }
        var sentences: [String] = []
        text.enumerateSubstrings(in: text.startIndex..<text.endIndex, options: [.bySentences, .substringNotRequired]) { _, _, enclosing, _ in
            sentences.append(String(text[enclosing]))
        }
        if sentences.isEmpty { sentences = [text] }
        var chunks: [String] = []
        var cur = ""
        func flush() {
            let c = cur.trimmingCharacters(in: .whitespacesAndNewlines)
            if !c.isEmpty { chunks.append(c) }
            cur = ""
        }
        for s in sentences.flatMap(split) {
            if cur.unicodeScalars.count + s.unicodeScalars.count > maxScalars { flush() }
            cur += s
        }
        flush()
        let total = max(1, chunks.reduce(0) { $0 + $1.unicodeScalars.count })
        var offset = 0
        return chunks.map { c in
            let start = Int(Double(offset) / Double(total) * Double(max(0, durationSeconds)))
            offset += c.unicodeScalars.count
            return TranscriptPiece(startSec: start, text: c)
        }
    }

    /// A sentence longer than maxScalars, cut at the last space before the limit when there is one
    /// (else at a character boundary). Never cuts inside a character.
    private static func split(_ sentence: String) -> [String] {
        guard sentence.unicodeScalars.count > maxScalars else { return [sentence] }
        var out: [String] = []
        var cur = ""
        var n = 0
        for ch in sentence {
            let k = ch.unicodeScalars.count
            if n + k > maxScalars, let sp = cur.lastIndex(of: " "), sp > cur.startIndex {
                let tail = String(cur[cur.index(after: sp)...])
                out.append(String(cur[...sp]))
                cur = tail
                n = tail.unicodeScalars.count
            }
            if n + k > maxScalars, !cur.isEmpty {
                out.append(cur)
                cur = ""
                n = 0
            }
            cur.append(ch)
            n += k
        }
        if !cur.isEmpty { out.append(cur) }
        return out
    }

    /// One-time at launch: every note with a transcript and no pieces gets pieces. Marked done only
    /// when every note was handled, so a failure is retried at the next launch.
    @MainActor
    static func runOnce() async {
        let d = UserDefaults.standard
        guard !d.bool(forKey: doneKey) else { return }
        let engine = CoreEngine.shared
        var allOK = true, any = false
        for n in await engine.notes(limit: 10_000) {
            guard await engine.pieces(n.id).isEmpty, let note = await engine.note(n.id),
                  let t = note.transcript else { continue }
            let ps = pieces(transcript: t, durationSeconds: note.duration_seconds)
            guard !ps.isEmpty else { continue }
            if await engine.setPieces(n.id, ps) { any = true } else { allOK = false }
        }
        if allOK { d.set(true, forKey: doneKey) }
        if any { NotificationCenter.default.post(name: .assistantChanged, object: nil) }
    }
}

/// Callbacks seen since the key (phase and chunk) last changed; the core calls from its own thread.
private final class CallbackCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var key = ""
    private var count = 0
    func next(key k: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        if k != key { key = k; count = 0 }
        count += 1
        return count
    }
}

#if DEBUG
// MARK: Debug: Shruti alone over a note's audio (device checks; no UI)

extension ProcessingPipeline {
    /// SVE_RETRANSCRIBE=<id,id>: for each note, Shruti alone over a copy of its audio in tmp (with its own sidecar,
    /// cleared first), so the note, its stored transcript and its own sidecar are never touched; no Gemma fill.
    /// Writes tmp/retranscribe-<id>.txt (pending pieces as [gemma]) and one timing line. Waits for no run in flight.
    fileprivate func debugRetranscribe() {
        guard let raw = ProcessInfo.processInfo.environment["SVE_RETRANSCRIBE"] else { return }
        let ids = raw.split(separator: ",").compactMap { Int64($0.trimmingCharacters(in: .whitespaces)) }
        guard !ids.isEmpty else {
            NSLog("[sve] debug retranscribe: '%@' has no note id", raw)
            return
        }
        Task { @MainActor in
            guard ShrutiRecognizer.shared.ready else {
                NSLog("[sve] debug retranscribe: shruti files not ready; not run")
                return
            }
            for id in ids {
                var waited = 0
                while self.isBusy {
                    if waited >= 3600 {
                        NSLog("[sve] debug retranscribe: a run in flight after 60 min; not run")
                        return
                    }
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                    waited += 1
                }
                await self.debugRetranscribe(id)
            }
            await ShrutiRecognizer.unloadOffMain()
        }
    }

    private func debugRetranscribe(_ id: Int64) async {
        guard let note = await CoreEngine.shared.note(id), let src = AudioRecorder.existingAudioPath(note.audio_path) else {
            NSLog("[sve] debug retranscribe: note %lld or its audio not found", id)
            return
        }
        let fm = FileManager.default
        let tmp = fm.temporaryDirectory
        let dst = tmp.appendingPathComponent("retranscribe-\(id).wav").path
        let textPath = tmp.appendingPathComponent("retranscribe-\(id).txt").path
        TranscriptSidecar.remove(audioPath: dst)
        try? fm.removeItem(atPath: dst)
        try? fm.removeItem(atPath: textPath)
        do {
            if src.lowercased().hasSuffix(".m4a") {   // compressed once its notes were done
                try AudioCompressor.expandToWav(URL(fileURLWithPath: src), to: URL(fileURLWithPath: dst))
            } else {
                try fm.copyItem(atPath: src, toPath: dst)
            }
        } catch {
            NSLog("[sve] debug retranscribe: note %lld copy failed (%@)", id, String(describing: error))
            return
        }
        let thermalBefore = ProcessInfo.processInfo.thermalState.rawValue
        var pieces = 0
        let start = Date()
        do {
            _ = try await TranscriptRun.runShruti(audioPath: dst, isFinal: true, shouldStop: { Self.critical },
                                                  threads: { ShrutiThreads.count(HeatGovernor.shared.level) },
                                                  onPiece: { _ in pieces += 1 })
        } catch {
            NSLog("[sve] debug retranscribe: note %lld stopped (%@)", id, String(describing: error))
        }
        let wall = Date().timeIntervalSince(start)
        let result = TranscriptSidecar.load(audioPath: dst)
        let lines = result.pieces.map { p in "[\(p.start_sec) s] \(p.pending == true ? "[gemma]" : p.text)" }
        try? lines.joined(separator: "\n").write(toFile: textPath, atomically: true, encoding: .utf8)
        NSLog("[sve] retranscribe note %lld: %.1f s audio in %.1f s, %d pieces, %d pending, thermal %ld→%ld",
              id, result.transcribedSeconds, wall, pieces, result.pendingIndices.count,
              thermalBefore, ProcessInfo.processInfo.thermalState.rawValue)
    }
}

// MARK: Debug: replay (Replay in ModelLogic, ReplayMode in CoreEngine)

extension ProcessingPipeline {
    /// SVE_REPLAY=<id,id>: each note's audio, copied, goes through recordingStopped as if just stopped, one after the
    /// other, each starting once the one before has settled and the phone is at heat level full. Logs one line per
    /// replay start and end; the steps' own lines carry the timings.
    fileprivate func debugReplay() {
        guard ReplayMode.active else { return }
        let ids = Replay.ids(ProcessInfo.processInfo.environment[Replay.environmentKey])
        if ProcessInfo.processInfo.environment["SVE_REPLAY_EXTRACT_ONLY"] == "1" { debugExtractOnly(ids); return }
        Task { @MainActor in
            NSLog("[sve] replay: notes %@ on a copy of the notes file", ids.map(String.init).joined(separator: ","))
            var waited = 0
            while !ModelManager.shared.isReady || !ShrutiRecognizer.shared.ready {
                if waited >= 600 { NSLog("[sve] replay: the models are not ready after 10 min; not run"); return }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                waited += 1
            }
            let started = Date()
            for (i, id) in ids.enumerated() {
                await self.replaySettle()
                await self.replayCool()
                await self.replayOne(id, index: i + 1, of: ids.count)
            }
            await self.replaySettle()
            NSLog("[sve] replay: all %d done in %.0f s", ids.count, Date().timeIntervalSince(started))
        }
    }

    /// SVE_REPLAY_EXTRACT_ONLY=1: only reminders and tasks again on the copies of these notes (with SVE_EXTRACT_WINDOW
    /// to try another window size), each on a cool phone; the copy's suggestions change, the person's never.
    private func debugExtractOnly(_ ids: [Int64]) {
        Task { @MainActor in
            var waited = 0
            while !ModelManager.shared.isReady {
                if waited >= 600 { NSLog("[sve] replay extract: the model is not ready after 10 min; not run"); return }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                waited += 1
            }
            for id in ids {
                await self.replaySettle()
                await self.replayCool()
                let start = Date()
                let added = (try? await self.onBackend { try await CoreEngine.shared.extract(noteID: id) }) ?? -1
                NSLog("[sve] replay extract: note %lld added %d suggestions in %.0f s", id, added, Date().timeIntervalSince(start))
            }
            NSLog("[sve] replay extract: all %d done", ids.count)
        }
    }

    /// The screen stays on while a replay waits; the pipeline's own rule may turn it off between runs.
    private func replayAwake() {
        if !UIApplication.shared.isIdleTimerDisabled { UIApplication.shared.isIdleTimerDisabled = true }
    }

    private func replaySettle() async {
        var idleSince: TimeInterval?
        while true {
            replayAwake()
            let now = Date().timeIntervalSince1970
            if isBusy || isWorking || memoryWork { idleSince = nil } else if idleSince == nil { idleSince = now }
            if Replay.settled(idleSince: idleSince, now: now) { return }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    /// Each replay starts on a cool phone (at most 20 min of waiting), so one run's heat is not the next one's.
    private func replayCool() async {
        guard HeatGovernor.shared.level != .full else { return }
        let start = Date()
        NSLog("[sve] replay: waiting for heat level full")
        while HeatGovernor.shared.level != .full, Date().timeIntervalSince(start) < 1200 {
            replayAwake()
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
        NSLog("[sve] replay: heat %@ after %.0f s of waiting", HeatGovernor.shared.level.name, Date().timeIntervalSince(start))
    }

    private func replayOne(_ id: Int64, index: Int, of count: Int) async {
        guard let note = await CoreEngine.shared.note(id), let src = AudioRecorder.existingAudioPath(note.audio_path) else {
            NSLog("[sve] replay %d of %d: note %lld or its audio not found", index, count, id)
            return
        }
        let fm = FileManager.default
        try? fm.createDirectory(at: ReplayMode.audioFolder, withIntermediateDirectories: true)
        let dst = ReplayMode.audioFolder.appendingPathComponent("replay-\(id)-\(Int(Date().timeIntervalSince1970)).wav")
        do {
            if src.lowercased().hasSuffix(".m4a") {
                try AudioCompressor.expandToWav(URL(fileURLWithPath: src), to: dst)
            } else {
                try fm.copyItem(atPath: src, toPath: dst.path)
            }
        } catch {
            NSLog("[sve] replay %d of %d: note %lld copy failed (%@)", index, count, id, String(describing: error))
            return
        }
        NSLog("[sve] replay %d of %d: note %lld (%lld s audio), Stop (heat %@, battery %.2f)", index, count, id,
              note.duration_seconds, HeatGovernor.shared.level.name, UIDevice.current.batteryLevel)
        recordingStopped(audio: dst, duration: note.duration_seconds)
    }
}
#endif

