// Small pure rules behind the AI download, the device check, heat, the waiting-notes queue and the memory pass.
// Foundation only and free of app state, so they can be checked with a standalone swiftc script.
import Foundation

enum DownloadMath {
    static let gigabyte: Int64 = 1_000_000_000   // decimal GB, as iOS Settings > Storage counts

    /// Combined progress over several files, weighted by bytes. 0 when nothing is known.
    static func progress(_ parts: [(done: Int64, total: Int64)]) -> Double {
        let total = parts.reduce(Int64(0)) { $0 + max(0, $1.total) }
        guard total > 0 else { return 0 }
        let done = parts.reduce(Int64(0)) { $0 + min(max(0, $1.done), max(0, $1.total)) }
        return Double(done) / Double(total)
    }

    /// Bytes still to download over all files.
    static func remaining(_ parts: [(done: Int64, total: Int64)]) -> Int64 {
        parts.reduce(Int64(0)) { $0 + max(0, $1.total - max(0, $1.done)) }
    }

    /// "about X min left": nil until a speed is known; at least 1.
    static func minutesLeft(remainingBytes: Int64, bytesPerSecond: Double) -> Int? {
        guard bytesPerSecond > 0, remainingBytes > 0 else { return nil }
        return max(1, Int((Double(remainingBytes) / bytesPerSecond / 60).rounded(.up)))
    }
}

enum SpaceCheck {
    /// Head room kept free on top of the download itself.
    static let margin: Int64 = DownloadMath.gigabyte

    /// True when the download fits: available ≥ remaining + 1 GB. Unknown free space never blocks.
    static func hasRoom(available: Int64?, remaining: Int64) -> Bool {
        guard let available else { return true }
        return available >= remaining + margin
    }

    /// Whole GB to free ("Free up about N GB"), rounded up, at least 1. 0 when it already fits.
    static func gigabytesToFree(available: Int64?, remaining: Int64) -> Int {
        guard let available, !hasRoom(available: available, remaining: remaining) else { return 0 }
        let short = remaining + margin - available
        return max(1, Int((Double(short) / Double(DownloadMath.gigabyte)).rounded(.up)))
    }
}

enum RetrySchedule {
    /// Wait before the next automatic try after `failures` failures in a row (30 s, 2 min, 10 min).
    /// nil: stop retrying in this launch; the next launch starts again.
    static func delay(afterFailures failures: Int) -> TimeInterval? {
        let steps: [TimeInterval] = [30, 120, 600]
        guard failures >= 1, failures <= steps.count else { return nil }
        return steps[failures - 1]
    }
}

enum DeviceGate {
    /// Physical memory a phone must report to run the AI. iPhones ship with 4, 6 or 8 GB; a 6 GB
    /// iPhone reports a little under 6 GiB (the system keeps some), so the line sits well between
    /// 4 GB and 6 GB phones rather than at 5.5 GiB, where a 6 GB phone could fall just below.
    static let minimumGiB = 5.0

    static func isSupported(physicalMemory: UInt64) -> Bool {
        Double(physicalMemory) / 1_073_741_824 >= minimumGiB
    }
}

enum Heat {
    /// Serious or critical: too warm to start a one-off model run the person asked for (Ask, topic summary).
    static func isTooHot(_ state: ProcessInfo.ThermalState) -> Bool {
        state == .serious || state == .critical
    }

    /// What long work (transcription pieces, note stages) does before each piece or stage.
    enum Step: Equatable {
        /// Start now.
        case go
        /// Serious: rest `restSeconds`, then start anyway, so progress is always made, just slower.
        case rest
        /// Critical: stop; carry on once the phone is back to nominal or fair.
        case pause
    }

    /// Rest before a piece or stage while the phone is at serious.
    static let restSeconds: Double = 30

    static func beforeRun(_ state: ProcessInfo.ThermalState) -> Step {
        switch state {
        case .critical: return .pause
        case .serious: return .rest
        default: return .go
        }
    }

