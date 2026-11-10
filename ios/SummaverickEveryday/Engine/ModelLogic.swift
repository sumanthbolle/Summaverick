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

    /// Only critical cancels a piece or stage in the middle; the heat ladder (HeatLadder) otherwise only rests
    /// between pieces and stages (cancelling at serious threw away the same piece again and again).
    static func cancelsRun(_ state: ProcessInfo.ThermalState) -> Bool { state == .critical }
}

/// Gradual heat control for long work (transcription pieces, sections, note outputs), after Edge Veda's runtime
/// policy (ideas only): a ladder of levels fed by heat, free memory, battery and Low Power Mode. Worse is at once,
/// better one level per minute of calm. The knobs are the rest before each piece, section or output and the CPU
/// threads; decoding itself is never slowed.
enum HeatLadder {
    enum Level: Int, Comparable {
        case full = 0, eased, minimal, paused
        static func < (a: Level, b: Level) -> Bool { a.rawValue < b.rawValue }
        var name: String {
            switch self {
            case .full: return "full"
            case .eased: return "eased"
            case .minimal: return "minimal"
            case .paused: return "paused"
            }
        }
    }

    /// Demand must stay below the level this long before the level comes down one step.
    static let calmSeconds: Double = 60

    /// The level the inputs ask for; the first match wins. batteryLevel is 0…1, or negative when unknown
    /// (simulator, monitoring off), which never counts.
    static func demanded(thermal: ProcessInfo.ThermalState, availableMemoryMB: Int, batteryLevel: Float,
                         charging: Bool, lowPower: Bool) -> Level {
        let battery = batteryLevel >= 0 && !charging ? batteryLevel : nil
        if thermal == .critical || availableMemoryMB < 50 { return .paused }
        if thermal == .serious || availableMemoryMB < 100 || (battery.map { $0 < 0.05 } ?? false) { return .minimal }
        if thermal == .fair || lowPower || (battery.map { $0 < 0.15 } ?? false) { return .eased }
        return .full
    }

    /// Worse is immediate; better is one level, once `demanded < current` has held for calmSeconds (calmSince,
    /// kept by the caller: see State). Times are seconds on a monotonic clock that counts device sleep
    /// (HeatGovernor.now), never wall-clock time.
    static func next(current: Level, demanded: Level, calmSince: TimeInterval?, now: TimeInterval) -> Level {
        if demanded >= current { return max(current, demanded) }
        guard let calmSince, now - calmSince >= calmSeconds else { return current }
        return Level(rawValue: current.rawValue - 1) ?? current
    }

    /// The level with its calm clock, as one holder keeps it (HeatGovernor).
    struct State: Equatable {
        private(set) var level: Level = .full
        /// Since when demand has been below the level; nil when it is not. Reset whenever demand rises, and
        /// after each step down (the next step needs its own minute).
        private(set) var calmSince: TimeInterval?
        private var lastDemanded: Level?
        /// When the last reading was fed.
        private(set) var lastReading: TimeInterval?

        /// Feeds one reading; true when the level changed. A gap of more than calmSeconds since the last reading
        /// (the app was suspended, nothing read) counts as calm: the level starts again from this demand, so a
        /// stale paused never holds up the work that wakes the app (worse is still immediate: demand is fresh).
        mutating func update(demanded d: Level, now: TimeInterval) -> Bool {
            defer { lastReading = now }
            if let last = lastReading, now - last > HeatLadder.calmSeconds {
                let changed = d != level
                level = d
                calmSince = nil
                lastDemanded = d
                return changed
            }
            if d < level {
                if calmSince == nil || (lastDemanded.map { d > $0 } ?? false) { calmSince = now }
            } else {
                calmSince = nil
            }
            lastDemanded = d
            let n = HeatLadder.next(current: level, demanded: d, calmSince: calmSince, now: now)
            guard n != level else { return false }
            level = n
            calmSince = d < n ? now : nil
            return true
        }
    }

    /// Rest before each piece, section or output. Paused has no end: the work waits until the level is below paused.
    static func restSeconds(_ l: Level) -> Double {
        switch l {
        case .full: return 0
        case .eased: return 5
        case .minimal: return 20
        case .paused: return .infinity
        }
    }

    /// A rest that began at `started` is over: its seconds at the current level have passed, the phone is back
    /// at full, or it is paused (the gate pauses the work instead).
    static func restOver(level: Level, started: TimeInterval, now: TimeInterval) -> Bool {
        level == .paused || now - started >= restSeconds(level)
    }

    /// Decode threads for CPU runs (GPU runs keep the GPU and only rest).
    static func cpuThreads(_ l: Level) -> Int { l >= .minimal ? 2 : 4 }

    /// Prefill threads for CPU runs: 0 is the core's own rule (up to 8 cores), minimal 2.
    static func batchThreads(_ l: Level) -> Int { l >= .minimal ? 2 : 0 }

    /// The "going slower to keep the phone cool" line shows while resting.
    static func showsCooling(_ l: Level) -> Bool { l == .eased || l == .minimal }
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
    /// A resumed notes run writes only the outputs not stored yet (in OutputKind order; stored: each output's text, nil
    /// when none): a summary the person was told was ready is never rewritten, nor announced again. An empty or
    /// whitespace-only output has no text (RegenerateRules.accepts) and is written.
    static func outputsToWrite(stored: [String?]) -> [Bool] { stored.map { !($0.map(RegenerateRules.accepts) ?? false) } }

    /// Reminders, tasks and profile changes are extracted only when the note was not extracted yet (the core marks it
    /// only after a successful run, so one cut off mid-extraction runs again). Extracting twice would give an accepted
    /// card a second pending copy and bring a rejected one back.
    static func extracts(alreadyExtracted: Bool) -> Bool { !alreadyExtracted }

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

    /// A note whose run was killed resumes by itself at most this many launches; then only the person starts it.
    static let launchResumeLimit = 2

    struct AfterKill: Equatable {
        var steppedAside: Set<Int64>
        var tried: Set<Int64>
        var resumes: [String: Int]
        /// The note that reached the limit at this launch.
        var capped: Int64?
    }

    /// At launch: a notes run still saved as running was killed. Its note joins the notes restored at launch and the
    /// resume is counted; past the limit it is left out and counts as tried, so a note with no transcript is not
    /// queued either (a kill never reaches markAutoTried). running nil: nothing changes.
    static func afterKill(steppedAside: Set<Int64>, tried: Set<Int64>, resumes: [String: Int], running: Int64?) -> AfterKill {
        var r = AfterKill(steppedAside: steppedAside, tried: tried, resumes: resumes, capped: nil)
        guard let running else { return r }
        let key = String(running), count = resumes[key] ?? 0
        if count >= launchResumeLimit {
            r.steppedAside.remove(running)
            r.tried.insert(running)
            r.capped = running
        } else {
            r.steppedAside.insert(running)
            r.resumes[key] = count + 1
        }
        return r
    }

