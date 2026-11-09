// Recording → transcript → title → three outputs. Everything after recording is local.
// Transcription runs on the CPU, mostly while recording (LiveTranscriber); after Stop it continues from
// the saved cursor (TranscriptSidecar). Notes use the GPU while the app is in front, else the CPU.
// On iOS 26+ a continued-processing task keeps the work going with the phone locked. If iOS suspends the
// app mid-way, or the phone gets too hot, the note keeps whatever was finished and is resumed
// automatically later; the user can also tap "Generate notes". Cut off with the app not in front, the note goes on
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
            case .transcribing(let p): return String(localized: "Transcribing… \(Int(p * 100))%")
            case .loadingModel: return String(localized: "Loading AI model…")
            case .extracting: return String(localized: "Finding reminders and tasks…")
            case .sections(let d, let t):
                return String(localized: "Summarising part \(min(d + 1, max(t, 1))) of \(max(t, 1))…",
                              comment: "Part number, total parts of a long recording")
            case .generating(let k, let phase, let d, let t):
                return phase == "chunk" && t > 1 ? String(localized: "Reading part \(d + 1) of \(t) for \(k.title.lowercased())…") : String(localized: "Writing \(k.title.lowercased())…")
            case .done: return String(localized: "Done")
            case .failed(let e): return String(localized: "Failed: \(e)")
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
            if stage.isWorking { ContinuedWork.shared.update(subtitle: stage.label) }
            guard case .failed(let msg) = stage, let id = activeNoteID else { followStage(); return }
            if BackendChoice.failureIsCutOff(aborted: abort.isSet, wentToBackground: wentToBackground, gpuStep: stepOnGPU,
                                             continuedTaskHeld: ContinuedWork.shared.isHeld) {
                // iOS stops the AI model on the GPU once the app is in the background, and an expiry cancels the
                // run, so such a failure is an interruption, not a real error. Resume it later. Anything else
                // (no speech, a failed CPU load) is a real failure, even after the phone locked.
                interrupted.insert(id)
                lastFailure[id] = Self.cutOffMessage
                cutOff(id)
            } else {
                lastFailure[id] = msg
                notifyFailed(id, line: msg)
                RecordingActivities.shared.update(noteID: id, .failed, line: msg, percent: ContinuedWork.shared.percent)
            }
        }
    }

    /// The note being worked on has a recording Live Activity: it shows this stage (failures are handled above).
    private func followStage() {
        guard let id = activeNoteID, RecordingActivities.shared.isFollowing(noteID: id) else { return }
        let line: String
        switch stage {
        case .idle: line = lastFailure[id] ?? Self.waitingMessage
        case .done, .memories: line = noteTitles[id] ?? ""
        default: line = stage.label
        }
        RecordingActivities.shared.update(noteID: id, stage.activityStage, line: line, percent: ContinuedWork.shared.percent)
    }

    /// A run of this note ended: an activity still showing its work ends now, unless the note waits its turn (queued,
    /// shown as such). Every other ending (ready, paused, cut off, failed) has already ended it.
    private func activityRunEnded(_ id: Int64) {
        guard RecordingActivities.shared.isFollowing(noteID: id), !interrupted.contains(id) else { return }
        RecordingActivities.shared.update(noteID: id, .idle, line: lastFailure[id] ?? "", percent: ContinuedWork.shared.percent)
    }
    /// Notes whose processing was cut off (app in the background, phone too hot) or that waited for
    /// the AI to download; resumed one at a time while the app is active.
    private(set) var interrupted: Set<Int64> = []
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
    private(set) var starting = false
    /// Notes the "AI is ready" pass already ran once, so a silent recording is not re-run every launch.
    @ObservationIgnored private var autoTried: Set<Int64> = Set((UserDefaults.standard.array(forKey: "pipeline.autoTried") as? [Int64]) ?? [])
    private(set) var activeNoteID: Int64?
    /// Last failure message per note, so the note screen can show why nothing was generated.
    private(set) var lastFailure: [Int64: String] = [:]
    /// Any model work, memories included: nothing else that runs the model may start.
    var isWorking: Bool { starting || stage.isWorking }
    /// What the person's controls (Ask, Regenerate, Try again, Generate notes) treat as busy: memory work never is,
    /// it stops for them (yieldMemories).
    var isBusy: Bool { PersonFirst.controlsBusy(working: isWorking, memoriesOnly: memoryWork) }

    private var bgTask: UIBackgroundTaskIdentifier = .invalid
    /// Set when the continued-processing task expires: cancels the model run in flight.
    @ObservationIgnored private let abort = AtomicFlag()

    /// A note's work cut off with the phone locked: it finishes by itself later (the overnight task) or when opened.
    static var cutOffMessage: String { String(localized: "Paused when the phone locked. It will finish by itself later, or when you open Summaverick.") }
    static var heatMessage: String { String(localized: "Paused to let the phone cool down. It will continue automatically.") }
    static var waitingMessage: String { String(localized: "Waiting for Summaverick's AI to finish downloading") }
    /// Stopped while another recording's notes are being written.
    static var queuedMessage: String { String(localized: "Waiting for the earlier recording to finish") }
    nonisolated static var overnightMessage: String { String(localized: "Waiting for the charger — finishes overnight") }
    nonisolated private static var thermal: ProcessInfo.ThermalState { ProcessInfo.processInfo.thermalState }
    /// Critical: the only heat that cancels a model run in the middle.
    nonisolated private static var critical: Bool { Heat.cancelsRun(thermal) }
    /// Notes paused at critical; they carry on only at nominal or fair (others may start at serious).
    @ObservationIgnored private var heatPaused: Set<Int64> = []
    /// True while the only work is memories (after a note's notes, or a backfill): it yields to the person.
    private(set) var memoryWork = false
    /// Set by the person's action (any thread reads it): the memory section in flight is cancelled, the run stops.
    @ObservationIgnored private let memoryStop = AtomicFlag()
    /// Notes whose memory model call failed (-3) in this launch: backfills (in front and overnight) skip them, so one
    /// such note never holds up the others. Tried again at the next launch.
    @ObservationIgnored private var memoryFailed: Set<Int64> = []
    /// A person's action on this note could not start (other work still ran): the note shows the busy line.
    private(set) var busyNotice: Int64?
    /// Set by Settings while "Delete everything" or "Remove AI" runs: no memory backfill starts meanwhile.
    var memoriesHeld = false
    /// Set when the app stops being active (read on any thread): cancels a memory run on the GPU before iOS refuses
    /// GPU work in the background, where a failed reply would mark the section done with no memories.
    @ObservationIgnored private let leftFront = AtomicFlag()
    @ObservationIgnored private var resignObserver: NSObjectProtocol?
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
    /// Any thread (OvernightWork's request): a note cut off after Stop is waiting to finish by itself.
    nonisolated static var automaticFinishPending: Bool {
        !((UserDefaults.standard.array(forKey: automaticKey) as? [Int64]) ?? []).isEmpty
    }
    /// The run in flight writes a note's notes (process, or resuming them; not a regenerate or a backfill): only
    /// such a run is enrolled after a cut-off and notifies ready, paused and failed.
    @ObservationIgnored private var notesRun = false
    /// Notes whose "paused" notification was posted for the cut-off of their current run (posted once).
    @ObservationIgnored private var pausedPosted: Set<Int64> = []
    /// Titles of the notes this launch worked on or queued, so a notification never waits for the core (busy until
    /// the cancelled model call stops) before it takes its background time.
    @ObservationIgnored private var noteTitles: [Int64: String] = [:]
    /// The model step running (or that just failed) is on the GPU: its failure in the background is a cut-off.
    @ObservationIgnored private var stepOnGPU = false
    /// After Stop: the note whose work left is long enough to offer finishing it overnight.
    private(set) var overnightOffer: Int64?
    /// "Yes" tapped while the note is being written: it stops before its next piece or stage.
    @ObservationIgnored private var parkRequested: Set<Int64> = []
    /// Notes that stopped for the night in this run (set by the heat gate, read once the run ends).
    @ObservationIgnored private var parked: Set<Int64> = []
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
        let leftFront = self.leftFront
        resignObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in leftFront.set(true) }
        // Cooled down: carry on with the notes that were paused for heat.
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, Heat.canResume(Self.thermal), self.appIsActive else { return }
                Task { await self.resumeInterrupted() }
            }
        }
    }

    /// Critical: the note is paused and resumes once the phone is back to nominal or fair.
    private func pausedForHeat(_ id: Int64) -> Bool {
        guard Heat.beforeRun(Self.thermal) == .pause else { return false }
        NSLog("[sve] pausing note %lld: thermal state %ld", id, ProcessInfo.processInfo.thermalState.rawValue)
        interrupted.insert(id)
        heatPaused.insert(id)
        lastFailure[id] = Self.heatMessage
        stage = .paused(Self.heatMessage)
        if notesRun { cutOff(id) }   // in the background: finishes by itself later, "paused" once
        return true
    }

    /// Before each piece or stage. Serious: rest 30 s (cut short by an abort or a cooler phone), then go on,
    /// so progress is always made, at a lower duty cycle. Critical: pause the note (false).
    private func heatGate(_ id: Int64) async -> Bool {
        if parkedForNight(id) { return false }
        switch Heat.beforeRun(Self.thermal) {
        case .go: return true
        case .pause: return !pausedForHeat(id)
        case .rest:
            let before = stage
            stage = .cooling
            NSLog("[sve] note %lld: phone is warm, resting %.0f s", id, Heat.restSeconds)
            let end = Date().addingTimeInterval(Heat.restSeconds)
            while Date() < end, !abort.isSet, Heat.beforeRun(Self.thermal) == .rest {
                do { try await Task.sleep(nanoseconds: 500_000_000) } catch { break }
            }
            if pausedForHeat(id) { return false }
            stage = before
            return true
        }
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
            // Keyed on the file name: the note id is not known until the engine is free.
            ContinuedWork.shared.begin(key: audio.deletingPathExtension().lastPathComponent, subtitle: Stage.transcribing(0).label)
            if !isWorking {
                starting = true
                claimed = true
                activeNoteID = nil   // the note is about to exist; no other note shows as busy
                beginBackground()
                stage = .transcribing(0)
            }
        }
        Task { await self.process(audio: audio, duration: duration, claimed: claimed) }
    }

    /// Full run for a fresh recording. Returns the note id as soon as it exists. claimed: recordingStopped
    /// already holds the busy claim and the background time for this run.
    @discardableResult
    func process(audio: URL, duration: Int64, claimed: Bool = false) async -> Int64? {
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
            let line = String(localized: "Could not save the note.")
            // Only this run's own claim shows the failure as the stage (and ends it, so nothing stays busy). Another
            // note running keeps its stage and its Live Activity; only this recording's activity ends.
            if claimed { stage = .failed(line) }
            RecordingActivities.shared.update(audioPath: audio.path, .failed, line: line, percent: 0)
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
        noteTitles[id] = title
        lastFailure[id] = nil
        memoriesMayBeDue = true
        NotificationCenter.default.post(name: .notesChanged, object: nil)

        // Live transcription finishes the piece in flight and hands over; this run continues from its cursor.
        await LiveTranscriber.shared.finish(audioPath: audio.path)
        // Not downloaded yet: the note waits and is processed when the AI is ready (processWaitingNotes).
        guard ModelManager.shared.isReady else {
            RecordingActivities.shared.update(noteID: id, .waitingForAI, line: Self.waitingMessage, percent: 0)
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
        if !(await heatGate(id)) { return id }

        // 1. transcript
        stage = .transcribing(0)
        let done: TranscriptSidecar
        do {
            done = try await transcribe(audio, noteID: id, durationSeconds: duration)
        } catch {
            if parked.contains(id) { return id }   // stopped for the night; the cursor is kept
            NSLog("[sve] transcription failed: %@", String(describing: error))
            if !pausedForHeat(id) { stage = .failed(error.localizedDescription) }
            return id
        }
        let pieces = TranscriptRun.pieces(done)
        let transcript = Self.joined(pieces)
        await store(done, noteID: id, transcript: transcript)
        TranscriptSidecar.remove(audioPath: audio.path)   // complete and saved to the note
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        if transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            stage = .failed(String(localized: "No speech was detected in the recording."))
            return id
        }
        // The transcript is the note's and the sidecar is gone: nothing is handed over any more, so this recording
        // no longer holds the task for itself (notesWritten).
        handingOver.remove(audio.path)

        // 2. notes
        await generateOutputs(noteID: id, transcript: transcript, lang: noteLang, setTitle: true)
        return id
    }

    /// "Generate notes" on a note: memory work yields first; other work still running shows the busy line.
    func regenerate(noteID: Int64) async {
        let asked = personTapped(noteID, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(noteID, asked: asked) }
        guard await personMayStart(noteID) else { return }
        await rerun(noteID: noteID)
    }

    /// Re-runs the AI stage for an existing note (resume after interruption, or after a model change). Internal
    /// callers (the resume loop, Do it now, Try again) come here directly: no busy line without a tap.
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
                                     outputs: OutputKind.allCases.map { note.output($0) != nil }) else {
            RecordingActivities.shared.update(noteID: noteID, .ready, line: note.title, percent: 100)
            return
        }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        startNotesRun(noteID)
        defer { notesRun = false }
        if !(await heatGate(noteID)) { return }

        // No transcript yet (earlier transcription failed): redo it from the audio if we still have it.
        var t = note.transcript ?? ""
        if t.isEmpty {
            guard let path = AudioRecorder.existingAudioPath(note.audio_path) else {
                stage = .failed(String(localized: "No transcript and the audio is no longer available.")); return
            }
            stage = .transcribing(0)
            let done: TranscriptSidecar
            do {
                done = try await transcribe(URL(fileURLWithPath: path), noteID: noteID, durationSeconds: note.duration_seconds)
                t = Self.joined(TranscriptRun.pieces(done))
            } catch {
                if parked.contains(noteID) { return }   // stopped for the night; the cursor is kept
                NSLog("[sve] transcription failed: %@", String(describing: error))
                if !pausedForHeat(noteID) { stage = .failed(error.localizedDescription) }
                return
            }
            if t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                stage = .failed(String(localized: "No speech was detected in the recording.")); return
            }
            await store(done, noteID: noteID, transcript: t)
            TranscriptSidecar.remove(audioPath: path)   // complete and saved to the note
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        } else if let path = AudioRecorder.existingAudioPath(note.audio_path) {
            TranscriptSidecar.remove(audioPath: path)   // left over from before the transcript was saved
        }
        await generateOutputs(noteID: noteID, transcript: t, lang: AppSettings.shared.notesLanguage, setTitle: note.title.hasPrefix("Recording "))
    }

    // MARK: regenerate one output, or all

    /// A "Regenerate" asked for: one output, or (kind nil) the title and all three outputs.
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
    private(set) var regenerating: Regeneration?
    /// The last regenerate of a note that failed (error, heat, cut off by iOS): the note shows "Try again".
    private(set) var regenerateFailed: [Int64: Regeneration] = [:]
    @ObservationIgnored private var regenerateStopped = false

    /// "Regenerate" (one output) or "Regenerate all" (kind nil: title and the three outputs; not reminders and
    /// tasks, and the sections are reused, not rewritten). Same source rule, heat rule and backend as the first
    /// time, with sampling, so it gives a new version. The old text stays until everything asked for is written,
    /// then it is replaced at once; after a failure or Stop nothing changes. Returns the replaced text (for Undo)
    /// on success.
    @discardableResult
    func regenerateOutput(noteID id: Int64, kind: OutputKind?) async -> Previous? {
        let asked = personTapped(id, subtitle: Stage.loadingModel.label)   // before any await: the app is in front
        defer { personRunEnded(id, asked: asked) }
        guard await personMayStart(id) else { return nil }
        starting = true
        defer { starting = false }
        let request = Regeneration(noteID: id, kind: kind)
        regenerateFailed[id] = nil
        let engine = CoreEngine.shared
        guard let note = await engine.note(id) else { return nil }   // deleted
        let transcript = note.transcript ?? ""
        guard ModelManager.shared.isReady, !transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            regenerateFailed[id] = request; return nil
        }
        beginBackground()
        defer { endBackground() }
        activeNoteID = id
        regenerating = request
        regenerateStopped = false
        // A regenerate never queues the note for resuming nor changes its resume banner: put those back after.
        let wasQueued = interrupted.contains(id), wasHeatPaused = heatPaused.contains(id), failure = lastFailure[id]
        // In the language the note was written in (as section backfill does).
        let written = await writeAgain(request, transcript: transcript, lang: note.lang)
        if !wasQueued { interrupted.remove(id) }
        if !wasHeatPaused { heatPaused.remove(id) }
        lastFailure[id] = failure
        regenerating = nil

        let outcome = RegenerateRules.outcome(written: written != nil, stopped: regenerateStopped)
        if outcome == .replaced, let written, let now = await engine.note(id) {   // deleted meanwhile: nothing is written
            var previous = Previous(title: nil, outputs: [:])
            if let t = written.title { previous.title = now.title; await engine.setTitle(id, t); noteTitles[id] = t }
            for k in request.kinds {
                guard let text = written.outputs[k] else { continue }
                if let old = now.output(k) { previous.outputs[k] = old }
                await engine.setOutput(id, k, text)
            }
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            if noteTitles[id] == nil { noteTitles[id] = now.title }
            // Written with the app closed: "ready", as for the first notes (nothing is posted with the app in front).
            notify(id) { NotesNotifier.notesReady(noteID: id, title: $0, hold: $1) }
            stage = .done
            return previous
        }
        if outcome == .failed { regenerateFailed[id] = request }
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
        let covered = SectionWriter.coversAll(await engine.sections(id), pieceCount: pieces.count)
        let fromSections = await notesFromSections(covered: covered, transcript: transcript, lang: lang)
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
            do {
                let step = done
                let text = try await writeOutput(kind, noteID: id, transcript: transcript, lang: lang, fromSections: fromSections,
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
        defer { personRunEnded(id, asked: asked) }
        guard await personMayStart(id) else { return }
        interrupted.remove(id)
        heatPaused.remove(id)
        await rerun(noteID: id)
    }

    /// Audio → text, on the CPU, from where live transcription (or an interrupted run) stopped. The model
    /// hears the recording itself, so any language mix works. Stops (the progress callback cancels) as
    /// soon as the phone gets too hot or the background time runs out; the caller treats that as a pause,
    /// and the next run continues from the saved cursor.
    private func transcribe(_ audio: URL, noteID: Int64, durationSeconds: Int64) async throws -> TranscriptSidecar {
        stepOnGPU = false   // transcription always runs on the CPU
        let duration = Double(max(1, durationSeconds))
        func report(_ s: TranscriptSidecar) {
            // The fraction first: the stage change sends it to the recording's Live Activity.
            ContinuedWork.shared.update(fraction: NotesProgress.fraction(transcribedSeconds: s.transcribedSeconds, durationSeconds: duration))
            stage = .transcribing(min(1, s.transcribedSeconds / duration))
        }
        report(TranscriptSidecar.load(audioPath: audio.path))   // from where live transcription got to
        let abort = self.abort
        let (done, stop) = try await TranscriptRun.run(audioPath: audio.path, isFinal: true,
                                                       shouldStop: { ContinuedWork.beat(); return Self.critical || abort.isSet },
                                                       beforePiece: { await self.heatGate(noteID) },
                                                       onPiece: { report($0) })
        if stop == .yielded { throw CancellationError() }   // paused for heat; the cursor is kept
        return done
    }

    /// A finished transcription into the note: pieces, transcript, and the sections written while recording
    /// (those that still fit the pieces; replacing whatever the note had, which belonged to older pieces).
    private func store(_ done: TranscriptSidecar, noteID id: Int64, transcript: String) async {
        let engine = CoreEngine.shared
        let pieces = TranscriptRun.pieces(done)
        await engine.setPieces(id, pieces)
        await engine.setTranscript(id, transcript)
        await engine.setSections(id, NoteSection.validPrefix(done.sections, pieceCount: pieces.count))
    }

    private static func joined(_ pieces: [TranscriptPiece]) -> String {
        pieces.map(\.text).joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func loadSelectedModel() async throws {
        stage = .loadingModel
        expectBackend()
        stepOnGPU = try await ModelManager.shared.loadIntoCore(for: .generation) == .gpu
    }

    /// Before a load that may throw: the backend it is about to use (as ModelManager picks it).
    private func expectBackend() {
        stepOnGPU = BackendChoice.backend(for: .generation, appActive: UIApplication.shared.applicationState != .background,
                                          recording: LiveTranscriber.shared.isRecording) == .gpu
    }

    /// One model step on the backend for right now (GPU in front, else CPU). A GPU run cut off by the app
    /// going to the background is retried once on the CPU while a continued-processing task is held.
    private func onBackend<T>(_ body: () async throws -> T) async throws -> T {
        expectBackend()
        let backend = try await ModelManager.shared.loadIntoCore(for: .generation)
        stepOnGPU = backend == .gpu
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
        // Memories run after the notes are done: stopping them leaves nothing to resume or show.
        var running: Int64?
        if let r = regenerating, !appIsActive {
            // A regenerate is not resumed (the overnight task only finishes notes that are missing): say so.
            NSLog("[sve] bg: note %lld: regenerate cut off at %ld%%; the old text is kept", r.noteID, ContinuedWork.shared.percent)
            notify(r.noteID) { NotesNotifier.regenerateStopped(noteID: r.noteID, title: $0, hold: $1) }
        }
        if isWorking, !memoryWork, let id = activeNoteID {
            interrupted.insert(id)
            lastFailure[id] = Self.cutOffMessage
            cutOff(id)
            running = id
        }
        // Notes waiting their turn (queued behind this one, waiting for the AI, cut off or paused for heat earlier):
        // every queued note is notes work, and nothing else would run them now.
        for id in interrupted.sorted() where id != running { cutOff(id, queued: true) }
    }

    /// A notes run starts for this note: it may be cut off (and notify) again.
    private func startNotesRun(_ id: Int64) {
        notesRun = true
        pausedPosted.remove(id)
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
        notify(id) { NotesNotifier.paused(noteID: id, title: $0, percent: percent, hold: $1) }
    }

    /// Takes the background time now, before any await, then posts with the note's title: the one known from this
    /// launch at once, else read from the core.
    private func notify(_ id: Int64, _ post: @escaping (String, NotesNotifier.Hold) -> Void) {
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

    /// Steps after transcription, for the lock-screen progress: reminders, sections, title, the three outputs.
    private static let notesSteps = 3 + OutputKind.allCases.count
    private static let sectionsStep = 1
    private static let firstOutputStep = 3

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
            stage = .failed(error.localizedDescription)
            return
        }

        // Reminders, tasks and profile changes first, so they are ready even if note writing is interrupted.
        // A failure here never fails the notes.
        let abort = self.abort
        let steps = Self.notesSteps   // reminders, sections, title, three outputs
        ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 0, outputsTotal: steps))
        do {
            if await engine.pieces(id).isEmpty {   // note from before pieces existed
                let dur = await engine.note(id)?.duration_seconds ?? 0
                await engine.setPieces(id, PiecesBackfill.pieces(transcript: transcript, durationSeconds: dur))
            }
            stage = .extracting
            do {
                let n = try await onBackend {
                    try await engine.extract(noteID: id, shouldStop: { ContinuedWork.beat(); return abort.isSet })
                }
                NSLog("[sve] extract added %d suggestions", n)
            } catch {
                NSLog("[sve] extract failed: %@", String(describing: error))
            }
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
        }
        if abort.isSet { stage = .failed(CoreError.generationFailed.localizedDescription); return }
        ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 1, outputsTotal: steps))

        // Section summaries the note does not have yet (all of them for a note from before sections; the chapters
        // come from them). Notes and title come from the transcript, exactly as before sections, whenever it fits
        // one prompt; from the sections only when it does not and they cover the whole recording.
        guard let covered = await writeSections(noteID: id, lang: lang, quiet: false) else { return }
        ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 2, outputsTotal: steps))
        let fromSections = await notesFromSections(covered: covered, transcript: transcript, lang: lang)
        if setTitle, !(await heatGate(id)) { return }

        if setTitle, let t = await writeTitle(noteID: id, transcript: transcript, lang: lang, fromSections: fromSections) {
            await engine.setTitle(id, t)
            noteTitles[id] = t
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }
        ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: 3, outputsTotal: steps))

        for (i, kind) in OutputKind.allCases.enumerated() {
            if abort.isSet { stage = .failed(CoreError.generationFailed.localizedDescription); return }
            if !(await heatGate(id)) { return }
            do {
                let step = Self.firstOutputStep + i, total = Self.notesSteps
                let text = try await writeOutput(kind, noteID: id, transcript: transcript, lang: lang, fromSections: fromSections,
                                                 fraction: { NotesProgress.fraction(outputsDone: step, part: $0, outputsTotal: total) })
                await engine.setOutput(id, kind, text)
                NotificationCenter.default.post(name: .notesChanged, object: nil)
                ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: Self.firstOutputStep + 1 + i, outputsTotal: steps))
            } catch {
                stage = .failed(error.localizedDescription)
                return
            }
        }
        // The notes are stored: "ready" now (when the app is not in front), not after the memories.
        NSLog("[sve] bg: note %lld: notes ready (app %@)", id, appIsActive ? "in front" : "not in front")
        notify(id) { NotesNotifier.notesReady(noteID: id, title: $0, hold: $1) }
        RecordingActivities.shared.update(noteID: id, .ready, line: noteTitles[id] ?? "", percent: 100)
        notesWritten(id)
        // Memories now, never before them; a stop or failure here never fails the notes
        // (sections left are done later, idle in front or overnight).
        memoryStop.set(false)
        _ = await writeMemories(noteID: id, quiet: false)
        if (try? await ModelManager.shared.loadIntoCore(for: .generation)) != nil {
            _ = await engine.generateInsight(noteID: id)
        }
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        stage = .done
        Task { await self.compressAudio(noteID: id) }   // the notes are done; the WAV can shrink
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
        // Every few tokens (core thread): the stage line, the chunk progress, and the heartbeat.
        let progress: (String, Int, Int) -> Void = { phase, done, total in
            ContinuedWork.beat()
            Task { @MainActor in
                self.stage = .generating(kind, phase, done, total)
                let part = total > 0 ? Double(done) / Double(total) : 0
                ContinuedWork.shared.update(fraction: fraction(part))
            }
        }
        return try await onBackend { () async throws -> String in
            if fromSections, let t = await engine.generateFromSections(noteID: id, kind: kind, lang: lang, shouldStop: { abort.isSet },
                                                                       onProgress: progress) { return t }
            // nil: cancelled (our flag) or the sections could not be used, then the transcript as before.
            if abort.isSet { throw CoreError.generationFailed }
            return try await engine.generate(transcript: transcript, kind: kind, lang: lang, shouldStop: { abort.isSet }, onProgress: progress)
        }
    }

    /// Writes the note's missing section summaries from its stored pieces, one at a time, each stored before the
    /// next (a note deleted meanwhile refuses the write, which ends this). nil: stopped, the caller returns —
    /// paused for heat (critical), cut off, or the note is gone; quiet: a recording started, the screen was
    /// left, or the phone got warm, and nothing is shown or queued. Otherwise whether the sections now cover
    /// every piece (false after a failure: the notes then come from the transcript, as before sections).
    /// quiet (backfill when an older note is opened): no failure banner, no pause queue, stops at critical.
    private func writeSections(noteID id: Int64, lang: String, quiet: Bool) async -> Bool? {
        let engine = CoreEngine.shared
        let pieces = await engine.pieces(id)
        guard !pieces.isEmpty else { return false }
        let existing = await engine.sections(id)
        if SectionWriter.coversAll(existing, pieceCount: pieces.count) { return true }
        let duration = await engine.note(id)?.duration_seconds ?? 0
        let from = NoteSection.covered(NoteSection.validPrefix(existing, pieceCount: pieces.count))
        let total = SectionWriter.plan(pieces: Array(pieces[from...]), endSec: duration, isFinal: true).count
        let abort = self.abort, recording = LiveTranscriber.shared.recordingNow
        var written = 0, gone = false, stopped = false
        /// The lock-screen progress inside the sections step: one share per section.
        func reportSections() {
            let part = Double(written) / Double(max(total, 1))
            ContinuedWork.shared.update(fraction: NotesProgress.fraction(outputsDone: Self.sectionsStep, part: part, outputsTotal: Self.notesSteps))
        }
        func stop() -> Bool { abort.isSet || (quiet && LiveTranscriber.shared.isRecording) || Task.isCancelled }
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
                    if !quiet { reportSections() }
                    return true
                },
                summarize: { ps, lang in
                    try await onBackend {
                        try await engine.sectionSummarize(pieces: ps, lang: lang, shouldStop: {
                            if !quiet { ContinuedWork.beat() }
                            return abort.isSet || Self.critical || (quiet && recording.isSet)
                        })
                    }
                },
                onSection: { secs in
                    guard await engine.setSections(id, secs) else { gone = true; return false }
                    written += 1
                    if !quiet { reportSections() }
                    NotificationCenter.default.post(name: .notesChanged, object: nil)   // chapters appear as they come
                    return true
                })
            if gone {
                NSLog("[sve] note %lld is gone; not writing its sections", id)
                stage = .idle
                return nil
            }
            if stopped {
                if quiet {
                    stage = .idle
                } else if case .paused = stage {
                    // paused for heat: heatGate set the stage and queued the note
                } else if parked.contains(id) {
                    // stopped for the night: heatGate set the stage
                } else {
                    stage = .failed(CoreError.generationFailed.localizedDescription)   // cut off (abort: resumed later)
                }
                return nil
            }
            return SectionWriter.coversAll(done, pieceCount: pieces.count)
        } catch {
            if quiet {
                NSLog("[sve] section backfill stopped: %@", String(describing: error))
                stage = .idle
                return nil
            }
            if abort.isSet { stage = .failed(CoreError.generationFailed.localizedDescription); return nil }
            if pausedForHeat(id) { return nil }
            NSLog("[sve] section summaries failed, notes come from the transcript: %@", String(describing: error))
            return false
        }
    }

    /// The heat rule for quiet work: serious rests 30 s (cut short by `stop` or a cooler phone), critical stops.
    private func quietHeatGate(_ stop: () -> Bool) async -> Bool {
        switch Heat.beforeRun(Self.thermal) {
        case .go: return true
        case .pause: return false
        case .rest:
            stage = .cooling
            let end = Date().addingTimeInterval(Heat.restSeconds)
            while Date() < end, !stop(), Heat.beforeRun(Self.thermal) == .rest {
                do { try await Task.sleep(nanoseconds: 500_000_000) } catch { return false }
            }
            return !stop() && Heat.beforeRun(Self.thermal) != .pause
        }
    }

    /// An older note opened (made before sections, or its sections were cut off) that already has all its
    /// notes: writes its section summaries from the stored pieces so it gets chapters. Only while nothing else
    /// runs, the app is in front, nothing is recording and the AI is ready; stops quietly when a recording
    /// starts, the phone gets too hot or the caller's task is cancelled (the screen was left), keeping every
    /// finished section. A note that still needs its notes gets its sections from regenerate instead.
    func backfillSections(noteID: Int64) async {
        guard !isWorking, appIsActive, ModelManager.shared.isReady, !LiveTranscriber.shared.isRecording,
              !interrupted.contains(noteID), Heat.beforeRun(Self.thermal) != .pause else { return }
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
    /// at a time, each kept before the next, each after the heat rule for quiet work (serious rests 30 s, critical
    /// stops), on the backend for right now. Memories never hold up notes and never fail them: before each section
    /// it stops when a recording runs, another note's notes may start, the run was cut off, or the person chose
    /// overnight for this note; a recording starting, critical heat or the cut-off cancels the section in flight
    /// (nothing of it is stored; it stays due). It never sets a failure, pauses or queues the note.
    /// quiet: a backfill, not the end of the note's own run (the stage goes back to idle when it ends).
    /// Returns .finished when every section has its memories; .gone when the note is gone (deleted meanwhile: the
    /// run ends quietly); .failed when a section's model call failed (-3: the note is skipped for the rest of this
    /// launch, other notes go on); .stopped for a real stop, or no model (the sections left are done later).
    @discardableResult
    private func writeMemories(noteID id: Int64, quiet: Bool) async -> MemoryPass.Outcome {
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
            var backend: ModelBackend = retried ? .cpu
                : BackendChoice.backend(for: .generation, appActive: appIsActive, recording: LiveTranscriber.shared.isRecording)
            if backend == .gpu {
                leftFront.set(false)
                if !appIsActive { backend = .cpu }   // left the front just now
            }
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
              Heat.beforeRun(Self.thermal) != .pause, nextToResume(tried: []) == nil else { return }
        starting = true
        memoryWork = true
        memoryStop.set(false)
        defer { starting = false; memoryWork = false }
        let limit = PersonFirst.backfillNotes(inFront: overnightRun == nil)
        let needing = await CoreEngine.shared.notesNeedingMemories(limit: limit + memoryFailed.count)
        let ids = MemoryPass.backfillNotes(needing, skipping: memoryFailed, limit: limit)
        guard !ids.isEmpty else { memoriesMayBeDue = false; return }
        guard !LiveTranscriber.shared.isRecording, !Task.isCancelled, !memoryStop.isSet else { return }
        beginBackground()
        UIApplication.shared.isIdleTimerDisabled = false   // background work: the phone may lock (the GPU run stops)
        defer { endBackground() }
        var allDone = ids.count < limit
        for id in ids {
            activeNoteID = id
            // A failed note is skipped (memoryFailed) and the next goes on; a real stop ends the backfill.
            guard MemoryPass.backfillGoesOn(after: await writeMemories(noteID: id, quiet: true)) else { allDone = false; break }
        }
        if allDone { memoriesMayBeDue = false }
        stage = .idle
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

    /// Before a person's action on a note: memory work yields. False when other work still runs; the note then shows
    /// the busy line for a few seconds instead of the tap doing nothing.
    private func personMayStart(_ id: Int64) async -> Bool {
        if busyNotice == id { busyNotice = nil }
        await yieldMemories()
        guard isWorking else { return true }
        busyNotice = id
        Task {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            if self.busyNotice == id { self.busyNotice = nil }
        }
        return false
    }

    /// "Find reminders and tasks" on a note that was never extracted (an older recording): extraction
    /// only, with the same busy guard as a recording.
    func findReminders(noteID: Int64) async {
        let asked = personTapped(noteID, subtitle: Stage.extracting.label)   // before any await: the app is in front
        defer { personRunEnded(noteID, asked: asked) }
        guard await personMayStart(noteID) else { return }
        starting = true
        defer { starting = false }
        guard ModelManager.shared.isReady else { lastFailure[noteID] = Self.waitingMessage; return }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        if Heat.isTooHot(Self.thermal) {
            stage = .failed(String(localized: "The phone is too warm. Try again when it has cooled down.")); return
        }
        let engine = CoreEngine.shared
        guard let note = await engine.note(noteID), let t = note.transcript,
              !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            stage = .failed(String(localized: "No speech was detected in the recording.")); return
        }
        do {
            try await loadSelectedModel()
        } catch {
            stage = .failed(error.localizedDescription); return
        }
        if await engine.pieces(noteID).isEmpty {
            await engine.setPieces(noteID, PiecesBackfill.pieces(transcript: t, durationSeconds: note.duration_seconds))
        }
        stage = .extracting
        do {
            // On the backend for right now, retried once on the CPU when the app is closed mid-run (GPU stopped).
            let n = try await onBackend { try await engine.extract(noteID: noteID, shouldStop: { ContinuedWork.beat(); return false }) }
            NSLog("[sve] extract added %d suggestions", n)
            stage = .done
        } catch {
            NSLog("[sve] extract failed: %@", String(describing: error))
            stage = .failed(error.localizedDescription)
        }
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
    }

    private func beginBackground() {
        wentToBackground = false
        stepOnGPU = false
        // A run of the overnight task that starts after it expired is cancelled at once; any other run starts clean.
        abort.set(overnightOwnsRun.isSet && (overnightRun?.isSet ?? false))
        ContinuedWork.shared.startNote()   // lock-screen progress starts again for this note
        // Keep the screen on while the model works: a locked phone stops GPU work and the run fails.
        UIApplication.shared.isIdleTimerDisabled = true
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
        stoppedWhileNotActive = false   // this run's; a later run is not one stopped from the background
        UIApplication.shared.isIdleTimerDisabled = false
        if bgTask != .invalid { UIApplication.shared.endBackgroundTask(bgTask); bgTask = .invalid }
        // A recording queued behind this run (or a paused note) goes next.
        if !interrupted.isEmpty && !resuming { Task { await self.resumeInterrupted() } }
        // After the caller's other defers (its busy claim) have run.
        else if !resuming { Task { self.finishContinuedWork() } }
    }

    /// Nothing is running and nothing more can run now: release the continued-processing task. Not while a person's
    /// run that asked for it has not ended (it may be waiting for memories to yield, so nothing runs for a moment),
    /// nor while a Stop is still being handed over (it asked for the task and runs, or queues, once handed over).
    private func finishContinuedWork() {
        guard TaskLifecycle.finishes(working: isWorking || !handingOver.isEmpty, personRunsPending: personRunNotes.count,
                                     queued: 0) else { return }
        ContinuedWork.shared.finish(success: interrupted.isEmpty)
    }

    /// A note's notes are stored: the task's work for it is done, so the task ends here unless more work waits for it
    /// (a note queued, a Stop being handed over, a person's run for another note). The memories and the insight
    /// after the notes never hold it: they stop when cut off and are done later. A person's run for this note is
    /// this run (while notes run, no tap asks for the task: PersonRun.asksForContinuedTask).
    private func notesWritten(_ id: Int64) {
        let otherPersonRuns = personRunNotes.count - (personRunNotes.contains(id) ? 1 : 0)
        guard TaskLifecycle.finishes(working: !interrupted.isEmpty || !handingOver.isEmpty,
                                     personRunsPending: otherPersonRuns, queued: 0) else { return }
        ContinuedWork.shared.finish(success: true)
    }

    /// Notes of the person's runs (personTapped) that asked for the continued-processing task and have not ended
    /// yet, one entry per run.
    @ObservationIgnored private var personRunNotes: [Int64] = []

    /// A person's tap on a note (Generate notes, Regenerate, Try again, Do it now, Find reminders), called before the
    /// entry point's first await, while the app is in front: asks for the continued-processing task, as
    /// recordingStopped does, so the run goes on (with lock-screen progress) after the app is closed. Not when the run
    /// cannot start (the AI is not ready, another note's notes run: that run keeps its task). A task already held or
    /// asked for is kept. True when it asked: the caller passes it to personRunEnded.
    private func personTapped(_ id: Int64, subtitle: String) -> Bool {
        guard PersonRun.asksForContinuedTask(appActive: appIsActive, aiReady: ModelManager.shared.isReady,
                                             working: isWorking, memoriesOnly: memoryWork) else { return false }
        personRunNotes.append(id)
        ContinuedWork.shared.begin(key: "note-\(id)", subtitle: subtitle)
        return true
    }

    /// Every exit of a person's entry point (done, failed, stopped, busy, AI not ready, the note deleted): the task
    /// asked for in the tap is released once nothing is left, after the caller's other defers (its busy claim), as
    /// endBackground does. Notes still queued go next (the resume loop releases it when it ends); another run still
    /// working keeps it.
    private func personRunEnded(_ id: Int64, asked: Bool) {
        if asked, let i = personRunNotes.firstIndex(of: id) { personRunNotes.remove(at: i) }
        guard !resuming else { return }   // the resume loop releases it when it ends
        if interrupted.isEmpty { Task { self.finishContinuedWork() } } else { Task { await self.resumeInterrupted() } }
    }

    /// Re-runs cut-off and waiting notes, oldest first, one at a time, while the app is active (or a
    /// continued-processing or the overnight task keeps it running), not recording, the AI is ready and the
    /// phone is not too hot. Each note is tried once per pass, so a note that is paused again (heat,
    /// background) waits for the next pass instead of looping. Notes marked overnight join only while plugged
    /// in or inside the overnight task.
    func resumeInterrupted() async {
        guard !resuming else { return }
        resuming = true
        defer { resuming = false; finishContinuedWork(); endWaitingActivities() }
        var tried: Set<Int64> = []
        while !isWorking, appIsActive || ContinuedWork.shared.isHeld || overnightRun != nil,
              !(overnightRun?.isSet ?? false), !LiveTranscriber.shared.isRecording,
              ModelManager.shared.isReady, Heat.beforeRun(Self.thermal) != .pause,
              let id = nextToResume(tried: tried) {
            tried.insert(id)
            interrupted.remove(id)
            heatPaused.remove(id)
            let owned = OvernightList.ownsRun(inOvernightTask: overnightRun != nil, appActive: appIsActive,
                                              continuedTaskHeld: ContinuedWork.shared.isHeld)
            if !appIsActive {
                NSLog("[sve] bg: resuming note %lld (%@)", id,
                      ContinuedWork.shared.isHeld ? "continued processing" : (overnightRun != nil ? "overnight task" : "background time"))
            }
            overnightOwnsRun.set(owned)
            await rerun(noteID: id)
            overnightOwnsRun.set(false)
            if !interrupted.contains(id) { markAutoTried(id) }
        }
    }

    /// The resume loop stopped with the app not in front and nothing running: notes still waiting their turn will not
    /// start until the app is opened (or the overnight task runs), so their Live Activities end as paused instead of
    /// staying on "Waiting for the earlier recording to finish".
    private func endWaitingActivities() {
        guard !appIsActive, !isWorking else { return }
        for id in interrupted.sorted() where RecordingActivities.shared.isFollowing(noteID: id) {
            RecordingActivities.shared.update(noteID: id, .cutOff, line: lastFailure[id] ?? Self.cutOffMessage, percent: 0)
        }
    }

    /// The oldest note that may start now: cut off or waiting, or marked overnight while plugged in or inside
    /// the overnight task; never one paused at critical until the phone is back to nominal or fair.
    private func nextToResume(tried: Set<Int64>) -> Int64? {
        let inTask = overnightRun != nil, plugged = Self.pluggedIn, onPower = OvernightWork.shared.runOnPower
        let marked = Set(overnight)
        let automaticMarked = marked.intersection(automatic)   // cut-off notes kept across launches: always candidates
        return interrupted.union(automaticMarked).union(inTask || plugged ? marked : []).sorted().first(where: {
            !tried.contains($0)
                && OvernightList.mayStart(mark: OvernightList.mark(onList: marked.contains($0), automatic: automatic.contains($0)),
                                          pluggedIn: plugged, inOvernightTask: inTask, taskOnPower: onPower)
                && (!heatPaused.contains($0) || Heat.canResume(Self.thermal))
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
        for n in await engine.notes(limit: 10_000) where n.has_audio && !autoTried.contains(n.id) {
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
        defer { personRunEnded(id, asked: asked) }
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
        heatPaused.remove(id)
        lastFailure[id] = nil
        regenerateFailed[id] = nil
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
            memoriesLeft = !MemoryPass.backfillNotes(needing, skipping: memoryFailed, limit: 1).isEmpty
        }
        return !overnight.isEmpty || !interrupted.isEmpty || memoriesLeft
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