    /// Only critical cancels a piece or stage in the middle; serious lets it finish (cancelling at serious
    /// threw away the same piece again and again and made no progress).
    static func cancelsRun(_ state: ProcessInfo.ThermalState) -> Bool { state == .critical }

    /// A note paused at critical carries on only once the phone is back to nominal or fair.
    static func canResume(_ state: ProcessInfo.ThermalState) -> Bool { state == .nominal || state == .fair }
}

enum SizeCheck {
    /// The full size the finished file must have: the total of a Content-Range ("bytes 0-9/10") on a
    /// resumed (206) response, Content-Length on a 200, else the size recorded while downloading.
    static func expectedSize(status: Int, contentLength: Int64, contentRange: String?, recorded: Int64?) -> Int64? {
        if status == 206, let r = contentRange, let slash = r.lastIndex(of: "/"),
           let total = Int64(r[r.index(after: slash)...].trimmingCharacters(in: .whitespaces)), total > 0 {
            return total
        }
        if status == 200, contentLength > 0 { return contentLength }
        if let recorded, recorded > 0 { return recorded }
        return nil
    }

    /// Unknown expected size is accepted (nothing to compare with); otherwise sizes must be equal.
    static func isComplete(actual: Int64, expected: Int64?) -> Bool {
        guard let expected else { return actual > 0 }
        return actual == expected
    }
}

enum WaitingNotes {
    struct Candidate: Equatable {
        let id: Int64
        let hasAudio: Bool
        let hasTranscript: Bool
    }

    /// Notes to process once the AI is ready: audio still there, no transcript yet, not already
    /// tried automatically (so a silent recording is not re-run at every launch) and not the note
    /// being processed right now. Oldest first.
    static func toProcess(_ notes: [Candidate], alreadyTried: Set<Int64>, running: Int64? = nil) -> [Int64] {
        notes.filter { $0.hasAudio && !$0.hasTranscript && !alreadyTried.contains($0.id) && $0.id != running }
            .map(\.id)
            .sorted()
    }

    /// A queued note still needs the model unless it has a transcript and all three outputs.
    static func needsWork(hasTranscript: Bool, outputs: [Bool]) -> Bool {
        !(hasTranscript && outputs.count == 3 && outputs.allSatisfy { $0 })
    }
}

/// Where a model run goes. iOS refuses GPU work from the background (even while recording), so
/// transcription always runs on the CPU, and everything else only uses the GPU while the app is in front.
enum ModelBackend: Equatable {
    case gpu, cpu

    /// Layers given to the GPU: all of them (Metal), or none.
    var gpuLayers: Int32 { self == .gpu ? 99 : 0 }
    /// 4 threads on the CPU (measured: 6 is slower on the phone); 0 = the core's own choice for the GPU.
    var threads: Int32 { self == .cpu ? 4 : 0 }
}

enum ModelPurpose: Equatable {
    /// Recording -> text (live and after Stop).
    case transcription
    /// Notes, titles, extraction, Ask.
    case generation
}

enum BackendChoice {
    /// Picked right before each model run, never during one.
    static func backend(for purpose: ModelPurpose, appActive: Bool, recording: Bool) -> ModelBackend {
        switch purpose {
        case .transcription: return .cpu
        case .generation: return appActive && !recording ? .gpu : .cpu
        }
    }

    /// A GPU run that failed because the app went to the background is retried once on the CPU, but
    /// only while a continued-processing task keeps the app running; otherwise the note is interrupted.
    /// A notes run's failure is a cut-off (the note resumes later; "paused"), not a real failure, only when the run
    /// was cancelled (expiry, the overnight task ended) or the GPU was stopped in the background with no
    /// continued-processing task to retry it on the CPU. Anything else (no speech, a failed CPU load) is a failure.
    static func failureIsCutOff(aborted: Bool, wentToBackground: Bool, gpuStep: Bool, continuedTaskHeld: Bool) -> Bool {
        aborted || (wentToBackground && gpuStep && !continuedTaskHeld)
    }