    /// When a run ends, the saved running note stays only while that note is queued to go on (it was cut off or
    /// paused, and the queue is not saved): a kill before its turn still resumes it. Otherwise it is cleared.
    static func keepsRunningNote(queued: Bool) -> Bool { queued }

    /// Tried ids of notes that still exist. An empty list may be a read failure, and a list cut at its limit does
    /// not show every note: nothing is pruned then.
    static func prunedTried(_ tried: Set<Int64>, existing: Set<Int64>, listComplete: Bool) -> Set<Int64> {
        guard listComplete, !existing.isEmpty else { return tried }
        return tried.intersection(existing)
    }
}

/// Where a model run goes. iOS refuses GPU work from the background (even while recording), so
/// transcription always runs on the CPU, and everything else only uses the GPU while the app is in front.
enum ModelBackend: Equatable {
    case gpu, cpu

    /// Layers given to the GPU: all of them (Metal), or none.
    var gpuLayers: Int32 { self == .gpu ? 99 : 0 }
    /// Decode threads on the CPU: 4 (measured: 6 is slower on the phone); 0 = the core's own choice for the GPU.
    /// Decode only: prefill and the audio encoder follow sve_threads.h.
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

    /// Why live transcription should wait now, nil to go on. Heat (the ladder at paused; the levels below rest
    /// between pieces instead, and it leaves paused only after a minute of calm) comes first. batteryLevel is 0…1,
    /// or negative when unknown (simulator, monitoring off), which never pauses.
    static func reason(heat: HeatLadder.Level, batteryLevel: Float, charging: Bool) -> LivePause? {
        if heat == .paused { return .heat }
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

    /// iOS ends a continued-processing task whose progress stands still for about 30 s (seen on the owner's iPhone:
    /// expired 42 s after the last move, during a CPU regenerate), so the lock-screen progress moves at least this
    /// often while the model works.
    static let heartbeatSeconds: TimeInterval = 10

    /// The work counts as running while the model (or the pipeline) showed a sign of life this recently. Longer than
    /// a CPU model load or one 512-token prefill batch, which call back nothing.
    static let aliveSeconds: TimeInterval = 90

    /// A heartbeat now (a timer asks, not the model): the progress has not moved for `heartbeatSeconds` and the work
    /// is still alive. Work that stopped calling back gets no pulses, so iOS may end it.
    static func pulseDue(lastMove: Date, lastAlive: Date, now: Date) -> Bool {
        now.timeIntervalSince(lastMove) >= heartbeatSeconds && now.timeIntervalSince(lastAlive) < aliveSeconds
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

    /// "Taking a short break" when work after Stop is cut off; not each time the overnight task is ended during the night (the
    /// note was already paused and finishes in a later run).
    static func postsPaused(cutOffByOvernightTask: Bool) -> Bool { !cutOffByOvernightTask }

    /// The parts of a note's notes run that each say when they are ready: the transcript (stored after Stop), the
    /// summary (title and short summary stored), the notes (detailed notes and questions stored).
    static let transcript = "transcript", summary = "summary", notes = "notes"

    /// One notification per stage per note per run: `posted` holds the stages this run already posted.
    static func next(posted: Set<String>, stage: String) -> Bool { !posted.contains(stage) }

    /// "Transcript ready" only with a real title: with the placeholder, "Summary ready" follows with the real one.
    /// Counting ("1 of 3 ready", StepCount) it is posted with the placeholder too, so the count starts at 1.
    static func saysTranscriptReady(title: String, counting: Bool = false) -> Bool { counting || !title.hasPrefix("Recording ") }

    /// What a regenerate finishing in the background says: a summary-only one "Summary ready", any other "Your notes are ready".
    static func regenerateStage(summaryOnly: Bool) -> String { summaryOnly ? summary : notes }
}

/// Which version of a notification's words a note gets, and the first name the notifications greet with.
enum NotifyCopy {
    /// UserDefaults key: the profile's first word, written whenever the profile loads or saves, because the notifier
    /// must not await the core while posting. "Delete everything" clears it.
    static let firstNameKey = "profile.firstName"

    /// 0 ..< count. The same note and stage always get the same version; different notes spread across them. FNV-1a,
    /// because Swift's own hash changes on every launch.
    static func variant(noteID: Int64, stage: String, count: Int) -> Int {
        guard count > 1 else { return 0 }
        var h: UInt64 = 14695981039346656037
        for b in stage.utf8 { h = (h ^ UInt64(b)) &* 1099511628211 }
        var id = UInt64(bitPattern: noteID)
        for _ in 0..<8 {
            h = (h ^ (id & 0xFF)) &* 1099511628211
            id >>= 8
        }
        // Finished like MurmurHash3, so neighbouring ids do not fall into long runs of one version.
        h = (h ^ (h >> 33)) &* 0xFF51AFD7ED558CCD
        h = (h ^ (h >> 33)) &* 0xC4CEB9FE1A85EC53
        return Int((h ^ (h >> 33)) % UInt64(count))
    }

    /// The first word of a profile name; nil when there is none.
    static func firstName(of name: String?) -> String? {
        name?.split(whereSeparator: \.isWhitespace).first.map(String.init)
    }

    static func rememberFirstName(of name: String?, in defaults: UserDefaults = .standard) {
        if let first = firstName(of: name) { defaults.set(first, forKey: firstNameKey) }
        else { defaults.removeObject(forKey: firstNameKey) }
    }

    static func storedFirstName(in defaults: UserDefaults = .standard) -> String? {
        firstName(of: defaults.string(forKey: firstNameKey))
    }
}

/// "1 of 3 ready" (owner, 2026-11-10: "so it will give an impression something is already working"). The parts of a
/// notes run, in order: the transcript (stored after Stop), the summary (title and short summary), the notes
/// (detailed notes and revision questions: one stored part with one "ready" notification, NotifyRule.notes). The
/// total is what this run writes; with one part there is no counter. A "Write again" replaces its text only once
/// everything asked for is written, so it has one ready moment and no counter either (it never counts).
struct StepCount: Equatable {
    enum Part: String, Equatable { case transcript, summary, notes }

    let parts: [Part]
    private(set) var ready: [Part] = []

    init(parts: [Part]) { self.parts = parts }

    /// outputsToWrite: per OutputKind (short summary, detailed notes, questions), as WaitingNotes.outputsToWrite.
    static func parts(writesTranscript: Bool, outputsToWrite: [Bool]) -> [Part] {
        var p: [Part] = writesTranscript ? [.transcript] : []
        if outputsToWrite.first == true { p.append(.summary) }
        if outputsToWrite.dropFirst().contains(true) { p.append(.notes) }
        return p
    }

    /// A "Write again" of these OutputKind raw values (the summary is 0).
    static func parts(regenerating kinds: [Int]) -> [Part] {
        parts(writesTranscript: false, outputsToWrite: (0..<3).map { kinds.contains($0) })
    }

    var total: Int { parts.count }
    var counts: Bool { total > 1 }

    /// The part is stored: its number (1...total). nil when it is not one this run writes, or was already counted.
    mutating func stored(_ part: Part) -> Int? {
        guard parts.contains(part), !ready.contains(part) else { return nil }
        ready.append(part)
        return ready.count
    }

    /// The line of a part's "ready" notification ("1 of 3 ready: what was said. Writing your summary…"); nil without a counter.
    func readyLine(step: Int, part: Part) -> String? {
        guard counts else { return nil }
        let n = min(max(1, step), total), t = total
        if n == t { return String(localized: "\(n) of \(t) ready. Your notes are done.") }
        let next = parts.firstIndex(of: part).flatMap { i in i + 1 < parts.count ? parts[i + 1] : nil }
        switch (part, next) {
        case (.transcript, .summary): return String(localized: "\(n) of \(t) ready: what was said. Writing your summary…")
        case (.transcript, .notes): return String(localized: "\(n) of \(t) ready: what was said. Writing your notes…")
        case (.summary, .notes): return String(localized: "\(n) of \(t) ready: your summary. Writing your notes…")
        default: return nil
        }
    }

    /// The progress line on the note, the Live Activity and the lock screen: "1 of 3 ready · <stage>" once a part is
    /// stored and more are left; the stage alone otherwise.
    func workingLine(_ label: String) -> String {
        guard counts, !ready.isEmpty, ready.count < total else { return label }
        return String(localized: "\(ready.count) of \(total) ready · \(label)")
    }

    /// Pieces of about TranscriptRun's 28 s: written down so far and in the whole recording. Done never passes total.
    static let pieceSeconds = 28.0
    static func pieces(transcribedSeconds: Double, durationSeconds: Double) -> (done: Int, total: Int) {
        guard durationSeconds > 0, durationSeconds.isFinite else { return (0, 0) }
        let total = max(1, Int((durationSeconds / pieceSeconds).rounded(.up)))
        let t = transcribedSeconds.isFinite ? max(0, transcribedSeconds) : 0
        let done = t >= durationSeconds ? total : min(total, Int(t / pieceSeconds))
        return (done, total)
    }

    /// "Writing down what was said · 14 of 56 parts"; nil for a recording of one piece (the percent line stays).
    static func transcribingLine(transcribedSeconds: Double, durationSeconds: Double) -> String? {
        let p = pieces(transcribedSeconds: transcribedSeconds, durationSeconds: durationSeconds)
        guard p.total > 1 else { return nil }
        return String(localized: "Writing down what was said · \(p.done) of \(p.total) parts")
    }
}

/// What a run that stopped short shows, everywhere (note screen, Live Activity, notifications, the lock screen's
/// continued-processing progress): only a real failure (FailureKind .failed) says failed. A cut-off (expiry, heat,
/// the GPU stopped in the background, a kill, the first unknown error) is a pause that continues by itself.
enum PauseWords {
    enum Shows: Equatable { case paused, failed }

    static func shows(_ outcome: FailureKind.Outcome) -> Shows { outcome == .failed ? .failed : .paused }

    /// The note screen's banner for a note with a line (lastFailure): failed only while that line is the one its real
    /// failure wrote (failedLine); any other line (cut off, too warm, waiting) is a pause. nil: no line, no banner.
    static func banner(line: String?, failedLine: String?) -> Shows? {
        guard let line else { return nil }
        return line == failedLine ? .failed : .paused
    }

    /// The lock screen's line when the continued-processing task ends with work left.
    static var paused: String { String(localized: "Paused. I'll finish this by myself.") }
}

/// The one decision for a notes run (and a tap) that stopped short: a cut-off (the work is kept and continues by
/// itself, "paused" at most once) or a real failure ("failed", the person sees why). Only no speech in the recording,
/// or no transcript and no audio file, is a failure. Any interruption (cancelled: a CancellationError or the task
/// cancelled; aborted: an expiry or the overnight task ended; cutOffByBackground: the GPU stopped in the background,
/// BackendChoice.failureIsCutOff; heat) is a cut-off. Anything else is an unknown error: a cut-off the first time,
/// failed the second time in a row for the same note (unknownStreak: unknown errors in a row before this one,
/// failureStreak[noteID]; it resets on any success).
enum FailureKind {
    enum Outcome: Equatable { case cutOff, failed }

    /// Unknown errors in a row that are still retried by themselves.
    static let unknownRetries = 1

    static func classify(cancelled: Bool, aborted: Bool, cutOffByBackground: Bool, heat: Bool, noSpeech: Bool,
                         audioGone: Bool, unknownStreak: Int = 0) -> Outcome {
        if noSpeech || audioGone { return .failed }
        if cancelled || aborted || cutOffByBackground || heat { return .cutOff }
        return unknownStreak < unknownRetries ? .cutOff : .failed
    }

    /// Which kind of run a streak belongs to: one kind's unknown error never fails another kind.
    enum Run: String { case notes, regenerate, findReminders }

    /// The streak's key in UserDefaults ([String: Int], kept across launches): note id and run kind.
    static func streakKey(noteID: Int64, run: Run) -> String { "\(noteID).\(run.rawValue)" }

    /// The streaks without any of a deleted note's.
    static func forgetting(noteID: Int64, in streaks: [String: Int]) -> [String: Int] {
        streaks.filter { !$0.key.hasPrefix("\(noteID).") }
    }

    /// The error is none of the known kinds: it counts in the note's streak.
    static func isUnknown(cancelled: Bool, aborted: Bool, cutOffByBackground: Bool, heat: Bool, noSpeech: Bool,
                          audioGone: Bool) -> Bool {
        !(cancelled || aborted || cutOffByBackground || heat || noSpeech || audioGone)
    }
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
    /// "Write again" under a tab: the tab has text, nothing runs for this note, the AI is ready and the note is not
    /// waiting for the charger. (Disabled, not hidden, while another note runs: one model run at a time.)
    static func showsRegenerate(hasText: Bool, noteBusy: Bool, aiReady: Bool, overnight: Bool) -> Bool {
        hasText && !noteBusy && aiReady && !overnight
    }

    /// "Write it all again" in the note's menu: there is a transcript and at least one output to write again.
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

    /// Lock-screen steps of a regenerate: the title (regenerate all) and each output asked for.
    static func steps(all: Bool, outputs: Int) -> Int { (all ? 1 : 0) + max(0, outputs) }

    /// 0…1 for a regenerate's lock-screen progress: `part` (0…1) of step `done` of `steps`. No transcription share:
    /// a regenerate starts from the stored transcript.
    static func fraction(done: Int, part: Double, steps: Int) -> Double {
        guard steps > 0 else { return 1 }
        return min(1, max(0, (Double(max(0, done)) + min(1, max(0, part))) / Double(steps)))
    }
}

/// A person's action on an existing note (Generate notes, Regenerate, Try again, Do it now, Find reminders): the
/// continued-processing task is asked for in the tap, while the app is in front, so the run goes on after the app is
/// closed, as after Stop.
enum PersonRun {
    /// Ask in the tap: the app is in front (a request from the background is refused), the AI is ready, and the run
    /// may start: nothing runs, or only memories, which yield. Another note's work running: the tap is queued
    /// (TapQueue) and that run keeps the task it has.
    static func asksForContinuedTask(appActive: Bool, aiReady: Bool, working: Bool, memoriesOnly: Bool) -> Bool {
        appActive && aiReady && PersonFirst.action(working: working, memoriesOnly: memoriesOnly) != .busy
    }
}

/// One continued-processing task per run of work: it ends as soon as the work is done, a finished task is never
/// reused for the next Stop or tap, and its progress never goes back.
enum TaskLifecycle {
    enum BeginAction: Equatable {
        /// The held task has work left: the new work joins it.
        case keep
        /// Complete the held task (if any) first, then submit a new request.
        case finishThenSubmit
    }

    /// A Stop or tap asks for the task. A held task whose work is done is completed first and a new request is
    /// submitted (seen on the owner's iPhone: a finished task reused for the next Stop went 100% to 0% and iOS
    /// ended it a minute later). Nothing held: a new request.
    static func onBegin(held: Bool, workLeft: Bool) -> BeginAction {
        held && workLeft ? .keep : .finishThenSubmit
    }

    /// The task ends when nothing runs and nothing waits: no person's run that asked for it and has not ended yet
    /// (it may be waiting for memories to yield, so nothing runs for a moment), and no tap queued.
    static func finishes(working: Bool, personRunsPending: Int, queued: Int) -> Bool {
        !working && personRunsPending <= 0 && queued <= 0
    }

    /// How the continued-processing task ends: done, or paused with work left (a cut-off, a note waiting its turn).
    enum Completion: Equatable { case done, paused }
    static func completion(workLeft: Bool) -> Completion { workLeft ? .paused : .done }

    /// Told to iOS as success either way: a task completed with success false shows "Failed" on the lock screen (the
    /// owner saw "failed at 50%" for note 22, which was only cut off and finished by itself). Paused first changes
    /// the task's lines to the paused wording (PauseWords.paused).
    static func reportsSuccess(_ c: Completion) -> Bool { true }

    /// The task expired (or the background time ran out without one): the run in flight is cut off (resumed later,
    /// "paused" posted) only while it still has notes to write. Memories, and the insight after a note's notes are
    /// stored, are just stopped: there is nothing to resume.
    static func expiryCutsOff(working: Bool, memoriesOnly: Bool, notesStored: Bool) -> Bool {
        working && !memoriesOnly && !notesStored
    }

}

/// A person's tap (Regenerate, Generate notes or Try again, Find reminders) made while another note's work runs: it
/// waits its turn and runs by itself, never dropped. Kept across launches (TapQueue.key).
struct QueuedTap: Codable, Equatable {
    enum Kind: Codable, Equatable {
        /// output: an OutputKind raw value, or nil for the title and all three outputs.
        case regenerate(output: Int?)
        /// Generate notes, or Try again on the note's banner.
        case generate
        case findReminders
    }
    let noteID: Int64
    let kind: Kind
    /// How many times in a row this tap's run was cut off (expiry, heat, the GPU stopped) and queued again. Not part
    /// of its identity: the same note and kind are one tap whatever the streak.
    var cutOffs: Int = 0

    init(noteID: Int64, kind: Kind, cutOffs: Int = 0) {
        self.noteID = noteID
        self.kind = kind
        self.cutOffs = cutOffs
    }

    private enum CodingKeys: String, CodingKey { case noteID, kind, cutOffs }
    /// A queue saved before the streak existed decodes with a streak of 0.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        noteID = try c.decode(Int64.self, forKey: .noteID)
        kind = try c.decode(Kind.self, forKey: .kind)
        cutOffs = try c.decodeIfPresent(Int.self, forKey: .cutOffs) ?? 0
    }

    static func == (a: QueuedTap, b: QueuedTap) -> Bool { a.noteID == b.noteID && a.kind == b.kind }
}

enum TapQueue {
    /// UserDefaults key of the queue (the regenerate running is kept there too, so a kill never loses it).
    static let key = "pipeline.queuedTaps"

