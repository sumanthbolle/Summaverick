// Assistant API of the core (sve_core.h, "personal assistant" section), as typed Swift.
// Model calls run inside the CoreEngine actor on its sve_ctx; store-only calls are nonisolated and run on
// CoreStore's context (CoreEngine.store), so each sve_ctx is only ever used from one thread at a time.
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

    private nonisolated func encodeJSON<T: Encodable>(_ v: T) -> String? {
        guard let d = try? JSONEncoder().encode(v) else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private nonisolated func list<T: Decodable>(_ type: T.Type, _ json: UnsafeMutablePointer<CChar>?) -> [T] {
        decode([T].self, take(json)) ?? []
    }

    private func progressBox(_ onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) -> ProgressBox {
        ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled })
    }

    // MARK: profile (store context)

    nonisolated func profile() async -> Profile? { await store { self.decode(Profile.self, self.take(sve_profile_get($0))) } }

    nonisolated func setProfile(_ p: Profile) async throws {
        guard let json = encodeJSON(p), await store({ sve_profile_set($0, json) }) == 0 else {
            throw AssistantError.failed(String(localized: "save the profile"))
        }
    }

    // MARK: medicines (store context)

    nonisolated func medicines() async -> [Medicine] { await store { self.list(Medicine.self, sve_medicines_json($0)) } }

    /// Saves a medicine (id 0 = new) and adds a pending reminder suggestion for it. Returns the medicine id.
    nonisolated func saveMedicine(_ m: Medicine) async throws -> Int64 {
        guard let json = encodeJSON(m) else { throw AssistantError.badJSON }
        let id = await store { sve_medicine_save($0, json) }
        guard id > 0 else { throw AssistantError.failed(String(localized: "save the medicine")) }
        return id
    }

    @discardableResult nonisolated func deleteMedicine(_ id: Int64) async -> Bool { await store { sve_medicine_delete($0, id) == 0 } }

    /// Routine times implied by the words of "when" (the core's one keyword table, against the saved
    /// routine). [] when there is no keyword or no routine time for it.
    nonisolated func resolveTimes(_ when: String) async -> [String] {
        await store { self.decode([String].self, self.take(sve_resolve_times($0, when))) ?? [] }
    }

    /// Deletes everything the assistant stored (profile, medicines, memory, reminders, tasks, topics,
    /// suggestions, insights, transcript pieces). Notes are deleted separately; the caller resyncs reminders.
    /// Runs on the store context, so it does not wait for a model call in flight: the caller stops memory work
    /// first (yieldMemories) and refuses while a note is being written; a model call still running (Ask, a topic
    /// summary) only reads, or updates a row that is gone by then.
    nonisolated func wipeAssistant() async throws {
        guard await store({ sve_assistant_wipe($0) }) == 0 else { throw AssistantError.failed(String(localized: "delete the assistant data")) }
    }

    // MARK: suggestions (store context)

    /// status: "pending", "accepted", "rejected"; nil = all.
    nonisolated func suggestions(status: String? = "pending") async -> [Suggestion] {
        await store { self.list(Suggestion.self, sve_suggestions_json($0, status)) }
    }

    nonisolated func accept(_ id: Int64, edited: SuggestionPayload? = nil) async throws -> AcceptResult {
        var json: String?
        if let edited {   // never fall back to "as proposed" when an edit can't be encoded
            guard let j = encodeJSON(edited) else { throw AssistantError.badJSON }
            json = j
        }
        let payload = json
        let r = await store { sve_suggestion_accept($0, id, payload) }
        switch r {
        case 1...: return .created(r)
        case 0: return .applied
        case -2: return .staleReflagged
        default: throw AssistantError.failed(String(localized: "accept the suggestion"))
        }
    }

    /// false when it is gone or no longer pending.
    @discardableResult nonisolated func reject(_ id: Int64) async -> Bool { await store { sve_suggestion_reject($0, id) == 0 } }

    // MARK: reminders and tasks (store context)

    /// Like `reminders`, but throws when the engine is unavailable, the call fails, or the JSON does not decode,
    /// so callers can tell "no reminders" from "could not read".
    nonisolated func remindersStrict(activeOnly: Bool = false) async throws -> [Reminder] {
        try await store { c in
            guard c != nil else { throw AssistantError.failed(String(localized: "read reminders (engine not ready)")) }
            guard let json = self.take(sve_reminders_json(c, activeOnly ? 1 : 0)) else {
                throw AssistantError.failed(String(localized: "read reminders"))
            }
            guard let rows = self.decode([Reminder].self, json) else { throw AssistantError.badJSON }
            return rows
        }
    }

    nonisolated func reminders(activeOnly: Bool = false) async -> [Reminder] {
        await store { self.list(Reminder.self, sve_reminders_json($0, activeOnly ? 1 : 0)) }
    }
    nonisolated func setReminderActive(_ id: Int64, _ active: Bool) async {
        await store { _ = sve_reminder_set_active($0, id, active ? 1 : 0) }
    }
    nonisolated func tasks(openOnly: Bool = false) async -> [TaskItem] {
        await store { self.list(TaskItem.self, sve_tasks_json($0, openOnly ? 1 : 0)) }
    }
    nonisolated func setTaskDone(_ id: Int64, _ done: Bool) async { await store { _ = sve_task_set_done($0, id, done ? 1 : 0) } }

    /// Creates a reminder the person made themselves (their own edit, applied directly). Returns its id.
    /// Throws if it cannot be encoded or the core refuses it (no text, no valid time, bad date).
    nonisolated func createReminder(_ r: NewReminder) async throws -> Int64 {
        let json = encodeJSON(r)
        return try await store { c in
            guard c != nil else { throw AssistantError.failed(String(localized: "save the reminder (engine not ready)")) }
            guard let json else { throw AssistantError.badJSON }
            let id = sve_reminder_create(c, json)
            guard id > 0 else { throw AssistantError.failed(String(localized: "save the reminder")) }
            return id
        }
    }

    /// Ids of the saved medicines named in a text, by the core's own matcher (the one sve_reminder_create
    /// checks against). Throws when the core cannot answer.
    nonisolated func medicinesNamed(_ text: String) async throws -> [Int64] {
        try await store { c in
            guard c != nil else { throw AssistantError.failed(String(localized: "read medicines (engine not ready)")) }
            guard let json = self.take(sve_medicines_named(c, text)) else { throw AssistantError.failed(String(localized: "read medicines")) }
            guard let ids = self.decode([Int64].self, json) else { throw AssistantError.badJSON }
            return ids
        }
    }

    /// Creates a to-do the person made themselves. Returns its id. Throws on encode failure or refusal.
    nonisolated func createTask(_ t: NewTask) async throws -> Int64 {
        let json = encodeJSON(t)
        return try await store { c in
            guard c != nil else { throw AssistantError.failed(String(localized: "save the to-do (engine not ready)")) }
            guard let json else { throw AssistantError.badJSON }
            let id = sve_task_create(c, json)
            guard id > 0 else { throw AssistantError.failed(String(localized: "save the to-do")) }
            return id
        }
    }

    // MARK: topics (store context, except the summary: it runs the model)

    /// kind: "topic", "person", or nil for both.
    nonisolated func topics(kind: String? = nil) async -> [Topic] { await store { self.list(Topic.self, sve_topics_json($0, kind)) } }
    nonisolated func topicNotes(_ topicID: Int64) async -> [TopicNote] {
        await store { self.list(TopicNote.self, sve_topic_notes_json($0, topicID)) }
    }
    nonisolated func renameTopic(_ id: Int64, _ name: String) async { await store { _ = sve_topic_rename($0, id, name) } }
    nonisolated func mergeTopic(_ from: Int64, into: Int64) async { await store { _ = sve_topic_merge($0, from, into) } }

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

    // MARK: memory (store context)

    /// kind: "fact", "preference", "vocab", or nil for all.
    nonisolated func memory(kind: String? = nil) async -> [MemoryItem] { await store { self.list(MemoryItem.self, sve_memory_json($0, kind)) } }
    @discardableResult nonisolated func updateMemory(_ id: Int64, _ text: String) async -> Bool {
        await store { sve_memory_update($0, id, text) == 0 }
    }
    @discardableResult nonisolated func deleteMemory(_ id: Int64) async -> Bool { await store { sve_memory_delete($0, id) == 0 } }
    @discardableResult nonisolated func addVocab(_ term: String) async -> Int64 { await store { sve_vocab_add($0, term) } }

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
    nonisolated func noteExtracted(_ noteID: Int64) async -> Bool { await store { sve_note_extracted($0, noteID) != 0 } }

    nonisolated func latestInsight() async -> Insight? { await store { self.decode(Insight.self, self.take(sve_insight_latest($0))) } }
    /// At most one per calendar day; nil when there is nothing new, or when shouldStop cancelled it (nothing stored).
    /// Runs the model (this actor's context).
    func generateInsight(noteID: Int64, shouldStop: @escaping () -> Bool = { false }) -> Insight? {
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        return decode(Insight.self, take(sve_insight_generate(ctx, noteID, CoreEngine.progressTrampoline, user)))
    }
    nonisolated func dismissInsight(_ id: Int64) async { await store { _ = sve_insight_dismiss($0, id) } }

    // MARK: transcript pieces (store context)

    @discardableResult
    nonisolated func setPieces(_ noteID: Int64, _ pieces: [TranscriptPiece]) async -> Bool {
        guard let json = encodeJSON(pieces) else { return false }
        return await store { sve_note_set_pieces($0, noteID, json) == 0 }
    }

    nonisolated func pieces(_ noteID: Int64) async -> [TranscriptPiece] {
        await store { self.list(TranscriptPiece.self, sve_note_pieces($0, noteID)) }
    }

    // MARK: sections (summaries, notes and titles run the model; the stored rows are on the store context)

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
    nonisolated func setSections(_ noteID: Int64, _ sections: [NoteSection]) async -> Bool {
        guard let json = encodeJSON(sections) else { return false }
        return await store { sve_note_set_sections($0, noteID, json) == 0 }
    }

    nonisolated func sections(_ noteID: Int64) async -> [NoteSection] {
        await store { self.list(NoteSection.self, sve_note_sections($0, noteID)) }
    }

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

    nonisolated func chapters(_ noteID: Int64) async -> [Chapter] { await store { self.list(Chapter.self, sve_chapters_json($0, noteID)) } }

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
