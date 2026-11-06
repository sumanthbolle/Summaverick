// Recording → transcript → title → three outputs. Everything after recording is local.
// If iOS suspends the app mid-way, or the phone gets too hot, the note keeps whatever was finished and
// is resumed automatically later; the user can also tap "Generate notes". Recordings made before the AI
// has downloaded wait and are processed, one at a time, once it is ready. Same stages the device
// firmware will run.
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
        case generating(OutputKind, String, Int, Int)   // kind, phase, done, total
        case done
        case failed(String)
        /// Stopped for now (the phone is too hot); resumes by itself.
        case paused(String)

        var label: String {
            switch self {
            case .idle: return ""
            case .transcribing(let p): return String(localized: "Transcribing… \(Int(p * 100))%")
            case .loadingModel: return String(localized: "Loading AI model…")
            case .extracting: return String(localized: "Finding reminders and tasks…")
            case .generating(let k, let phase, let d, let t):
                return phase == "chunk" && t > 1 ? String(localized: "Reading part \(d + 1) of \(t) for \(k.title.lowercased())…") : String(localized: "Writing \(k.title.lowercased())…")
            case .done: return String(localized: "Done")
            case .failed(let e): return String(localized: "Failed: \(e)")
            case .paused(let m): return m
            }
        }
        /// True while the model is working (not idle, done or failed).
        var isWorking: Bool {
            switch self { case .idle, .done, .failed, .paused: return false; default: return true }
        }
    }

    private(set) var stage: Stage = .idle {
        didSet {
            guard case .failed(let msg) = stage, let id = activeNoteID else { return }
            if wentToBackground {
                // iOS stops the AI model (it runs on the GPU) once the app is in the background, so a
                // failure after the phone locked is an interruption, not a real error. Resume it later.
                interrupted.insert(id)
                lastFailure[id] = String(localized: "Paused when the phone locked. It will continue when you open Summaverick.")
            } else {
                lastFailure[id] = msg
            }
        }
    }
    /// Notes whose processing was cut off (app in the background, phone too hot) or that waited for
    /// the AI to download; resumed one at a time while the app is active.
    private(set) var interrupted: Set<Int64> = []
    private var wentToBackground = false
    @ObservationIgnored private var backgroundObserver: NSObjectProtocol?
    @ObservationIgnored private var thermalObserver: NSObjectProtocol?
    @ObservationIgnored private var resuming = false
    /// Set by the app from its scene phase: model work only starts while the app is in front.
    @ObservationIgnored var appIsActive = false
    /// Notes the "AI is ready" pass already ran once, so a silent recording is not re-run every launch.
    @ObservationIgnored private var autoTried: Set<Int64> = Set((UserDefaults.standard.array(forKey: "pipeline.autoTried") as? [Int64]) ?? [])
    private(set) var activeNoteID: Int64?
    /// Last failure message per note, so the note screen can show why nothing was generated.
    private(set) var lastFailure: [Int64: String] = [:]
    var isBusy: Bool { stage.isWorking }

    private var bgTask: UIBackgroundTaskIdentifier = .invalid

    static var heatMessage: String { String(localized: "Paused to let the phone cool down. It will continue automatically.") }
    static var waitingMessage: String { String(localized: "Waiting for Summaverick's AI to finish downloading") }
    private static var tooHot: Bool { Heat.isTooHot(ProcessInfo.processInfo.thermalState) }

    private init() {
        // Cooled down: carry on with the notes that were paused for heat.
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, !Self.tooHot, self.appIsActive else { return }
                Task { await self.resumeInterrupted() }
            }
        }
    }

    /// Stops before a model run when the phone is too hot; the note resumes when it has cooled down.
    private func pausedForHeat(_ id: Int64) -> Bool {
        guard Self.tooHot else { return false }
        NSLog("[sve] pausing note %lld: thermal state %ld", id, ProcessInfo.processInfo.thermalState.rawValue)
        interrupted.insert(id)
        lastFailure[id] = Self.heatMessage
        stage = .paused(Self.heatMessage)
        return true
    }

    private func markAutoTried(_ id: Int64) {
        guard autoTried.insert(id).inserted else { return }
        UserDefaults.standard.set(autoTried.sorted(), forKey: "pipeline.autoTried")
    }

    /// Full run for a fresh recording. Returns the note id as soon as it exists.
    @discardableResult
    func process(audio: URL, duration: Int64) async -> Int64? {
        let engine = CoreEngine.shared
        let title = "Recording " + Self.titleFormatter.string(from: Date())
        let noteLang = AppSettings.shared.notesLanguage
        // The recording is always kept, even when the AI cannot run yet.
        let id = await engine.createNote(title: title, lang: noteLang, audioPath: audio.path, transcript: nil, durationSeconds: duration)
        guard id > 0 else { stage = .failed(String(localized: "Could not save the note.")); return nil }
        NotificationCenter.default.post(name: .notesChanged, object: nil)

        // Not downloaded yet: the note waits and is processed when the AI is ready (processWaitingNotes).
        guard ModelManager.shared.isReady else { return id }
        // Another note is being written: queue this one behind it.
        guard !isBusy else { interrupted.insert(id); return id }
        beginBackground()
        defer { endBackground() }
        activeNoteID = id
        lastFailure[id] = nil
        defer { if !interrupted.contains(id) { markAutoTried(id) } }
        if pausedForHeat(id) { return id }

        // 1. transcript
        stage = .transcribing(0)
        let pieces: [TranscriptPiece]
        do {
            pieces = try await transcribe(audio, noteID: id)
        } catch {
            NSLog("[sve] transcription failed: %@", String(describing: error))
            if !pausedForHeat(id) { stage = .failed(error.localizedDescription) }
            return id
        }
        let transcript = Self.joined(pieces)
        await engine.setPieces(id, pieces)
        await engine.setTranscript(id, transcript)
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        if transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            stage = .failed(String(localized: "No speech was detected in the recording."))
            return id
        }

        // 2. notes
        await generateOutputs(noteID: id, transcript: transcript, lang: noteLang, setTitle: true)
        return id
    }

    /// Re-runs the AI stage for an existing note (resume after interruption, or after a model change).
    func regenerate(noteID: Int64) async {
        guard !isBusy, let note = await CoreEngine.shared.note(noteID) else { return }
        guard ModelManager.shared.isReady else { lastFailure[noteID] = Self.waitingMessage; return }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        if pausedForHeat(noteID) { return }

        // No transcript yet (earlier transcription failed): redo it from the audio if we still have it.
        var t = note.transcript ?? ""
        if t.isEmpty {
            guard let path = AudioRecorder.existingAudioPath(note.audio_path) else {
                stage = .failed(String(localized: "No transcript and the audio is no longer available.")); return
            }
            stage = .transcribing(0)
            var pieces: [TranscriptPiece] = []
            do {
                pieces = try await transcribe(URL(fileURLWithPath: path), noteID: noteID)
                t = Self.joined(pieces)
            } catch {
                NSLog("[sve] transcription failed: %@", String(describing: error))
                if !pausedForHeat(noteID) { stage = .failed(error.localizedDescription) }
                return
            }
            if t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                stage = .failed(String(localized: "No speech was detected in the recording.")); return
            }
            await CoreEngine.shared.setPieces(noteID, pieces)
            await CoreEngine.shared.setTranscript(noteID, t)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }
        await generateOutputs(noteID: noteID, transcript: t, lang: AppSettings.shared.notesLanguage, setTitle: note.title.hasPrefix("Recording "))
    }

    /// Audio → text. The model hears the recording itself, so any language mix works. Stops (the
    /// progress callback cancels) as soon as the phone gets too hot; the caller treats that as a pause.
    private func transcribe(_ audio: URL, noteID: Int64) async throws -> [TranscriptPiece] {
        try await loadSelectedModel()
        stage = .transcribing(0)
        return try await CoreEngine.shared.transcribeAudioPieces(path: audio.path, shouldStop: { Self.tooHot }) { done, total in
            Task { @MainActor in self.stage = .transcribing(total > 0 ? Double(done) / Double(total) : 0) }
        }
    }

    private static func joined(_ pieces: [TranscriptPiece]) -> String {
        pieces.map(\.text).joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func loadSelectedModel() async throws {
        stage = .loadingModel
        try await ModelManager.shared.loadIntoCore()
    }

    private func generateOutputs(noteID id: Int64, transcript: String, lang: String, setTitle: Bool) async {
        let engine = CoreEngine.shared
        guard ModelManager.shared.isReady else {
            lastFailure[id] = Self.waitingMessage
            stage = .idle
            return
        }
        if pausedForHeat(id) { return }
        do {
            try await loadSelectedModel()
        } catch {
            stage = .failed(error.localizedDescription)
            return
        }

        // Reminders, tasks and profile changes first, so they are ready even if note writing is interrupted.
        // A failure here never fails the notes.
        do {
            if await engine.pieces(id).isEmpty {   // note from before pieces existed
                let dur = await engine.note(id)?.duration_seconds ?? 0
                await engine.setPieces(id, PiecesBackfill.pieces(transcript: transcript, durationSeconds: dur))
            }
            stage = .extracting
            do {
                let n = try await engine.extract(noteID: id)
                NSLog("[sve] extract added %d suggestions", n)
            } catch {
                NSLog("[sve] extract failed: %@", String(describing: error))
            }
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
        }
        if pausedForHeat(id) { return }

        if setTitle, let t = await engine.suggestTitle(transcript: transcript, lang: lang) {
            await engine.setTitle(id, t)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }

        for kind in OutputKind.allCases {
            if pausedForHeat(id) { return }
            stage = .generating(kind, "final", 0, 1)
            do {
                let text = try await engine.generate(transcript: transcript, kind: kind, lang: lang) { phase, done, total in
                    Task { @MainActor in self.stage = .generating(kind, phase, done, total) }
                }
                await engine.setOutput(id, kind, text)
                NotificationCenter.default.post(name: .notesChanged, object: nil)
            } catch {
                stage = .failed(error.localizedDescription)
                return
            }
        }
        _ = await engine.generateInsight(noteID: id)
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        stage = .done
    }

    /// "Find reminders and tasks" on a note that was never extracted (an older recording): extraction
    /// only, with the same busy guard as a recording.
    func findReminders(noteID: Int64) async {
        guard !isBusy else { return }
        guard ModelManager.shared.isReady else { lastFailure[noteID] = Self.waitingMessage; return }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil
        if Self.tooHot {
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
            let n = try await engine.extract(noteID: noteID)
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
        // Keep the screen on while the model works: a locked phone stops GPU work and the run fails.
        UIApplication.shared.isIdleTimerDisabled = true
        if backgroundObserver == nil {
            backgroundObserver = NotificationCenter.default.addObserver(
                forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { if self?.isBusy == true { self?.wentToBackground = true } }
            }
        }
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "sve.process") { [weak self] in
            self?.endBackground()
        }
    }
    private func endBackground() {
        UIApplication.shared.isIdleTimerDisabled = false
        if bgTask != .invalid { UIApplication.shared.endBackgroundTask(bgTask); bgTask = .invalid }
        // A recording queued behind this run (or a paused note) goes next.
        if !interrupted.isEmpty && !resuming { Task { await self.resumeInterrupted() } }
    }

    /// Re-runs cut-off and waiting notes, oldest first, one at a time, while the app is active, the
    /// AI is ready and the phone is not too hot. Each note is tried once per pass, so a note that is
    /// paused again (heat, background) waits for the next pass instead of looping.
    func resumeInterrupted() async {
        guard !resuming else { return }
        resuming = true
        defer { resuming = false }
        var tried: Set<Int64> = []
        while !isBusy, appIsActive, ModelManager.shared.isReady, !Self.tooHot,
              let id = interrupted.sorted().first(where: { !tried.contains($0) }) {
            tried.insert(id)
            interrupted.remove(id)
            await regenerate(noteID: id)
            if !interrupted.contains(id) { markAutoTried(id) }
        }
    }

    /// Once the AI is ready: every note with audio and no transcript (recorded while it downloaded)
    /// is queued and processed one at a time.
    func processWaitingNotes() async {
        guard ModelManager.shared.isReady else { return }
        let engine = CoreEngine.shared
        var candidates: [WaitingNotes.Candidate] = []
        for n in await engine.notes(limit: 10_000) where n.has_audio && !autoTried.contains(n.id) {
            guard let note = await engine.note(n.id) else { continue }
            candidates.append(.init(id: n.id,
                                    hasAudio: AudioRecorder.existingAudioPath(note.audio_path) != nil,
                                    hasTranscript: !(note.transcript ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty))
        }
        let ids = WaitingNotes.toProcess(candidates, alreadyTried: autoTried)
        if !ids.isEmpty { NSLog("[sve] %ld notes waiting for the AI", ids.count) }
        interrupted.formUnion(ids)
        await resumeInterrupted()
    }

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
