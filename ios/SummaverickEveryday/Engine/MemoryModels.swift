// Typed models for the memory half of the core API (sve_core.h, "memories (the second brain)" section).
// Decoding is lenient, as in AssistantModels: a missing key, a null or an unexpected type becomes nil/default
// instead of failing the whole list. Foundation only; checked with a standalone swiftc script together with
// TranscriptSidecar.swift (lenient helpers) and AssistantModels.swift (Suggestion).
import Foundation

/// A name, number, date or time a memory relies on, as the exact words of its quote.
struct CheckedField: Codable, Equatable, Hashable {
    /// "name" | "number" | "date" | "time"
    var kind: String
    /// The words as said in the quote.
    var text: String
    /// Names only: how the model wrote the name (e.g. "Ravi" for "రవి"); nil otherwise.
    var asWritten: String?

    init(kind: String, text: String, asWritten: String? = nil) { self.kind = kind; self.text = text; self.asWritten = asWritten }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = c.lenient(.kind) ?? ""; text = c.lenient(.text) ?? ""; asWritten = c.lenient(.asWritten)
    }
    enum CodingKeys: String, CodingKey { case kind, text, asWritten = "as_written" }

    /// What the app shows: a name said in Telugu and written in English shows the written form; everything
    /// else (and a name without one) shows the words as said. The quote itself is always shown as said.
    var shown: String {
        let w = asWritten?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return kind == "name" && !w.isEmpty ? w : text
    }
}

/// Where a memory was said: the recording, the second and the words.
struct MemorySource: Decodable, Equatable, Hashable, Identifiable {
    var noteID: Int64
    var startSec: Int
    var quote: String
    var title: String
    var id: String { "\(noteID)@\(startSec)" }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        noteID = c.lenient(.noteID) ?? 0; startSec = c.lenient(.startSec) ?? 0
        quote = c.lenient(.quote) ?? ""; title = c.lenient(.title) ?? ""
    }
    enum CodingKeys: String, CodingKey { case quote, title, noteID = "note_id", startSec = "start_sec" }
}

/// A person a memory is linked to, and how ("who", "to_whom", "mentioned").
struct MemoryPerson: Decodable, Equatable, Hashable, Identifiable {
    var id: Int64
    var name: String
    var role: String

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; name = c.lenient(.name) ?? ""; role = c.lenient(.role) ?? ""
    }
    enum CodingKeys: String, CodingKey { case id, name, role }
}

/// One memory row (sve_memories_json, sve_memory_get). `history` (the versions it replaced) is filled only by
/// sve_memory_get.
struct BrainMemory: Identifiable, Decodable, Equatable {
    var id: Int64
    /// "fact" | "decision" | "promise" | "question" | "person" | "idea"
    var type: String
    var text: String
    var who: String?
    var toWhom: String?
    var dueDate: String?
    var dueText: String?
    var saidDate: String?
    var checked: [CheckedField]
    /// The words it came from were heard with low confidence ("I may have heard this wrong").
    var lowConfidence: Bool
    /// "new" | "kept" | "corrected" | "wrong"
    var review: String
    /// Waiting in the review queue.
    var queued: Bool
    var health: Bool
    var done: Bool
    var importance: Double
    /// "recording" | "chat" | "medicine" | "migrated"
    var origin: String
    var medicineID: Int64?
    var suggestionID: Int64?
    var supersededBy: Int64?
    var conflictWith: Int64?
    var firstSeen: Int64?
    var lastSeen: Int64?
    var sources: [MemorySource]
    var people: [MemoryPerson]
    var history: [BrainMemory]

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; type = c.lenient(.type) ?? "fact"; text = c.lenient(.text) ?? ""
        who = c.lenient(.who); toWhom = c.lenient(.toWhom); dueDate = c.lenient(.dueDate); dueText = c.lenient(.dueText)
        saidDate = c.lenient(.saidDate); checked = c.lenient(.checked) ?? []
        lowConfidence = (c.lenient(.confidence) as String?) == "low"
        review = c.lenient(.review) ?? "new"; queued = c.flag(.queue) ?? false; health = c.flag(.health) ?? false
        done = c.flag(.done) ?? false; importance = c.lenient(.importance) ?? 0; origin = c.lenient(.origin) ?? "recording"
        medicineID = c.lenient(.medicineID); suggestionID = c.lenient(.suggestionID)
        supersededBy = c.lenient(.supersededBy); conflictWith = c.lenient(.conflictWith)
        firstSeen = c.lenient(.firstSeen); lastSeen = c.lenient(.lastSeen)
        sources = c.lenient(.sources) ?? []; people = c.lenient(.people) ?? []; history = c.lenient(.history) ?? []
    }
    enum CodingKeys: String, CodingKey {
        case id, type, text, who, checked, confidence, review, queue, health, done, importance, origin, sources, people, history
        case toWhom = "to_whom", dueDate = "due_date", dueText = "due_text", saidDate = "said_date"
        case medicineID = "medicine_id", suggestionID = "suggestion_id", supersededBy = "superseded_by"
        case conflictWith = "conflict_with", firstSeen = "first_seen", lastSeen = "last_seen"
    }
}