    static func retryOnCPU(failedOn backend: ModelBackend, wentToBackground: Bool, continuedTaskHeld: Bool,
                           cancelled: Bool, alreadyRetried: Bool) -> Bool {
        backend == .gpu && wentToBackground && continuedTaskHeld && !cancelled && !alreadyRetried
    }
}

enum LivePause: Equatable {
    case heat, battery

    /// Below this battery level (and not charging) live transcription waits; the recording continues.
    static let lowBattery: Float = 0.20

    /// Why live transcription should wait now, nil to go on. Heat (critical only; serious rests between
    /// pieces instead, see Heat.beforeRun) comes first. batteryLevel is 0…1, or negative when unknown
    /// (simulator, monitoring off), which never pauses.
    /// heatPaused: already paused for heat, which lasts until nominal or fair (not just below critical).
    static func reason(thermal: ProcessInfo.ThermalState, batteryLevel: Float, charging: Bool,
                       heatPaused: Bool = false) -> LivePause? {
        if Heat.beforeRun(thermal) == .pause || (heatPaused && !Heat.canResume(thermal)) { return .heat }
        if batteryLevel >= 0, batteryLevel < lowBattery, !charging { return .battery }
        return nil
    }
}

enum LiveLag {
    /// One piece is always in the making (28 s + 3 s wait, then about as long again to write it), so
    /// up to this much lag still counts as "writing as you talk".
    static let keepingUpSeconds: Double = 90

    /// Whole minutes behind ("About N min behind"), nil while keeping up.
    static func minutesBehind(recordedSeconds: Double, transcribedSeconds: Double) -> Int? {
        let behind = recordedSeconds - transcribedSeconds
        guard behind > keepingUpSeconds else { return nil }
        return max(1, Int((behind / 60).rounded()))
    }
}

enum NotesProgress {
    /// Share of the work after Stop that is transcription; the rest is reminders, title and the three outputs.
    static let transcriptionShare = 0.7

    /// 0…1 for the lock-screen progress. transcribed / duration in seconds during transcription;
    /// afterwards `outputsDone` of `outputsTotal` steps (extraction, then each output).
    static func fraction(transcribedSeconds: Double, durationSeconds: Double) -> Double {
        guard durationSeconds > 0 else { return 0 }
        return transcriptionShare * min(1, max(0, transcribedSeconds / durationSeconds))
    }

    static func fraction(outputsDone: Int, outputsTotal: Int) -> Double {
        fraction(outputsDone: outputsDone, part: 0, outputsTotal: outputsTotal)
    }

    /// Inside a step: `part` (0…1) of step `outputsDone` is done (a section of the sections, a chunk of an
    /// output). Part 1 is where the next step starts.
    static func fraction(outputsDone: Int, part: Double, outputsTotal: Int) -> Double {
        guard outputsTotal > 0 else { return 1 }
        let done = Double(outputsDone) + min(1, max(0, part))
        return transcriptionShare + (1 - transcriptionShare) * min(1, max(0, done / Double(outputsTotal)))
    }

    /// iOS may end a continued-processing task whose progress stands still, so the lock-screen progress
    /// moves at least this often while the model works.
    static let heartbeatSeconds: TimeInterval = 30

    /// The progress has not moved for `heartbeatSeconds`: the next progress callback from the model moves it.
    static func heartbeatDue(lastReport: Date, now: Date) -> Bool {
        now.timeIntervalSince(lastReport) >= heartbeatSeconds
    }

    /// One heartbeat: the progress moves by one unit of the system's 1000, and never reaches done (0.99 at most).
    static func heartbeatFraction(_ fraction: Double) -> Double {
        let f = max(0, fraction)
        guard f < 0.99 else { return f }
        return min(0.99, f + 0.0011)
    }
}

/// Notes notifications (NotesNotifier): ready, paused, failed. One per state change, never per percent.
enum NotifyRule {
    /// In front the app already shows the note: nothing is posted.
    static func shouldNotify(appActive: Bool) -> Bool { !appActive }

    /// One identifier per note, so a later state replaces the earlier one.
    static func identifier(noteID: Int64) -> String { "notes.\(noteID)" }

