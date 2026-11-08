// Memory API of the core (sve_core.h, "memories (the second brain)" section), as typed Swift.
// Everything runs inside the CoreEngine actor, so the one sve_ctx is never used from two threads.
import Foundation

extension CoreEngine {
    enum MemoryError: Error, Equatable {
        /// -1: the note or section is gone (deleted meanwhile), bad input, or no model.
        case unavailable
        /// -2: cancelled by `shouldStop`; nothing was stored and the section stays due.
        case cancelled
        /// -3: the model call itself failed; nothing was stored and the section stays due.
        case modelFailed
    }

    private func memoryJSON<T: Decodable>(_ type: T.Type, _ json: UnsafeMutablePointer<CChar>?) -> T? {
        decode(type, take(json))
    }

    // MARK: writing memories

    /// Makes the memories of one section of a note (one memory-only model call, checked against the section's
    /// words), on `backend` when given (switched here, in the same actor turn as the run). Returns how many were
    /// stored (0 for a section already done, or a reply that could not be used: the section is then done).
    /// `shouldStop` is asked during the run (any thread); true cancels it.
    func memoriesFromSection(noteID: Int64, idx: Int, backend: ModelBackend? = nil,
                             shouldStop: @escaping () -> Bool = { false }) throws -> Int {
        guard let ctx else { throw MemoryError.unavailable }
        if let backend { try ensureBackend(backend) }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        let r = sve_memories_from_section(ctx, noteID, Int32(clamping: idx), CoreEngine.progressTrampoline, user)
        switch MemoryPass.Step(result: r) {
        case .stored(let n): return n
        case .gone: throw MemoryError.unavailable
        case .cancelled: throw MemoryError.cancelled
        case .failed: throw MemoryError.modelFailed
        }
    }

    /// Indexes of the note's sections whose memories are not made yet, in order; [] when none (or no note).
    func memorySectionsDue(_ noteID: Int64) -> [Int] { memoryJSON([Int].self, sve_memory_sections_due(ctx, noteID)) ?? [] }

    /// Notes with a section whose memories are not made yet, newest first.
    func notesNeedingMemories(limit: Int) -> [Int64] {
        memoryJSON([Int64].self, sve_notes_needing_memories(ctx, Int32(clamping: limit))) ?? []
    }

    // MARK: reading

    func memories(_ filter: MemoryFilter = .all) -> [BrainMemory] {
        guard let d = try? JSONEncoder().encode(filter), let json = String(data: d, encoding: .utf8) else { return [] }
        return memoryJSON([BrainMemory].self, sve_memories_json(ctx, json)) ?? []
    }

    /// One memory with its history (the versions it replaced); nil when it is gone.
    func memory(_ id: Int64) -> BrainMemory? { memoryJSON(BrainMemory.self, sve_memory_get(ctx, id)) }

    /// One memory with its history, telling "gone" apart from "could not read".
    func lookupMemory(_ id: Int64) -> MemoryLookup { MemoryLookup(json: take(sve_memory_get(ctx, id))) }

    /// Memories waiting for the person, newest recording first, each with its pending card (if any);
    /// nil when the queue could not be read.
    func reviewQueue() -> [ReviewItem]? { ReviewItem.queue(fromJSON: take(sve_review_queue_json(ctx))) }

    // MARK: the person's changes (medicine memories follow their medicine: these refuse them)

    /// "Keep" (kept: true) or "Wrong". Both take the memory out of the review queue.
    @discardableResult func reviewMemory(_ id: Int64, _ kept: Bool) -> Bool {
        sve_memory_review(ctx, id, kept ? "kept" : "wrong") == 0
    }

    /// The person's own words for a memory, applied directly.
    @discardableResult func editMemory(_ id: Int64, _ text: String) -> Bool { sve_memory_edit(ctx, id, text) == 0 }

    @discardableResult func removeMemory(_ id: Int64) -> Bool { sve_memory_remove(ctx, id) == 0 }

    @discardableResult func setMemoryDone(_ id: Int64, _ done: Bool) -> Bool { sve_memory_set_done(ctx, id, done ? 1 : 0) == 0 }
}