    /// Appended (oldest first), with no duplicate for the same note and kind. A regenerate-all replaces the
    /// note's single-output regenerates; a single-output regenerate with a regenerate-all of its note queued is
    /// covered by it.
    static func enqueue(_ q: [QueuedTap], _ t: QueuedTap) -> [QueuedTap] {
        if q.contains(t) { return q }
        guard case .regenerate(let output) = t.kind else { return q + [t] }
        let all = QueuedTap(noteID: t.noteID, kind: .regenerate(output: nil))
        if output != nil { return q.contains(all) ? q : q + [t] }
        return q.filter { !($0.noteID == t.noteID && isRegenerate($0.kind)) } + [t]
    }

    /// A note deleted: its taps leave the queue.
    static func forgetting(noteID: Int64, in q: [QueuedTap]) -> [QueuedTap] { q.filter { $0.noteID != noteID } }

    /// The tap to run next: the oldest.
    static func next(_ q: [QueuedTap]) -> QueuedTap? { q.first }

    /// Whether a tap is queued at all. notesRunningFor: the note whose own notes are being written now (nil: none, or
    /// the work running is not a note's notes). That run writes its notes and extracts its reminders, so Generate
    /// notes, Try again and Find reminders on it are covered; a regenerate of it waits for it.
    static func accepts(_ t: QueuedTap, notesRunningFor running: Int64?) -> Bool {
        t.noteID != running || isRegenerate(t.kind)
    }

