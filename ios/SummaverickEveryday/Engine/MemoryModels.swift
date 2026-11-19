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

/// What the person tapped on a memory's card in the review queue (Edit is not here: it stays separate).
enum ReviewTap: Equatable {
    case keep, wrong
    /// Yes on the linked reminder or to-do row; edited: saved from its Edit form.
    case suggestionYes(edited: Bool)
    /// No on the linked reminder or to-do row.
    case suggestionNo
    /// Delete on a memory's card (MemoryCardView), for a memory waiting in the queue.
    case delete
}

/// How the linked suggestion's step went.
enum SuggestionStepResult: Equatable { case done, stale, failed }

/// What running a LinkedDecision did.
struct LinkedRunResult: Equatable {
    /// A step failed: the card stays and shows the inline message.
    var failed = false
    /// The views must read again: something changed, or the accept came back re-flagged (its new check fields
    /// must reach the card before the next Yes, so a flagged card asks "Save anyway?" again).
    var needsReload = false
}

/// The changes one tap makes, run in this order: the linked suggestion first, then the memory. When the
/// suggestion's step fails (or comes back re-flagged), the memory is not changed and the card stays.
struct LinkedDecision: Equatable {
    enum SuggestionStep: Equatable { case accept, reject }
    var suggestion: SuggestionStep?
    /// "kept" | "wrong" | nil (unchanged)
    var memoryReview: String?
    /// Delete the memory (after the suggestion's step).
    var removesMemory = false

    /// Runs the steps in order with the given calls: the suggestion, then the memory's review, then its removal.
    /// A failed step stops there; a re-flagged accept stops without a failure and asks for a reload.
    func perform(suggestionStep: (SuggestionStep) async -> SuggestionStepResult,
                 memoryStep: (String) async -> Bool,
                 removeStep: () async -> Bool) async -> LinkedRunResult {
        var r = LinkedRunResult()
        if let suggestion {
            switch await suggestionStep(suggestion) {
            case .done: r.needsReload = true
            case .stale: r.needsReload = true; return r
            case .failed: r.failed = true; return r
            }
        }
        if let memoryReview {
            if await memoryStep(memoryReview) { r.needsReload = true } else { r.failed = true; return r }
        }
        if removesMemory {
            if await removeStep() { r.needsReload = true } else { r.failed = true }
        }
        return r
    }
}

extension ReviewItem {
    /// One card, one decision (K45): Yes on the linked row also keeps the memory, as Keep would; Wrong on the
    /// memory also declines its pending linked suggestion, so it does not come back on its own. Keep and No touch
    /// only their own half, and a Yes saved from the Edit form is the person's own reminder: the memory still
    /// waits to be checked. Medicine memories (the core refuses their review) are never changed from here.
    func linkedDecision(_ tap: ReviewTap) -> LinkedDecision {
        let linked = showsSuggestionRow
        let reviewable = memory.origin != "medicine"
        switch tap {
        case .keep:
            return LinkedDecision(suggestion: nil, memoryReview: reviewable ? "kept" : nil)
        case .wrong:
            return LinkedDecision(suggestion: linked ? .reject : nil, memoryReview: reviewable ? "wrong" : nil)
        case .suggestionYes(let edited):
            guard linked else { return LinkedDecision() }
            return LinkedDecision(suggestion: .accept, memoryReview: reviewable && !edited ? "kept" : nil)
        case .suggestionNo:
            return LinkedDecision(suggestion: linked ? .reject : nil, memoryReview: nil)
        case .delete:
            return LinkedDecision(suggestion: linked ? .reject : nil, memoryReview: nil, removesMemory: reviewable)
        }
    }

    /// The review queue from the core's JSON; nil when it could not be read (not the same as an empty queue).
    /// Medicine memories are left out: the core never queues them (they follow the saved medicine and refuse
    /// review), so one here would have no way out.
    static func queue(fromJSON json: String?) -> [ReviewItem]? {
        guard let json, let items = try? JSONDecoder().decode([ReviewItem].self, from: Data(json.utf8)) else { return nil }
        return items.filter { $0.memory.origin != "medicine" }
    }
}

/// One memory read by id: there, gone (deleted, "null"), or not readable (a read error, which is not "gone").
enum MemoryLookup: Equatable {
    case found(BrainMemory)
    case gone
    case failed

    init(json: String?) {
        guard let json else { self = .failed; return }
        if json.trimmingCharacters(in: .whitespacesAndNewlines) == "null" { self = .gone; return }
        guard let m = try? JSONDecoder().decode(BrainMemory.self, from: Data(json.utf8)) else { self = .failed; return }
        self = .found(m)
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

// MARK: - relations review (sve_relation_reviews_json)

/// A person a recording names, as one line of its relations review.
struct RelationPerson: Decodable, Equatable, Hashable, Identifiable {
    /// The relation id (what an edit names).
    var id: Int64
    var topicID: Int64
    var name: String

    init(id: Int64, topicID: Int64 = 0, name: String) { self.id = id; self.topicID = topicID; self.name = name }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; topicID = c.lenient(.topicID) ?? 0; name = c.lenient(.name) ?? ""
    }
    enum CodingKeys: String, CodingKey { case id, name, topicID = "topic_id" }
}

/// A decision and who made it, or an open question and who asked it.
struct RelationLine: Decodable, Equatable, Hashable, Identifiable {
    var id: Int64
    var memoryID: Int64
    var text: String
    /// The person's name; "" when it was the phone's owner ("me") or nobody was named.
    var who: String
    /// "person" | "me" | "nobody"
    var whoKind: String
    var quote: String?
    var startSec: Int?

    init(id: Int64, memoryID: Int64 = 0, text: String, who: String = "", whoKind: String = "nobody",
         quote: String? = nil, startSec: Int? = nil) {
        self.id = id; self.memoryID = memoryID; self.text = text; self.who = who; self.whoKind = whoKind
        self.quote = quote; self.startSec = startSec
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; memoryID = c.lenient(.memoryID) ?? 0; text = c.lenient(.text) ?? ""
        who = c.lenient(.who) ?? ""; whoKind = c.lenient(.whoKind) ?? "nobody"
        quote = c.lenient(.quote); startSec = c.lenient(.startSec)
    }
    enum CodingKeys: String, CodingKey {
        case id, text, who, quote
        case memoryID = "memory_id", whoKind = "who_kind", startSec = "start_sec"
    }
}

/// One recording's "who's involved, what was decided, what's still open", waiting for the person to check.
struct RelationReview: Decodable, Equatable, Identifiable {
    var noteID: Int64
    var title: String
    var people: [RelationPerson]
    var decisions: [RelationLine]
    var questions: [RelationLine]
    var id: Int64 { noteID }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        noteID = c.lenient(.noteID) ?? 0; title = c.lenient(.title) ?? ""
        people = c.lenient(.people) ?? []; decisions = c.lenient(.decisions) ?? []; questions = c.lenient(.questions) ?? []
    }
    enum CodingKeys: String, CodingKey { case title, people, decisions, questions, noteID = "note_id" }
}

/// The person's correction to one line, as sve_relations_accept takes it. Only what changed is sent.
struct RelationEdit: Encodable, Equatable {
    var id: Int64
    var removed: Bool? = nil
    /// A person line: the right name.
    var name: String? = nil
    /// A decision or question line: the right words, and who ("me" for the person themselves, "" for nobody).
    var text: String? = nil
    var who: String? = nil
}