    /// "Paused" when work after Stop is cut off; not each time the overnight task is ended during the night (the
    /// note was already paused and finishes in a later run).
    static func postsPaused(cutOffByOvernightTask: Bool) -> Bool { !cutOffByOvernightTask }
}

/// How long the work left on a note will take on the phone's CPU (measured on the device), to decide whether
/// to offer finishing it overnight. All the constants are here.
enum WorkEstimate {
    /// Transcription: seconds of work per second of audio still to transcribe.
    static let secondsPerAudioSecond = 1.2
    /// One section summary (~5 minutes of audio).
    static let secondsPerSection = 60
    /// One section's memories: their own model call after the notes, about 1.4 times a section summary
    /// (Mac CPU: 82.6 s against 60.2 s, 69.3 s against 46.1 s).
    static let secondsPerMemorySection = 84
    /// One of the three outputs (summary, notes, questions).
    static let secondsPerOutput = 90
    /// Audio per section, for sections not planned yet (the planner's target length).
    static let sectionSeconds = 300
    /// More work than this (minutes) and the note offers to finish overnight.
    static let offerAboveMinutes = 20

    /// Whole minutes of work left, rounded up. Negative inputs count as none.
    static func minutesLeft(audioSecondsLeft: Int, sectionsLeft: Int, outputsLeft: Int, memorySectionsLeft: Int = 0) -> Int {
        let seconds = Double(max(0, audioSecondsLeft)) * secondsPerAudioSecond
            + Double(max(0, sectionsLeft) * secondsPerSection + max(0, outputsLeft) * secondsPerOutput
                     + max(0, memorySectionsLeft) * secondsPerMemorySection)
        return Int((seconds / 60).rounded(.up))
    }

    /// Sections still to write: the audio after the last section written, in sections of `sectionSeconds`.
    static func sectionsLeft(durationSeconds: Int, coveredSeconds: Int) -> Int {
        let left = max(0, durationSeconds - max(0, coveredSeconds))
        return (left + sectionSeconds - 1) / sectionSeconds
    }

    static func offersOvernight(minutesLeft: Int) -> Bool { minutesLeft > offerAboveMinutes }

    /// The offer after Stop (K43): the transcription, the sections and the three outputs. Memory sections never
    /// count toward it (they run later, idle in front or overnight); memory time is only part of the overnight
    /// work left.
    static func offersOvernight(audioSecondsLeft: Int, sectionsLeft: Int, outputsLeft: Int, memorySectionsLeft: Int = 0) -> Bool {
        offersOvernight(minutesLeft: minutesLeft(audioSecondsLeft: audioSecondsLeft, sectionsLeft: sectionsLeft, outputsLeft: outputsLeft))
    }
}

/// Notes the person chose to finish overnight, while the phone charges (UserDefaults "pipeline.overnight").
enum OvernightList {
    /// The list after a check of the notes in `checked`: those not in `keep` (deleted, or nothing left to do)
    /// are dropped; ids marked after the check started (not in `checked`) are kept; repeats are dropped; order kept.
    static func pruned(_ ids: [Int64], checked: Set<Int64>, keep: Set<Int64>) -> [Int64] {
        var seen = Set<Int64>()
        return ids.filter { (!checked.contains($0) || keep.contains($0)) && seen.insert($0).inserted }
    }

    /// After a run of a note: its mark stays only while it is still marked (not forgotten, not "Do it now")
    /// and work is left — stopped for the night, or cut off / too hot and queued again.
    static func keepsMark(marked: Bool, parked: Bool, queued: Bool) -> Bool { marked && (parked || queued) }

    /// A run belongs to the overnight task (and its expiry may cancel it) only when that task started it with
    /// the app in the background and no continued-processing task holding it; never a run in front or one the
    /// continued-processing task keeps going.
    static func ownsRun(inOvernightTask: Bool, appActive: Bool, continuedTaskHeld: Bool) -> Bool {
        inOvernightTask && !appActive && !continuedTaskHeld
    }