    /// A regenerate whose run ended without new text: queued again rather than failed whenever FailureKind calls it
    /// a cut-off: the run was cancelled (abort: the continued-processing task expired, the background time ended, the
    /// overnight task expired), the heat gate paused it at critical, the GPU was stopped in the background with
    /// nothing to retry it on the CPU, or an unknown error the first time in a row (unknownStreak). Stopped for the
    /// night (the note was parked) is queued again too: it runs when the overnight conditions hold (waitsForNight).
    /// The person's Stop (which also sets abort) is neither: nothing changes and nothing runs again. Anything else is
    /// a real failure ("Try again").
    static func requeuesRegenerate(stoppedByPerson: Bool, aborted: Bool, heatPaused: Bool, parkedForNight: Bool,
                                   gpuCutOff: Bool, unknownStreak: Int) -> Bool {
        !stoppedByPerson && (parkedForNight
            || FailureKind.classify(cancelled: false, aborted: aborted, cutOffByBackground: gpuCutOff, heat: heatPaused,
                                    noSpeech: false, audioGone: false, unknownStreak: unknownStreak) == .cutOff)
    }

    /// A queued tap on a note the person chose to finish overnight (a regenerate stopped for the night) waits for the
    /// same conditions as the note: plugged in, or inside an overnight task that asked for the charger.
    static func waitsForNight(markedByPerson: Bool, pluggedIn: Bool, inOvernightTask: Bool, taskOnPower: Bool) -> Bool {
        !OvernightList.mayStart(mark: markedByPerson ? .person : .none, pluggedIn: pluggedIn,
                                inOvernightTask: inOvernightTask, taskOnPower: taskOnPower)
    }

