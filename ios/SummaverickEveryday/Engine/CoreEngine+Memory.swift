// Memory API of the core (sve_core.h, "memories (the second brain)" section), as typed Swift.
// Making memories runs the model inside the CoreEngine actor; everything else here is a store-only call on
// CoreStore's context (CoreEngine.store), so the review queue and the memory screens never wait behind it.
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

    private nonisolated func memoryJSON<T: Decodable>(_ type: T.Type, _ json: UnsafeMutablePointer<CChar>?) -> T? {
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
    nonisolated func memorySectionsDue(_ noteID: Int64) async -> [Int] {
        await store { self.memoryJSON([Int].self, sve_memory_sections_due($0, noteID)) ?? [] }
    }

    /// Notes with a section whose memories are not made yet, newest first.
    nonisolated func notesNeedingMemories(limit: Int) async -> [Int64] {
        await store { self.memoryJSON([Int64].self, sve_notes_needing_memories($0, Int32(clamping: limit))) ?? [] }
    }

    // MARK: reading (store context)

    nonisolated func memories(_ filter: MemoryFilter = .all) async -> [BrainMemory] {
        guard let d = try? JSONEncoder().encode(filter), let json = String(data: d, encoding: .utf8) else { return [] }
        return await store { self.memoryJSON([BrainMemory].self, sve_memories_json($0, json)) ?? [] }
    }

    /// One memory with its history (the versions it replaced); nil when it is gone.
    nonisolated func memory(_ id: Int64) async -> BrainMemory? { await store { self.memoryJSON(BrainMemory.self, sve_memory_get($0, id)) } }

    /// One memory with its history, telling "gone" apart from "could not read".
    nonisolated func lookupMemory(_ id: Int64) async -> MemoryLookup { await store { MemoryLookup(json: self.take(sve_memory_get($0, id))) } }

    /// Memories waiting for the person, newest recording first, each with its pending card (if any);
    /// nil when the queue could not be read.
    nonisolated func reviewQueue() async -> [ReviewItem]? { await store { ReviewItem.queue(fromJSON: self.take(sve_review_queue_json($0))) } }

    // MARK: the person's changes (store context; medicine memories follow their medicine: these refuse them)

    /// "Keep" (kept: true) or "Wrong". Both take the memory out of the review queue.
    @discardableResult nonisolated func reviewMemory(_ id: Int64, _ kept: Bool) async -> Bool {
        await store { sve_memory_review($0, id, kept ? "kept" : "wrong") == 0 }
    }

    /// The person's own words for a memory, applied directly.
    @discardableResult nonisolated func editMemory(_ id: Int64, _ text: String) async -> Bool {
        await store { sve_memory_edit($0, id, text) == 0 }
    }

    @discardableResult nonisolated func removeMemory(_ id: Int64) async -> Bool { await store { sve_memory_remove($0, id) == 0 } }

    @discardableResult nonisolated func setMemoryDone(_ id: Int64, _ done: Bool) async -> Bool {
        await store { sve_memory_set_done($0, id, done ? 1 : 0) == 0 }
    }
}