/// A memory waiting for the person (sve_review_queue_json): the row, plus the pending reminder or task card made
/// from the same words, if any (shown in the same card).
struct ReviewItem: Identifiable, Decodable, Equatable {
    var memory: BrainMemory
    var suggestion: Suggestion?
    var id: Int64 { memory.id }

    init(from decoder: Decoder) throws {
        memory = try BrainMemory(from: decoder)
        suggestion = try decoder.container(keyedBy: CodingKeys.self).lenient(.suggestion)
    }
    enum CodingKeys: String, CodingKey { case suggestion }

    /// The linked card is shown inside the memory's card (one card per thing) only while it is pending.
    var showsSuggestionRow: Bool { suggestion?.status == "pending" }

    /// Suggestions shown inside a queued memory's card, so the separate suggestion list leaves them out.
    static func suggestionIDsShown(in queue: [ReviewItem]) -> Set<Int64> {
        Set(queue.compactMap { $0.showsSuggestionRow ? $0.suggestion?.id : nil })
    }
}

/// Which memory rows to read (sve_memories_json). Every field left nil is not sent. Newest first; wrong and
/// superseded rows only when asked for.
struct MemoryFilter: Encodable, Equatable {
    var noteID: Int64?
    var types: [String]?
    var reviews: [String]?
    /// true: only those waiting in the review queue; false: only those not waiting.
    var queued: Bool?
    var includeWrong: Bool?
    var includeSuperseded: Bool?
    var limit: Int?

    init(noteID: Int64? = nil, types: [String]? = nil, reviews: [String]? = nil, queued: Bool? = nil,
         includeWrong: Bool? = nil, includeSuperseded: Bool? = nil, limit: Int? = nil) {
        self.noteID = noteID; self.types = types; self.reviews = reviews; self.queued = queued
        self.includeWrong = includeWrong; self.includeSuperseded = includeSuperseded; self.limit = limit
    }

    /// Every current memory.
    static let all = MemoryFilter()

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encodeIfPresent(noteID, forKey: .noteID); try c.encodeIfPresent(types, forKey: .types)
        try c.encodeIfPresent(reviews, forKey: .reviews); try c.encodeIfPresent(queued.map { $0 ? 1 : 0 }, forKey: .queue)
        try c.encodeIfPresent(includeWrong, forKey: .includeWrong)
        try c.encodeIfPresent(includeSuperseded, forKey: .includeSuperseded); try c.encodeIfPresent(limit, forKey: .limit)
    }
    enum CodingKeys: String, CodingKey {
        case types, reviews, queue, limit
        case noteID = "note_id", includeWrong = "include_wrong", includeSuperseded = "include_superseded"
    }
}