    /// Cut-offs in a row after which a queued tap waits for the app in front or the charger.
    static let cutOffsOnBattery = 3

    /// The tap queued again after a cut-off: one more in its streak (a success ends the tap, and the streak).
    static func requeued(_ t: QueuedTap) -> QueuedTap {
        QueuedTap(noteID: t.noteID, kind: t.kind, cutOffs: t.cutOffs + 1)
    }

    /// A tap cut off 3 times in a row runs only with the app in front or on the charger, never on battery in the
    /// background (the overnight task or a continued-processing task), so it cannot drain the battery over and over.
    static func mayRun(_ t: QueuedTap, appActive: Bool, pluggedIn: Bool) -> Bool {
        t.cutOffs < cutOffsOnBattery || appActive || pluggedIn
    }

    private static func isRegenerate(_ k: QueuedTap.Kind) -> Bool {
        if case .regenerate = k { return true }
        return false
    }
}

/// The progress of one continued-processing task. Each note run in the task has its own 1000 units: the first
/// note's are the task's first 1000, and each note that joins later adds 1000 to the total. The completed units
/// never go back. Inside a note, its notes fill units 0...900 and its memories and insight 900...990; a heartbeat
/// moves the note's units up to 990, so it can always move while that note's work is alive, however many notes
/// the task runs. Done (the total) is shown only when the task finishes.
struct TaskProgress: Equatable {
    static let noteUnits: Int64 = 1000
    /// Where the note's notes end and its memories and insight start (of its own 0...1).
    static let notesShare = 0.9

    private(set) var totalUnits: Int64 = noteUnits
    private(set) var completedUnits: Int64 = 0
    /// The note's own progress, of its 1000 units (heartbeats included), 0...0.99.
    private(set) var noteFraction: Double = 0
    /// The note's notes 0...1, as the pipeline reports them (for the note's percent in notices and the Live Activity).
    private(set) var notesFraction: Double = 0
    /// The note in flight has all its notes written.
    private(set) var notesDone = false
    private var noteBase: Int64 = 0
    private var noteStarted = false

    /// A note run starts in the task: the first one uses the task's first 1000 units, a later one adds its own 1000.
    mutating func startNote() {
        if noteStarted {
            totalUnits += Self.noteUnits
            noteBase = totalUnits - Self.noteUnits
        }
        noteStarted = true
        noteFraction = 0
        notesFraction = 0
        notesDone = false
        completedUnits = max(completedUnits, noteBase)
    }

    /// The note's notes 0...1: units 0...900 of the note.
    mutating func notes(_ fraction: Double) {
        let f = min(1, max(0, fraction))
        notesFraction = max(notesFraction, f)
        if f >= 1 { notesDone = true }
        move(to: f * Self.notesShare)
    }

    /// After the notes: memories and the insight, 0...1 of what is left, units 900...990 of the note.
    mutating func afterNotes(_ part: Double) {
        notesFraction = 1
        notesDone = true
        move(to: Self.notesShare + (0.99 - Self.notesShare) * min(1, max(0, part)))
    }

    /// One heartbeat on the note's own units (NotesProgress.heartbeatFraction: never past 990). False when it cannot move.
    mutating func heartbeat() -> Bool {
        let next = NotesProgress.heartbeatFraction(noteFraction)
        guard next > noteFraction else { return false }
        move(to: next)
        return true
    }