    /// A note marked overnight is only started while plugged in or inside the overnight task; "Do it now"
    /// clears the mark. Unmarked notes always may run.
    static func mayStart(marked: Bool, pluggedIn: Bool, inOvernightTask: Bool) -> Bool {
        mayStart(mark: marked ? .person : .none, pluggedIn: pluggedIn, inOvernightTask: inOvernightTask, taskOnPower: true)
    }

    /// Why a note is on the list: not at all, the person's "Finish overnight", or by itself after a cut-off.
    enum Mark: Equatable { case none, person, automatic }

    static func mark(onList: Bool, automatic: Bool) -> Mark { onList ? (automatic ? .automatic : .person) : .none }

    /// Who may start now. The person's mark waits for power: plugged in, or inside an overnight task that asked for
    /// the charger (one that runs on battery for automatic notes does not count). An automatic note is a cut-off
    /// note kept across launches: it may run whenever the resume loop runs (in front at once, or in any task).
    static func mayStart(mark: Mark, pluggedIn: Bool, inOvernightTask: Bool, taskOnPower: Bool) -> Bool {
        switch mark {
        case .none, .automatic: return true
        case .person: return pluggedIn || (inOvernightTask && taskOnPower)
        }
    }

    /// A notes run (a recording's notes, or resuming them) cut off with the app not in front (the continued-processing
    /// task expired, or the app left the front): the note goes on this list by itself, without the "Finish
    /// overnight?" offer, so the overnight task finishes it. Memories are not notes; in front the resume loop takes it.
    static func enrolsAfterCutOff(notesRun: Bool, memoriesOnly: Bool, appActive: Bool) -> Bool {
        notesRun && !memoriesOnly && !appActive
    }

    /// The enrolment is automatic only for a note the person did not mark: theirs keeps its meaning.
    static func marksAutomatic(alreadyMarked: Bool) -> Bool { !alreadyMarked }

    /// While an automatic note waits, the task may run on battery (iOS still picks an idle moment); otherwise only
    /// on the charger, as the person was told.
    static func requiresExternalPower(automaticPending: Bool) -> Bool { !automaticPending }
}

/// Regenerate, Undo and Try again on a note's screen.
enum RegenerateRules {
    /// "Regenerate" under a tab: the tab has text, nothing runs for this note, the AI is ready and the note is not
    /// waiting for the charger. (Disabled, not hidden, while another note runs: one model run at a time.)
    static func showsRegenerate(hasText: Bool, noteBusy: Bool, aiReady: Bool, overnight: Bool) -> Bool {
        hasText && !noteBusy && aiReady && !overnight
    }

    /// "Regenerate all" in the note's menu: there is a transcript and at least one output to write again.
    static func showsRegenerateAll(hasTranscript: Bool, outputs: [Bool], noteBusy: Bool, aiReady: Bool, overnight: Bool) -> Bool {
        hasTranscript && outputs.contains(true) && !noteBusy && aiReady && !overnight
    }

    /// Undo under a tab: the last regenerate (since the note was opened) replaced this tab's text and nothing
    /// runs for this note now.
    static func showsUndo(replacedThisTab: Bool, noteBusy: Bool) -> Bool { replacedThisTab && !noteBusy }

    /// "Try again" on the note's failed or paused banner: the note still needs work (the resume path does
    /// nothing otherwise), the AI is ready and the note is not waiting for the charger.
    static func showsTryAgain(needsWork: Bool, aiReady: Bool, overnight: Bool) -> Bool {
        needsWork && aiReady && !overnight
    }

    /// New text replaces the old only when it has something in it.
    static func accepts(_ text: String) -> Bool { !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    enum Outcome: Equatable {
        /// Everything asked for was written: the new text replaces the old.
        case replaced
        /// The person tapped Stop: old text kept, no failure shown.
        case stopped
        /// Error, heat or cut off by iOS: old text kept, "Try again" shown.
        case failed
    }

    /// Stop wins: a run cancelled by Stop is never shown as a failure, nor saved.
    static func outcome(written: Bool, stopped: Bool) -> Outcome {
        stopped ? .stopped : (written ? .replaced : .failed)
    }
}

/// The memories of one note, written after its notes: one section at a time (sve_memories_from_section), each
/// after the caller's checks (heat, a recording, notes waiting), every finished section kept before the next.
@MainActor
enum MemoryPass {
    /// What one section's memory call came to.
    enum Step: Equatable {
        /// Stored this many (0 also for a section already done, or a model reply that could not be used).
        case stored(Int)
        /// -1: the note or the section is gone (deleted meanwhile), bad input, or no model.
        case gone
        /// -2: cancelled; nothing was stored and the section stays due.
        case cancelled
        /// -3: the model call itself failed; nothing was stored and the section stays due.
        case failed

