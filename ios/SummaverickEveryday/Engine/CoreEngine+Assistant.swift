// Assistant API of the core (sve_core.h, "personal assistant" section), as typed Swift.
// Everything runs inside the CoreEngine actor, so the one sve_ctx is never used from two threads.
import Foundation

/// A chapter of a note: consecutive sections on the same topic (sve_chapters_json).
struct Chapter: Decodable, Equatable {
    let title: String
    let start_sec: Int
}

extension CoreEngine {
    enum AssistantError: LocalizedError {
        case failed(String), badJSON
        var errorDescription: String? {
            switch self {
            case .failed(let what): return String(localized: "Could not \(what).", comment: "%@ is an action such as \"save the reminder\"")
            case .badJSON: return String(localized: "Could not read the assistant's reply.")
            }
        }
    }

    private func encodeJSON<T: Encodable>(_ v: T) -> String? {
        guard let d = try? JSONEncoder().encode(v) else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private func list<T: Decodable>(_ type: T.Type, _ json: UnsafeMutablePointer<CChar>?) -> [T] {
        decode([T].self, take(json)) ?? []
    }

    private func progressBox(_ onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) -> ProgressBox {
        ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled })
    }

    // MARK: profile

    func profile() -> Profile? { decode(Profile.self, take(sve_profile_get(ctx))) }

    func setProfile(_ p: Profile) throws {
        guard let json = encodeJSON(p), sve_profile_set(ctx, json) == 0 else { throw AssistantError.failed(String(localized: "save the profile")) }
    }

    // MARK: medicines

    func medicines() -> [Medicine] { list(Medicine.self, sve_medicines_json(ctx)) }

    /// Saves a medicine (id 0 = new) and adds a pending reminder suggestion for it. Returns the medicine id.
    func saveMedicine(_ m: Medicine) throws -> Int64 {
        guard let json = encodeJSON(m) else { throw AssistantError.badJSON }
        let id = sve_medicine_save(ctx, json)
        guard id > 0 else { throw AssistantError.failed(String(localized: "save the medicine")) }
        return id
    }

    @discardableResult func deleteMedicine(_ id: Int64) -> Bool { sve_medicine_delete(ctx, id) == 0 }

    /// Routine times implied by the words of "when" (the core's one keyword table, against the saved
    /// routine). [] when there is no keyword or no routine time for it.
    func resolveTimes(_ when: String) -> [String] { decode([String].self, take(sve_resolve_times(ctx, when))) ?? [] }

    /// Deletes everything the assistant stored (profile, medicines, memory, reminders, tasks, topics,
    /// suggestions, insights, transcript pieces). Notes are deleted separately; the caller resyncs reminders.
    func wipeAssistant() throws {
        guard sve_assistant_wipe(ctx) == 0 else { throw AssistantError.failed(String(localized: "delete the assistant data")) }
    }

    // MARK: suggestions

    /// status: "pending", "accepted", "rejected"; nil = all.
    func suggestions(status: String? = "pending") -> [Suggestion] {
        list(Suggestion.self, sve_suggestions_json(ctx, status))
    }

    func accept(_ id: Int64, edited: SuggestionPayload? = nil) throws -> AcceptResult {
        var json: String?
        if let edited {   // never fall back to "as proposed" when an edit can't be encoded
            guard let j = encodeJSON(edited) else { throw AssistantError.badJSON }
            json = j
        }
        let r = sve_suggestion_accept(ctx, id, json)
        switch r {
        case 1...: return .created(r)
        case 0: return .applied
        case -2: return .staleReflagged
        default: throw AssistantError.failed(String(localized: "accept the suggestion"))
        }
    }

    /// false when it is gone or no longer pending.
    @discardableResult func reject(_ id: Int64) -> Bool { sve_suggestion_reject(ctx, id) == 0 }

    // MARK: reminders and tasks

    /// Like `reminders`, but throws when the engine is unavailable, the call fails, or the JSON does not decode,
    /// so callers can tell "no reminders" from "could not read".
    func remindersStrict(activeOnly: Bool = false) throws -> [Reminder] {
        guard ctx != nil else { throw AssistantError.failed(String(localized: "read reminders (engine not ready)")) }
        guard let json = take(sve_reminders_json(ctx, activeOnly ? 1 : 0)) else { throw AssistantError.failed(String(localized: "read reminders")) }
        guard let rows = decode([Reminder].self, json) else { throw AssistantError.badJSON }
        return rows
    }

    func reminders(activeOnly: Bool = false) -> [Reminder] { list(Reminder.self, sve_reminders_json(ctx, activeOnly ? 1 : 0)) }
    func setReminderActive(_ id: Int64, _ active: Bool) { _ = sve_reminder_set_active(ctx, id, active ? 1 : 0) }
    func tasks(openOnly: Bool = false) -> [TaskItem] { list(TaskItem.self, sve_tasks_json(ctx, openOnly ? 1 : 0)) }
    func setTaskDone(_ id: Int64, _ done: Bool) { _ = sve_task_set_done(ctx, id, done ? 1 : 0) }

    /// Creates a reminder the person made themselves (their own edit, applied directly). Returns its id.
    /// Throws if it cannot be encoded or the core refuses it (no text, no valid time, bad date).
    func createReminder(_ r: NewReminder) throws -> Int64 {
        guard ctx != nil else { throw AssistantError.failed(String(localized: "save the reminder (engine not ready)")) }
        guard let json = encodeJSON(r) else { throw AssistantError.badJSON }
        let id = sve_reminder_create(ctx, json)
        guard id > 0 else { throw AssistantError.failed(String(localized: "save the reminder")) }
        return id
    }

    /// Ids of the saved medicines named in a text, by the core's own matcher (the one sve_reminder_create
    /// checks against). Throws when the core cannot answer.
    func medicinesNamed(_ text: String) throws -> [Int64] {
        guard ctx != nil else { throw AssistantError.failed(String(localized: "read medicines (engine not ready)")) }
        guard let json = take(sve_medicines_named(ctx, text)) else { throw AssistantError.failed(String(localized: "read medicines")) }
        guard let ids = decode([Int64].self, json) else { throw AssistantError.badJSON }
        return ids
    }

    /// Creates a to-do the person made themselves. Returns its id. Throws on encode failure or refusal.
    func createTask(_ t: NewTask) throws -> Int64 {
        guard ctx != nil else { throw AssistantError.failed(String(localized: "save the to-do (engine not ready)")) }
        guard let json = encodeJSON(t) else { throw AssistantError.badJSON }
        let id = sve_task_create(ctx, json)
        guard id > 0 else { throw AssistantError.failed(String(localized: "save the to-do")) }
        return id
    }

    // MARK: topics

    /// kind: "topic", "person", or nil for both.
    func topics(kind: String? = nil) -> [Topic] { list(Topic.self, sve_topics_json(ctx, kind)) }
    func topicNotes(_ topicID: Int64) -> [TopicNote] { list(TopicNote.self, sve_topic_notes_json(ctx, topicID)) }
    func renameTopic(_ id: Int64, _ name: String) { _ = sve_topic_rename(ctx, id, name) }
    func mergeTopic(_ from: Int64, into: Int64) { _ = sve_topic_merge(ctx, from, into) }

    func topicSummary(_ topicID: Int64, lang: String,
                      onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> TopicSummary {
        guard let ctx else { throw CoreError.initFailed }
        let box = progressBox(onProgress)
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_topic_summary(ctx, topicID, lang, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        guard let s = decode(TopicSummary.self, json) else { throw AssistantError.badJSON }
        return s
    }

    // MARK: memory

    /// kind: "fact", "preference", "vocab", or nil for all.
    func memory(kind: String? = nil) -> [MemoryItem] { list(MemoryItem.self, sve_memory_json(ctx, kind)) }
    @discardableResult func updateMemory(_ id: Int64, _ text: String) -> Bool { sve_memory_update(ctx, id, text) == 0 }
    @discardableResult func deleteMemory(_ id: Int64) -> Bool { sve_memory_delete(ctx, id) == 0 }
    @discardableResult func addVocab(_ term: String) -> Int64 { sve_vocab_add(ctx, term) }

    // MARK: ask

    func ask(_ question: String, onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> AskAnswer {
        guard let ctx else { throw CoreError.initFailed }
        let box = progressBox(onProgress)
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_assistant_ask(ctx, question, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        guard let a = decode(AskAnswer.self, json) else { throw AssistantError.badJSON }
        return a
    }

    // MARK: extraction and insight

    /// Runs extraction on a note's pieces. Returns the number of pending suggestions added.
    func extract(noteID: Int64, shouldStop: @escaping () -> Bool = { false },
                 onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> Int {
        guard let ctx else { throw CoreError.initFailed }
        let box = ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        let n = sve_extract(ctx, noteID, CoreEngine.progressTrampoline, user)
        guard n >= 0 else { throw AssistantError.failed(String(localized: "find reminders and tasks")) }
        return Int(n)
    }

    /// Whether extraction has run on this note (core flag "extracted.<id>", or the note already has
    /// suggestions or topics). An error counts as yes, so the button is never offered on a guess.
    func noteExtracted(_ noteID: Int64) -> Bool { sve_note_extracted(ctx, noteID) != 0 }

    func latestInsight() -> Insight? { decode(Insight.self, take(sve_insight_latest(ctx))) }
    /// At most one per calendar day; nil when there is nothing new.
    func generateInsight(noteID: Int64) -> Insight? { decode(Insight.self, take(sve_insight_generate(ctx, noteID))) }
    func dismissInsight(_ id: Int64) { _ = sve_insight_dismiss(ctx, id) }

    // MARK: transcript pieces

    @discardableResult
    func setPieces(_ noteID: Int64, _ pieces: [TranscriptPiece]) -> Bool {
        guard let json = encodeJSON(pieces) else { return false }
        return sve_note_set_pieces(ctx, noteID, json) == 0
    }

    func pieces(_ noteID: Int64) -> [TranscriptPiece] { list(TranscriptPiece.self, sve_note_pieces(ctx, noteID)) }

    // MARK: sections

    /// Summary of one section's pieces, on `backend` when given (switched here, in the same actor turn as the
    /// run). nil when `shouldStart` said no once the engine was free (nothing ran). Throws when the run was
    /// cancelled (`shouldStop`) or there was nothing it could summarise.
    func sectionSummarize(pieces: [TranscriptPiece], lang: String, backend: ModelBackend? = nil,
                          shouldStart: () -> Bool = { true },
                          shouldStop: @escaping () -> Bool = { false }) throws -> SectionSummary? {
        guard let ctx else { throw CoreError.initFailed }
        guard shouldStart() else { return nil }
        if let backend { try ensureBackend(backend) }
        guard let json = encodeJSON(pieces) else { throw AssistantError.badJSON }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_section_summarize(ctx, json, lang, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        guard let summary = decode(SectionSummary.self, out) else { throw AssistantError.badJSON }
        return summary
    }

    /// Replaces the note's sections. False when it could not be stored, for example the note is gone.
    @discardableResult
    func setSections(_ noteID: Int64, _ sections: [NoteSection]) -> Bool {
        guard let json = encodeJSON(sections) else { return false }
        return sve_note_set_sections(ctx, noteID, json) == 0
    }

    func sections(_ noteID: Int64) -> [NoteSection] { list(NoteSection.self, sve_note_sections(ctx, noteID)) }

    /// An output written from the note's sections. nil when the note has no sections, or on cancel or failure:
    /// callers check their own cancel flag before falling back to `generate`.
    func generateFromSections(noteID: Int64, kind: OutputKind, lang: String, shouldStop: @escaping () -> Bool = { false },
                              onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) -> String? {
        guard let ctx else { return nil }
        let box = ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        return take(sve_generate_from_sections(ctx, noteID, kind.cKind, lang, CoreEngine.progressTrampoline, user))
    }

    /// A title from the section titles; nil when there are no sections or no model.
    func titleFromSections(noteID: Int64, lang: String) -> String? {
        guard let ctx else { return nil }
        let t = take(sve_title_from_sections(ctx, noteID, lang))?.trimmingCharacters(in: .whitespacesAndNewlines)
        return (t?.isEmpty ?? true) ? nil : t
    }

    func chapters(_ noteID: Int64) -> [Chapter] { list(Chapter.self, sve_chapters_json(ctx, noteID)) }

    /// Same as transcribeAudio, but keeps the start second of each ~28 s piece.
    /// `shouldStop` is asked at every progress step; true cancels the run (it then throws).
    func transcribeAudioPieces(path: String, shouldStop: @escaping () -> Bool = { false },
                               onProgress: @escaping (Int, Int) -> Void = { _, _ in }) throws -> [TranscriptPiece] {
        guard let ctx else { throw CoreError.initFailed }
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = ProgressBox(onProgress: { _, d, t in onProgress(d, t) }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_transcribe_audio_pieces(ctx, path, CoreEngine.progressTrampoline, user)),
              let pieces = decode([TranscriptPiece].self, json) else {
            throw CoreError.transcriptionFailed
        }
        return pieces
    }

    /// Result of one step of incremental transcription (sve_transcribe_next_piece).
    enum NextPiece: Equatable {
        /// Transcribed [start, endSample); text may be empty (silence).
        case piece(startSec: Int, endSample: Int64, text: String, conf: Double?)
        /// Still recording and under 31 s after the cursor: ask again later.
        case wait
        /// Nothing left to transcribe (final only).
        case end
        /// `shouldStart` said no once the engine was free: nothing was run.
        case skipped
    }

    private struct NextPieceJSON: Decodable {
        let status: String
        let start_sec: Int?
        let end_sample: Int64?
        let text: String?
        let conf: Double?
    }

    /// Plans and transcribes the next ~28 s piece of a WAV that may still be being recorded, from
    /// `startSample`, on `backend` (switched here, in the same actor turn as the run). isFinal: the
    /// recording has stopped. shouldStart is asked once the engine is free (a Stop while this call waited
    /// skips it). Throws on failure; cancels (throws) when `shouldStop` turns true during the run.
    func transcribeNextPiece(path: String, startSample: Int64, isFinal: Bool, previousText: String?,
                             backend: ModelBackend = .cpu,
                             shouldStart: () -> Bool = { true },
                             shouldStop: @escaping () -> Bool = { false }) throws -> NextPiece {
        guard let ctx else { throw CoreError.initFailed }
        guard shouldStart() else { return .skipped }
        try ensureBackend(backend)
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_transcribe_next_piece(ctx, path, startSample, isFinal ? 1 : 0, previousText,
                                                        CoreEngine.progressTrampoline, user)),
              let r = decode(NextPieceJSON.self, json) else {
            throw CoreError.transcriptionFailed
        }
        switch r.status {
        case "wait": return .wait
        case "end": return .end
        case "piece":
            guard let end = r.end_sample, end > startSample else { throw CoreError.transcriptionFailed }
            return .piece(startSec: r.start_sec ?? Int(startSample / 16_000), endSample: end, text: r.text ?? "", conf: r.conf)
        default: throw CoreError.transcriptionFailed
        }
    }
}