    private mutating func move(to f: Double) {
        noteFraction = max(noteFraction, min(0.99, max(0, f)))
        completedUnits = max(completedUnits, noteBase + Int64(noteFraction * Double(Self.noteUnits)))
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

/// While a recording runs, its live transcription comes before every other model run (seen on the owner's iPhone: an
/// 11-minute recording started while another note's notes were written got a piece in only between that run's model
/// calls, and had 70% of its transcription left at Stop). The model is one actor, so a run in flight finishes its
/// piece, section or output; at its next boundary any other run steps aside: its note waits, quietly, keeping
/// everything done, and carries on after Stop.
enum LiveFirst {
    /// recording: live transcription can run now (LiveTranscriber.liveRunning), not merely a recording going.
    /// runIsLive: the recording's own live transcription (its pieces and the sections it writes while recording). No
    /// caller passes true today: live transcription never asks (it is the run the others step aside for); the
    /// parameter keeps the rule's two sides explicit.
    static func yields(recording: Bool, runIsLive: Bool) -> Bool { recording && !runIsLive }

    /// A regenerate that stepped aside goes back in the queue as it was: a step-aside is no cut-off, so its streak of
    /// cut-offs in a row (TapQueue.requeued) does not grow.
    static func requeued(_ t: QueuedTap) -> QueuedTap { t }

    /// A note that stepped aside waits quietly while the recording runs: an expiry or the resume loop ending does not
    /// cut it off (no paused notice, no failure, no overnight mark, no cut-off Live Activity). After Stop it waits as
    /// any queued note.
    static func waitsQuietly(steppedAside: Bool, recording: Bool) -> Bool { steppedAside && recording }

    /// When a run steps aside, a note only waiting its turn behind it (queued, not cut off nor paused for heat) waits
    /// for the recording with it, quietly.
    static func joinsWait(waitingTurnOnly: Bool, recording: Bool) -> Bool { waitingTurnOnly && recording }

    /// The continued-processing task finishes with success when nothing is left, or when everything left only waits
    /// for the recording (it goes on after Stop, under that Stop's own task). otherNotesLeft: notes left that do not
    /// wait quietly (cut off, paused for heat). While recording no tap and no insight can run: they wait for it.
    static func finishSucceeds(otherNotesLeft: Int, tapsLeft: Int, insightsLeft: Int, recording: Bool) -> Bool {
        otherNotesLeft == 0 && (recording || (tapsLeft == 0 && insightsLeft == 0))
    }

    /// A note's insight stays due (it runs after Stop, from the resume loop) when it stepped aside for the recording,
    /// skipped or stopped, and was not written. One that failed or had nothing to say is not tried again (as before).
    /// stoppedWhileDue: an insight already due (runDueInsight) stopped by an abort (expiry): it stays due.
    static func insightStaysDue(yielded: Bool, written: Bool, stoppedWhileDue: Bool = false) -> Bool {
        (yielded || stoppedWhileDue) && !written
    }
}

/// A WAV the recorder never finished (the app was closed mid-recording) still has the sizes written at its start: 0,
/// 0xFFFFFFFF or an older value. The core reads past them, but AVAudioPlayer and AVAudioFile (playback, compression)
/// trust them. This gives the two size fields to write so the header matches the file on disk.
enum WAVHeaderRepair {
    struct Patch: Equatable {
        /// Byte offset of the little-endian UInt32 to write.
        let offset: UInt64
        let value: UInt32
    }
    struct Result: Equatable {
        /// Where the samples start (after the "data" chunk header; AVAudioRecorder writes a padding chunk first).
        let dataOffset: Int64
        /// Bytes of audio on disk, whole sample frames only.
        let dataBytes: Int64
        /// Only the fields that differ; empty for a finished header.
        let patches: [Patch]
    }

    /// head: the first bytes of the file (enough to reach the "data" chunk header; 8 KB is plenty). nil: not a WAV,
    /// cut inside its header, or no data chunk; nothing should be written.
    static func repair(head: [UInt8], fileSize: Int64) -> Result? {
        func le32(_ i: Int) -> UInt32 {
            UInt32(head[i]) | UInt32(head[i + 1]) << 8 | UInt32(head[i + 2]) << 16 | UInt32(head[i + 3]) << 24
        }
        guard head.count >= 12, head[0..<4].elementsEqual("RIFF".utf8), head[8..<12].elementsEqual("WAVE".utf8) else { return nil }
        var align = 2
        var i = 12
        while i + 8 <= head.count {
            let len = Int(le32(i + 4))
            if head[i..<i + 4].elementsEqual("fmt ".utf8), i + 8 + 14 <= head.count {
                align = max(1, Int(head[i + 8 + 12]) | Int(head[i + 8 + 13]) << 8)
            } else if head[i..<i + 4].elementsEqual("data".utf8) {
                let offset = Int64(i + 8)
                guard fileSize >= offset else { return nil }
                let bytes = min((fileSize - offset) / Int64(align) * Int64(align), Int64(UInt32.max))
                let riff = UInt32(min(fileSize - 8, Int64(UInt32.max)))
                var patches: [Patch] = []
                if le32(4) != riff { patches.append(Patch(offset: 4, value: riff)) }
                if le32(i + 4) != UInt32(bytes) { patches.append(Patch(offset: UInt64(i + 4), value: UInt32(bytes))) }
                return Result(dataOffset: offset, dataBytes: bytes, patches: patches)
            }
            i += 8 + len + (len & 1)
        }
        return nil
    }
}

/// "Stop when quiet" (ruling B11): a recording with no voice for the chosen time stops by itself and keeps what was
/// said. Fed the recorder's level (AVAudioRecorder averagePower, dBFS) about five times a second, with the recorded
/// time, by RecordingController; it decides when to warn, when the warning is off again and when to stop.
///
/// Voice is a level at least `margin` dB above an adaptive noise floor (and never quieter than `minVoiceLevel`) for at
/// least `voicedNeeded` of the last `window` seconds. The floor follows quieter levels quickly and louder ones only
/// slowly, so steady noise (a fan, traffic hum) becomes the floor and counts as quiet, while speech, which keeps
/// rising well above it between its own pauses, does not.
enum QuietStop {
    /// The settings' choices, in minutes; 0 is Off.
    static let choices = [0, 2, 5, 10]
    static let defaultMinutes = 5
    /// The one warning comes this long before the stop.
    static let warnBefore: TimeInterval = 30
    /// Audio kept after the last voice when it stops.
    static let keepAfterVoice: TimeInterval = 2

    /// dB above the floor that can be voice.
    static let margin: Double = 8
    /// dBFS: quieter than this is never voice (a near-silent room's floor sits around -65…-75).
    static let minVoiceLevel: Double = -60
    /// Voice needs this much loud time within the last `window` seconds: a click, a cough or a door is not voice.
    static let window: TimeInterval = 1.0
    static let voicedNeeded: TimeInterval = 0.3
    /// The floor rises at most this fast (dB per second) and falls toward a quieter level with this time constant.
    static let floorRise: Double = 1.0
    static let floorFallTime: TimeInterval = 0.5
    /// Levels are clamped to this range (the recorder says -160 for digital silence).
    static let levelRange: ClosedRange<Double> = -90...0
    /// A gap between two readings counts as at most this much (a late timer does not pump the floor).
    static let maxStep: TimeInterval = 1.0

    enum Event: Equatable {
        case none
        /// `warnBefore` seconds are left: warn once.
        case warn
        /// Voice came back after the warning: take it away.
        case warningCancelled
        /// Stop now and keep the audio up to this recorded time (seconds). nil: keep it all, because no voice was
        /// ever heard and nothing was confirmed (ruling B12: a faint speaker the detector missed loses nothing).
        case stop(keepUntil: TimeInterval?)
    }

    struct Detector {
        /// Seconds of quiet that stop the recording; 0: Off, never stops.
        let limit: TimeInterval
        /// The noise floor (dBFS), known after the first reading.
        private(set) var floor: Double?
        /// The quiet timer counts from here: the last voice, the last "Keep recording", or the start.
        private(set) var quietSince: TimeInterval = 0
        /// The quiet timer started at real voice or at "Keep recording", not at the start: only then is the audio
        /// after it cut (ruling B12).
        private(set) var mayTrim = false
        private(set) var warned = false
        private(set) var stopped = false
        private var lastTime: TimeInterval?
        /// Recent loud readings: (time, how long each one stands for).
        private var loud: [(t: TimeInterval, dt: TimeInterval)] = []

        init(minutes: Int) { limit = TimeInterval(max(0, minutes)) * 60 }

        /// time: seconds recorded so far; level: dBFS.
        mutating func feed(time t: TimeInterval, level db: Double) -> Event {
            let level = min(max(db, QuietStop.levelRange.lowerBound), QuietStop.levelRange.upperBound)
            let dt = lastTime.map { max(0, min(t - $0, QuietStop.maxStep)) } ?? 0
            lastTime = t
            let voiced = hearVoice(level, at: t, dt: dt)
            followFloor(level, dt: dt)
            var event = Event.none
            if voiced {
                quietSince = t
                mayTrim = true
                if warned { warned = false; event = .warningCancelled }
            }
            guard limit > 0, !stopped else { return event }
            let quiet = t - quietSince
            if quiet >= limit {
                stopped = true
                return .stop(keepUntil: mayTrim ? quietSince + QuietStop.keepAfterVoice : nil)
            }
            if !warned, quiet >= max(0, limit - QuietStop.warnBefore) {
                warned = true
                return .warn
            }
            return event
        }

        /// "Keep recording": the warning goes and the quiet timer starts again from now.
        mutating func keepRecording(at t: TimeInterval) {
            quietSince = t
            mayTrim = true
            warned = false
            stopped = false
        }

        private mutating func hearVoice(_ level: Double, at t: TimeInterval, dt: TimeInterval) -> Bool {
            if let floor, level >= QuietStop.minVoiceLevel, level >= floor + QuietStop.margin, dt > 0 {
                loud.append((t, dt))
            }
            loud.removeAll { $0.t <= t - QuietStop.window }
            return loud.reduce(0) { $0 + $1.dt } >= QuietStop.voicedNeeded - 1e-9
        }

        private mutating func followFloor(_ level: Double, dt: TimeInterval) {
            guard let f = floor else { floor = level; return }
            if level < f {
                floor = f + (level - f) * min(1, dt / QuietStop.floorFallTime)
            } else {
                floor = min(level, f + QuietStop.floorRise * dt)
            }
        }
    }

    /// Cutting a finished recording (16 kHz mono 16-bit WAV, 32 000 bytes a second) after `keepSeconds` of audio:
    /// the new file size and the header fields to write (WAVHeaderRepair). nil: not a WAV the repair understands
    /// (left as it is). The size is the current one when there is no more audio than that.
    static func trim(head: [UInt8], fileSize: Int64, keepSeconds: TimeInterval,
                     bytesPerSecond: Int64 = 32_000) -> (fileSize: Int64, dataBytes: Int64, patches: [WAVHeaderRepair.Patch])? {
        guard let r = WAVHeaderRepair.repair(head: head, fileSize: fileSize) else { return nil }
        let keep = Int64((max(0, keepSeconds) * Double(bytesPerSecond)).rounded()) / 2 * 2   // whole sample frames
        let size = r.dataOffset + min(r.dataBytes, keep)
        guard let t = WAVHeaderRepair.repair(head: head, fileSize: size) else { return nil }
        return (size, t.dataBytes, t.patches)
    }
}

/// Progress inside one output while the model writes it. The core calls back after each prefill batch and every 16
/// tokens, but reports only whole chunks done (a regenerate of one output is one chunk), so without this the
/// lock-screen progress sat at the start for minutes and then jumped to done (owner's iPhone, 2026-11-09).
enum OutputProgress {
    /// Tokens per core callback while generating.
    static let tokensPerCallback = 16

    /// About how long each output is (short summary, detailed notes, revision questions), in tokens.
    static func expectedTokens(kind: Int) -> Int {
        switch kind {
        case 0: return 250
        case 1: return 1000
        default: return 400
        }
    }

    /// 0…1 for the output: whole chunks done, plus the chunk in flight estimated from the callbacks seen in it
    /// (capped at 0.95, so it reaches 1 only when the chunk is really done).
    static func part(done: Int, total: Int, callbacks: Int, expectedTokens: Int) -> Double {
        let chunks = max(1, total)
        let finished = min(Double(chunks), Double(max(0, done)))
        guard finished < Double(chunks) else { return 1 }
        let inFlight = min(0.95, Double(max(0, callbacks) * tokensPerCallback) / Double(max(1, expectedTokens)))
        return min(1, (finished + inFlight) / Double(chunks))
    }
}

// MARK: - Ask as a chat (N9)

/// A message of a chat as sve_assistant_chat takes it: role "user" or "assistant", newest last. An assistant
/// message carries its sources (kept as text for the model) and whether it was a medicine answer (the core then
/// never hands its text to the model).
struct ChatMessage: Codable, Equatable {
    struct SourceRef: Codable, Equatable {
        let title: String
        let start_sec: Int
    }
    let role: String
    let text: String
    var medicine: Bool? = nil
    var sources: [SourceRef]? = nil
    /// On the question only: the quiz ids already asked in this chat.
    var quiz_asked: [String]? = nil
    /// "health" on a refusal of a question that was itself about health: the next question stays out of the model.
    var kind: String? = nil
}

/// Which earlier turns of a chat go with a new question. The core trims them again to its token budget
/// (1500 tokens, newest first); this keeps what is sent small and drops turns with nothing to say.
enum ChatWindow {
    /// One earlier turn: its question and what was shown for it.
    struct Turn: Equatable {
        let question: String
        /// The answer shown; for a stopped answer the part written before Stop; nil when none (failed, waiting).
        let answer: String?
        var medicine = false
        var sources: [ChatMessage.SourceRef] = []
        var kind: String? = nil
    }

    static let maxTurns = 12
    static let maxChars = 12_000

    /// The messages for sve_assistant_chat: earlier turns with an answer, newest first while within maxTurns and
    /// maxChars (whole turns), oldest first in the result, then the question. A turn with no answer (it failed,
    /// or was stopped before any word) is left out whole, so a question is never sent without what followed it.
    /// quizAsked goes on the question: the quiz ids already asked in this chat.
    static func messages(earlier: [Turn], question: String, quizAsked: [String] = []) -> [ChatMessage] {
        var picked: [[ChatMessage]] = []
        var chars = question.count
        for t in earlier.reversed() {
            guard picked.count < maxTurns else { break }
            let q = t.question.trimmingCharacters(in: .whitespacesAndNewlines)
            let a = (t.answer ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            guard !q.isEmpty, !a.isEmpty else { continue }
            guard chars + q.count + a.count <= maxChars else { break }
            chars += q.count + a.count
            picked.append([ChatMessage(role: "user", text: q),
                           ChatMessage(role: "assistant", text: a, medicine: t.medicine ? true : nil,
                                       sources: t.sources.isEmpty ? nil : t.sources, kind: t.kind)])
        }
        return picked.reversed().flatMap { $0 }
            + [ChatMessage(role: "user", text: question, quiz_asked: quizAsked.isEmpty ? nil : quizAsked)]
    }
}

/// How often the streamed answer redraws: the core's pieces come from its thread; the bubble is updated at most
/// once per interval, and always with the last text at the end.
enum StreamThrottle {
    static let interval: TimeInterval = 0.08

    /// Whether a piece arriving at `now` redraws, the last redraw having been at `last` (nil: none yet).
    static func due(last: TimeInterval?, now: TimeInterval) -> Bool {
        guard let last else { return true }
        return now - last >= interval
    }

    /// nil: redraw now. Otherwise how long until last + interval, when a skipped piece is drawn: every released
    /// sentence paints promptly even when no other piece follows it.
    static func delay(last: TimeInterval?, now: TimeInterval) -> TimeInterval? {
        guard let last, !due(last: last, now: now) else { return nil }
        return last + interval - now
    }
}

/// Stored section titles and points as shown: sections stored by an earlier build may hold "[unclear]" tags.
enum SectionText {
    static func clean(_ text: String) -> String {
        guard text.contains("[unclear]") else { return text }
        return text.replacingOccurrences(of: "[unclear] ", with: "").replacingOccurrences(of: "[unclear]", with: "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

/// The chats kept on the phone: the newest `limit` by last change, newest first.
enum ChatKeep {
    static let limit = 50

    static func kept<T>(_ chats: [T], updated: (T) -> Date) -> [T] {
        Array(chats.sorted { updated($0) > updated($1) }.prefix(limit))
    }

    /// Turns kept in one chat: the newest.
    static let turnLimit = 200
    static func capped<T>(_ turns: [T]) -> [T] { Array(turns.suffix(turnLimit)) }

    /// The file is written at most this often while an answer streams (a finished turn saves at once).
    static let streamSaveInterval: TimeInterval = 10
    static func streamSaveDue(last: TimeInterval?, now: TimeInterval) -> Bool {
        guard let last else { return true }
        return now - last >= streamSaveInterval
    }

    /// A note deleted: every turn citing it goes, whole, also a summary that cites other notes too, and every turn
    /// that may hold its words with no sources to tell (holdsUnknownWords).
    static func withoutNote<T>(_ turns: [T], noteID: Int64, sources: (T) -> [Int64], mayHoldNoteWords: (T) -> Bool) -> [T] {
        turns.filter { !sources($0).contains(noteID) && !mayHoldNoteWords($0) }
    }

    /// A turn written without sources (the core sends them only with the finished answer): what it drew on is not
    /// known, so it counts as possibly the deleted note's, unless it is general knowledge. No kind yet counts as possibly.
    static func holdsUnknownWords(writtenWithoutSources: Bool, kind: String?) -> Bool {
        writtenWithoutSources && kind != "general"
    }
}

/// Ask: which questions the core answers in code, with no model (on the store context, during any other work).
enum AskRoute {
    static func needsNoModel(_ kind: String) -> Bool { kind == "due" || kind == "quiz" || kind == "medicine" }
}

/// The first-time setup (Task L2): whether it shows, what comes after each step, which languages can be picked.
/// Setup screens (welcome → name and language → where notes are written → download → permissions), then the tour
/// over the real tabs, then "try it". Shown once, on a new install only.
enum SetupFlow {
    /// UserDefaults key holding `Progress.rawValue` once it is decided.
    static let progressKey = "setup.progress"

    enum Progress: String { case setup, tour, tryIt, done }
    enum Step: String, CaseIterable { case welcome, nameLanguage, whereNotes, download, permissions }
    /// Where notes are written. Only this phone for now: connectors to the person's own account are not built.
    enum NotesPlace: String { case phone, ownAccount }

    /// A new install has no notes, never acknowledged the recording reminder and has no profile name. Anything else is
    /// an existing install, which never sees setup.
    static func isNewInstall(hasNotes: Bool, consentAcknowledged: Bool, profileName: String?) -> Bool {
        let named = !(profileName ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        return !hasNotes && !consentAcknowledged && !named
    }

    /// At launch: the stored progress when there is one (setup, the tour or "try it" resume where they were left);
    /// otherwise setup for a new install and done for an existing one.
    static func start(stored: String?, hasNotes: Bool, consentAcknowledged: Bool, profileName: String?) -> Progress {
        if let stored, let p = Progress(rawValue: stored) { return p }
        return isNewInstall(hasNotes: hasNotes, consentAcknowledged: consentAcknowledged, profileName: profileName) ? .setup : .done
    }

    static func shows(_ p: Progress) -> Bool { p != .done }

    /// The step after `step`. Where notes are written: only this phone goes on, to the download unless it is not
    /// needed (already there, or a phone that cannot write notes). nil after permissions: the setup screens end.
    static func next(after step: Step, place: NotesPlace = .phone, downloadNeeded: Bool = true) -> Step? {
        switch step {
        case .welcome: return .nameLanguage
        case .nameLanguage: return .whereNotes
        case .whereNotes:
            guard placeSelectable(place) else { return .whereNotes }
            return downloadNeeded ? .download : .permissions
        case .download: return .permissions
        case .permissions: return nil
        }
    }

    static let afterSetup = Progress.tour
    /// "Show me around again" in Settings replays only the tips.
    static func afterTour(replay: Bool) -> Progress { replay ? .done : .tryIt }
    static let afterTryIt = Progress.done

    static func placeSelectable(_ place: NotesPlace) -> Bool { place == .phone }

    /// The progress dots: seven in all (the five setup screens, the tour, try it); how many are lit on a step.
    static let dotCount = 7
    static func dots(_ step: Step) -> Int { (Step.allCases.firstIndex(of: step) ?? 0) + 1 }

    /// The language list, in its own script. Only English and Telugu can be picked: the others' accuracy and
    /// medicine checks are not done.
    static let languages: [(code: String, native: String)] = [
        ("en", "English"), ("hi", "हिन्दी"), ("te", "తెలుగు"), ("ta", "தமிழ்"), ("ml", "മലയാളം"),
    ]
    static let selectableLanguages: Set<String> = ["en", "te"]
    static func languageSelectable(_ code: String) -> Bool { selectableLanguages.contains(code) }

    /// The language first shown: the phone's first preferred language that can be picked, else the current
    /// setting when it can be picked, else English.
    static func initialLanguage(current: String, preferred: [String]) -> String {
        for p in preferred {
            let code = String(p.split(separator: "-").first ?? "").lowercased()
            if code != "en", languageSelectable(code) { return code }
        }
        return languageSelectable(current) ? current : "en"
    }

    /// A tap on a language: one that cannot be picked yet leaves the choice as it was.
    static func choose(_ code: String, current: String) -> String { languageSelectable(code) ? code : current }

    /// UserDefaults key: the setup screen the person was on, so leaving the app mid-setup comes back there.
    static let stepKey = "setup.step"
    static func resumeStep(stored: String?) -> Step { stored.flatMap(Step.init(rawValue:)) ?? .welcome }

    /// UserDefaults key: "Not now" on Today's "Add your day and your medicines" card.
    static let dayCardDismissedKey = "today.dayCardDismissed"

    /// Today's profile card. With no profile at all it always shows ("Set up your profile"). Setup saves a profile with
    /// only a name and language, so the card stays ("Add your day and your medicines") until a routine or a medicine
    /// is added, or the person says Not now.
    static func showsDayCard(hasProfile: Bool, hasRoutine: Bool, medicineCount: Int, dismissed: Bool) -> Bool {
        !hasProfile || (!hasRoutine && medicineCount == 0 && !dismissed)
    }

    /// The older profile sheet opens by itself only when there is no profile at all.
    static func offersProfileSheet(hasProfile: Bool) -> Bool { !hasProfile }

    /// "Delete everything" starts again from the welcome, asking for agreement again (consent is cleared with it).
    static let afterWipe = Progress.setup
    /// Kept outside the core, cleared by "Delete everything" too.
    static let keysClearedByWipe = [stepKey, dayCardDismissedKey, "profile.firstName"]

    /// Setup builds a new profile only when the core said there is none; never after a failed read, which would
    /// overwrite a saved routine.
    static func mayCreateProfile(_ read: ProfileRead.Kind) -> Bool { read == .none }
}

/// What reading the profile from the core gave: the core answers "null" when nothing is saved, and no answer at all
/// (or something that does not decode) when the read failed.
enum ProfileRead {
    enum Kind: Equatable { case none, found, failed }
    static func kind(raw: String?, decodes: Bool) -> Kind {
        guard let raw else { return .failed }
        if raw.trimmingCharacters(in: .whitespacesAndNewlines) == "null" { return .none }
        return decodes ? .found : .failed
    }
}