        nonisolated init(result: Int32) {
            switch result {
            case 0...: self = .stored(Int(result))
            case -2: self = .cancelled
            case -3: self = .failed
            default: self = .gone
            }
        }
    }

    enum Outcome: Equatable {
        /// Every section given has its memories.
        case finished
        /// A section call said -1: the run ends quietly, nothing more for this note now.
        case gone
        /// A section's model call failed (-3): this note's sections left stay due, other notes go on.
        case failed
        /// `beforeEach` said no, or a call was cancelled (recording, the person, heat, cut off): the sections left
        /// stay due for later, and the backfill stops.
        case stopped
    }

    /// What a thrown -2 / -3 from one section counts as: a -3 (the model call failed) is the note's failure only
    /// when nothing was stopping the run; otherwise (the app left the front on the GPU, the run was cut off, the
    /// person asked for something, a recording started, critical heat) it is a cancel, like -2.
    nonisolated static func stepAfterError(modelFailed: Bool, stopping: Bool) -> Step {
        modelFailed && !stopping ? .failed : .cancelled
    }

    /// After one note of a backfill: a real stop ends it; a finished, gone or failed note lets the next one go.
    static func backfillGoesOn(after outcome: Outcome) -> Bool { outcome != .stopped }

    /// The notes a backfill takes, in the core's order (newest first): those whose memory call did not fail
    /// during this launch, at most `limit`.
    nonisolated static func backfillNotes(_ needing: [Int64], skipping failed: Set<Int64>, limit: Int) -> [Int64] {
        Array(needing.lazy.filter { !failed.contains($0) }.prefix(max(0, limit)))
    }

    /// For each section index in `due`, in order: `beforeEach` (false stops), `write`, then `afterEach` with the
    /// sections finished so far. Returns how it ended and how many sections were finished.
    static func run(due: [Int], beforeEach: (Int) async -> Bool, write: (Int) async -> Step,
                    afterEach: (_ finished: Int, _ stored: Int) async -> Void = { _, _ in }) async -> (outcome: Outcome, finished: Int) {
        var finished = 0
        for idx in due {
            guard await beforeEach(idx) else { return (.stopped, finished) }
            switch await write(idx) {
            case .stored(let n):
                finished += 1
                await afterEach(finished, n)
            case .gone: return (.gone, finished)
            case .cancelled: return (.stopped, finished)
            case .failed: return (.failed, finished)
            }
        }
        return (.finished, finished)
    }
}

/// The person comes first: memory work never makes their controls busy, and their action stops it.
enum PersonFirst {
    /// Whether Ask, Regenerate, Try again and Generate notes show busy. `working`: any model work runs;
    /// `memoriesOnly`: that work is memories.
    static func controlsBusy(working: Bool, memoriesOnly: Bool) -> Bool { working && !memoriesOnly }

    enum Action: Equatable {
        /// Nothing runs: go.
        case start
        /// Only memories run: stop them (the section in flight stays due), then go.
        case stopMemoriesThenStart
        /// Other work runs (a note's notes): busy, as before.
        case busy
    }

    static func action(working: Bool, memoriesOnly: Bool) -> Action {
        guard working else { return .start }
        return memoriesOnly ? .stopMemoriesThenStart : .busy
    }

    /// Notes one memory backfill takes: one in front (the next idle pass goes on), `overnightLimit` in the background.
    static func backfillNotes(inFront: Bool, overnightLimit: Int = 20) -> Int { inFront ? 1 : overnightLimit }
}
