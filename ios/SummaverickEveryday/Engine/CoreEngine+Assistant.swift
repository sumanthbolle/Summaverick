// Assistant API of the core (sve_core.h, "personal assistant" section), as typed Swift.
// Everything runs inside the CoreEngine actor, so the one sve_ctx is never used from two threads.
import Foundation

extension CoreEngine {
    enum AssistantError: LocalizedError {
        case failed(String), badJSON
        var errorDescription: String? {
            switch self {
            case .failed(let what): return "Could not \(what)."
            case .badJSON: return "Could not read the assistant's reply."
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
        guard let json = encodeJSON(p), sve_profile_set(ctx, json) == 0 else { throw AssistantError.failed("save the profile") }
    }

    // MARK: medicines

    func medicines() -> [Medicine] { list(Medicine.self, sve_medicines_json(ctx)) }

    /// Saves a medicine (id 0 = new) and adds a pending reminder suggestion for it. Returns the medicine id.
    func saveMedicine(_ m: Medicine) throws -> Int64 {
        guard let json = encodeJSON(m) else { throw AssistantError.badJSON }
        let id = sve_medicine_save(ctx, json)
        guard id > 0 else { throw AssistantError.failed("save the medicine") }
        return id
    }

    func deleteMedicine(_ id: Int64) { _ = sve_medicine_delete(ctx, id) }

    // MARK: suggestions

    /// status: "pending", "accepted", "rejected"; nil = all.
    func suggestions(status: String? = "pending") -> [Suggestion] {
        list(Suggestion.self, sve_suggestions_json(ctx, status))
    }

    func accept(_ id: Int64, edited: SuggestionPayload? = nil) throws -> AcceptResult {
        let json: String? = edited.flatMap { encodeJSON($0) }
        let r = sve_suggestion_accept(ctx, id, json)
        switch r {
        case 1...: return .created(r)
        case 0: return .applied
        case -2: return .staleReflagged
        default: throw AssistantError.failed("accept the suggestion")
        }
    }

    func reject(_ id: Int64) { _ = sve_suggestion_reject(ctx, id) }

    // MARK: reminders and tasks

    func reminders(activeOnly: Bool = false) -> [Reminder] { list(Reminder.self, sve_reminders_json(ctx, activeOnly ? 1 : 0)) }
    func setReminderActive(_ id: Int64, _ active: Bool) { _ = sve_reminder_set_active(ctx, id, active ? 1 : 0) }
    func tasks(openOnly: Bool = false) -> [TaskItem] { list(TaskItem.self, sve_tasks_json(ctx, openOnly ? 1 : 0)) }
    func setTaskDone(_ id: Int64, _ done: Bool) { _ = sve_task_set_done(ctx, id, done ? 1 : 0) }

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
    func updateMemory(_ id: Int64, _ text: String) { _ = sve_memory_update(ctx, id, text) }
    func deleteMemory(_ id: Int64) { _ = sve_memory_delete(ctx, id) }
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
    func extract(noteID: Int64, onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> Int {
        guard let ctx else { throw CoreError.initFailed }
        let box = progressBox(onProgress)
        let user = Unmanaged.passUnretained(box).toOpaque()
        let n = sve_extract(ctx, noteID, CoreEngine.progressTrampoline, user)
        guard n >= 0 else { throw AssistantError.failed("find reminders and tasks") }
        return Int(n)
    }

    func latestInsight() -> Insight? { decode(Insight.self, take(sve_insight_latest(ctx))) }
    /// At most one per calendar day; nil when there is nothing new.
    func generateInsight(noteID: Int64) -> Insight? { decode(Insight.self, take(sve_insight_generate(ctx, noteID))) }
    func dismissInsight(_ id: Int64) { _ = sve_insight_dismiss(ctx, id) }

    // MARK: transcript pieces

    func setPieces(_ noteID: Int64, _ pieces: [TranscriptPiece]) {
        guard let json = encodeJSON(pieces) else { return }
        _ = sve_note_set_pieces(ctx, noteID, json)
    }

    func pieces(_ noteID: Int64) -> [TranscriptPiece] { list(TranscriptPiece.self, sve_note_pieces(ctx, noteID)) }

    /// Same as transcribeAudio, but keeps the start second of each ~28 s piece.
    func transcribeAudioPieces(path: String, onProgress: @escaping (Int, Int) -> Void = { _, _ in }) throws -> [TranscriptPiece] {
        guard let ctx else { throw CoreError.initFailed }
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = progressBox { _, d, t in onProgress(d, t) }
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_transcribe_audio_pieces(ctx, path, CoreEngine.progressTrampoline, user)),
              let pieces = decode([TranscriptPiece].self, json) else {
            throw CoreError.transcriptionFailed
        }
        return pieces
    }
}
