// Typed models for the assistant half of the core API (sve_core.h, "personal assistant" section).
// The core's JSON uses snake_case keys, so every type has explicit CodingKeys. Decoding is lenient:
// a missing key, a null or an unexpected type becomes nil/default instead of failing the whole list.
import Foundation

extension KeyedDecodingContainer {
    /// nil when the key is missing, null, or of an unexpected type.
    fileprivate func lenient<T: Decodable>(_ key: Key) -> T? { try? decode(T.self, forKey: key) }
    /// The core stores booleans as 0/1 integers in some places and true/false in others.
    fileprivate func flag(_ key: Key) -> Bool? {
        if let b: Bool = lenient(key) { return b }
        if let i: Int = lenient(key) { return i != 0 }
        return nil
    }
}

// MARK: - raw JSON

/// Any JSON value. Used to keep a suggestion payload's raw shape so edits round-trip untouched.
enum JSONValue: Codable, Equatable {
    case null, bool(Bool), number(Double), string(String), array([JSONValue]), object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let n = try? c.decode(Double.self) { self = .number(n) }
        else if let s = try? c.decode(String.self) { self = .string(s) }
        else if let a = try? c.decode([JSONValue].self) { self = .array(a) }
        else if let o = try? c.decode([String: JSONValue].self) { self = .object(o) }
        else { self = .null }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .null: try c.encodeNil()
        case .bool(let b): try c.encode(b)
        case .number(let n): try c.encode(n)
        case .string(let s): try c.encode(s)
        case .array(let a): try c.encode(a)
        case .object(let o): try c.encode(o)
        }
    }

    var stringValue: String? { if case .string(let s) = self { return s }; return nil }
}

// MARK: - profile, routine, medicines

struct Routine: Codable, Equatable {
    var wake: String?
    var breakfast: String?
    var lunch: String?
    var dinner: String?
    var bed: String?

    init(wake: String? = nil, breakfast: String? = nil, lunch: String? = nil, dinner: String? = nil, bed: String? = nil) {
        self.wake = wake; self.breakfast = breakfast; self.lunch = lunch; self.dinner = dinner; self.bed = bed
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        wake = c.lenient(.wake); breakfast = c.lenient(.breakfast); lunch = c.lenient(.lunch)
        dinner = c.lenient(.dinner); bed = c.lenient(.bed)
    }
    enum CodingKeys: String, CodingKey { case wake, breakfast, lunch, dinner, bed }
}

struct Profile: Codable, Equatable {
    var name: String?
    var lang: String?
    var style: String?
    var routine: Routine?

    init(name: String? = nil, lang: String? = nil, style: String? = nil, routine: Routine? = nil) {
        self.name = name; self.lang = lang; self.style = style; self.routine = routine
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = c.lenient(.name); lang = c.lenient(.lang); style = c.lenient(.style); routine = c.lenient(.routine)
    }
    enum CodingKeys: String, CodingKey { case name, lang, style, routine }
}

struct Medicine: Identifiable, Codable, Equatable {
    var id: Int64
    var name: String
    var dose: String?
    var whenText: String?
    var times: [String]
    var `repeat`: String?
    var untilDate: String?
    var createdAt: Int64?

    /// id 0 = a new medicine when passed to saveMedicine.
    init(id: Int64 = 0, name: String, dose: String? = nil, whenText: String? = nil, times: [String] = [],
         repeat: String? = nil, untilDate: String? = nil, createdAt: Int64? = nil) {
        self.id = id; self.name = name; self.dose = dose; self.whenText = whenText; self.times = times
        self.repeat = `repeat`; self.untilDate = untilDate; self.createdAt = createdAt
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; name = c.lenient(.name) ?? ""; dose = c.lenient(.dose)
        whenText = c.lenient(.whenText); times = c.lenient(.times) ?? []; `repeat` = c.lenient(.repeat)
        untilDate = c.lenient(.untilDate); createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey {
        case id, name, dose, times, `repeat`
        case whenText = "when_text", untilDate = "until_date", createdAt = "created_at"
    }
}

// MARK: - reminders and tasks

struct Reminder: Identifiable, Codable, Equatable {
    var id: Int64
    var text: String
    var times: [String]
    var `repeat`: String?
    var startDate: String?
    var untilDate: String?
    var noteID: Int64?
    var startSec: Int?
    var medicineID: Int64?
    var fromDeleted: Bool
    var active: Bool
    var createdAt: Int64?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; text = c.lenient(.text) ?? ""; times = c.lenient(.times) ?? []
        `repeat` = c.lenient(.repeat); startDate = c.lenient(.startDate); untilDate = c.lenient(.untilDate)
        noteID = c.lenient(.noteID); startSec = c.lenient(.startSec); medicineID = c.lenient(.medicineID)
        fromDeleted = c.flag(.fromDeleted) ?? false; active = c.flag(.active) ?? true; createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey {
        case id, text, times, `repeat`, active
        case startDate = "start_date", untilDate = "until_date", noteID = "note_id", startSec = "start_sec"
        case medicineID = "medicine_id", fromDeleted = "from_deleted", createdAt = "created_at"
    }
}

struct TaskItem: Identifiable, Codable, Equatable {
    var id: Int64
    var text: String
    var who: String?
    var dueDate: String?
    var done: Bool
    var noteID: Int64?
    var startSec: Int?
    var fromDeleted: Bool
    var createdAt: Int64?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; text = c.lenient(.text) ?? ""; who = c.lenient(.who); dueDate = c.lenient(.dueDate)
        done = c.flag(.done) ?? false; noteID = c.lenient(.noteID); startSec = c.lenient(.startSec)
        fromDeleted = c.flag(.fromDeleted) ?? false; createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey {
        case id, text, who, done
        case dueDate = "due_date", noteID = "note_id", startSec = "start_sec"
        case fromDeleted = "from_deleted", createdAt = "created_at"
    }
}

// MARK: - topics

struct Topic: Identifiable, Codable, Equatable {
    var id: Int64
    var kind: String          // "topic" | "person"
    var name: String
    var summary: String?
    var summaryStale: Int     // 0/1 in the core
    var noteCount: Int
    var openTasks: Int
    var lastTitle: String?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; kind = c.lenient(.kind) ?? "topic"; name = c.lenient(.name) ?? ""
        summary = c.lenient(.summary); summaryStale = c.lenient(.summaryStale) ?? 0
        noteCount = c.lenient(.noteCount) ?? 0; openTasks = c.lenient(.openTasks) ?? 0; lastTitle = c.lenient(.lastTitle)
    }
    enum CodingKeys: String, CodingKey {
        case id, kind, name, summary
        case summaryStale = "summary_stale", noteCount = "note_count", openTasks = "open_tasks", lastTitle = "last_title"
    }
}

/// One note inside a topic (sve_topic_notes_json).
struct TopicNote: Identifiable, Codable, Equatable {
    var id: Int64
    var title: String
    var createdAt: Int64?
    var shortSummary: String?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; title = c.lenient(.title) ?? ""; createdAt = c.lenient(.createdAt)
        shortSummary = c.lenient(.shortSummary)
    }
    enum CodingKeys: String, CodingKey { case id, title, createdAt = "created_at", shortSummary = "short_summary" }
}

struct KeyTerm: Codable, Equatable, Hashable {
    var term: String
    var meaning: String
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        term = c.lenient(.term) ?? ""; meaning = c.lenient(.meaning) ?? ""
    }
    enum CodingKeys: String, CodingKey { case term, meaning }
}

struct QuizItem: Codable, Equatable, Hashable {
    var q: String
    var a: String
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        q = c.lenient(.q) ?? ""; a = c.lenient(.a) ?? ""
    }
    enum CodingKeys: String, CodingKey { case q, a }
}

struct TopicSummary: Codable, Equatable {
    var summary: String
    var keyTerms: [KeyTerm]
    var quiz: [QuizItem]
    var lang: String?
    /// Where the core's verbatim medicine quotes (appended to `summary`) came from; empty otherwise.
    var sources: [Source]

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        summary = c.lenient(.summary) ?? ""; keyTerms = c.lenient(.keyTerms) ?? []
        quiz = c.lenient(.quiz) ?? []; lang = c.lenient(.lang); sources = c.lenient(.sources) ?? []
    }
    enum CodingKeys: String, CodingKey { case summary, quiz, lang, sources, keyTerms = "key_terms" }
}

// MARK: - memory, insight

struct MemoryItem: Identifiable, Codable, Equatable {
    var id: Int64
    var kind: String          // "fact" | "preference" | "vocab"
    var text: String
    var noteID: Int64?
    var createdAt: Int64?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; kind = c.lenient(.kind) ?? "fact"; text = c.lenient(.text) ?? ""
        noteID = c.lenient(.noteID); createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey { case id, kind, text, noteID = "note_id", createdAt = "created_at" }
}

struct Insight: Identifiable, Codable, Equatable {
    var id: Int64
    var text: String
    var noteID: Int64?
    var dismissed: Bool
    var createdAt: Int64?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; text = c.lenient(.text) ?? ""; noteID = c.lenient(.noteID)
        dismissed = c.flag(.dismissed) ?? false; createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey { case id, text, dismissed, noteID = "note_id", createdAt = "created_at" }
}

// MARK: - ask

struct Source: Codable, Equatable, Hashable, Identifiable {
    var noteID: Int64
    var startSec: Int
    var title: String
    var id: String { "\(noteID)@\(startSec)" }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        noteID = c.lenient(.noteID) ?? 0; startSec = c.lenient(.startSec) ?? 0; title = c.lenient(.title) ?? ""
    }
    enum CodingKeys: String, CodingKey { case noteID = "note_id", startSec = "start_sec", title }
}

struct AskAnswer: Codable, Equatable {
    /// May hold text even when notFound is true (e.g. the saved-medicines list with an explanation).
    var answer: String
    var sources: [Source]
    var notFound: Bool
    var medicine: Bool

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        answer = c.lenient(.answer) ?? ""; sources = c.lenient(.sources) ?? []
        notFound = c.flag(.notFound) ?? false; medicine = c.flag(.medicine) ?? false
    }
    enum CodingKeys: String, CodingKey { case answer, sources, medicine, notFound = "not_found" }
}

// MARK: - transcript pieces

struct TranscriptPiece: Codable, Equatable, Hashable {
    var startSec: Int
    var text: String
    /// Present only when read back from the store.
    var idx: Int?

    init(startSec: Int, text: String, idx: Int? = nil) { self.startSec = startSec; self.text = text; self.idx = idx }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        startSec = c.lenient(.startSec) ?? 0; text = c.lenient(.text) ?? ""; idx = c.lenient(.idx)
    }
    func encode(to encoder: Encoder) throws {   // idx is never sent back to the core
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(startSec, forKey: .startSec); try c.encode(text, forKey: .text)
    }
    enum CodingKeys: String, CodingKey { case idx, text, startSec = "start_sec" }
}

// MARK: - suggestions

enum SuggestionKind: String, Codable {
    case reminder, task, profile, vocab, fact
    case unknown

    init(from decoder: Decoder) throws {
        let s = try decoder.singleValueContainer().decode(String.self)
        self = SuggestionKind(rawValue: s) ?? .unknown
    }
}

/// The medicine object inside reminder and profile payloads.
struct PayloadMedicine: Codable, Equatable {
    var name: String?
    var dose: String?
    var whenText: String?
    var frequency: String?
    var duration: String?
    init(name: String? = nil, dose: String? = nil, whenText: String? = nil, frequency: String? = nil, duration: String? = nil) {
        self.name = name; self.dose = dose; self.whenText = whenText; self.frequency = frequency; self.duration = duration
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = c.lenient(.name); dose = c.lenient(.dose); whenText = c.lenient(.whenText)
        frequency = c.lenient(.frequency); duration = c.lenient(.duration)
    }
    enum CodingKeys: String, CodingKey { case name, dose, frequency, duration, whenText = "when_text" }
}

/// Payload of a suggestion. Shapes by kind:
///  - reminder: text, times, repeat, startDate, untilDate, medicine
///  - task: text, who, dueDate
///  - profile: change ("add_medicine" | "update_medicine" | "stop_medicine" | "routine" ...), text, whenText, medicine, routine
///  - vocab: text
///  - fact: memoryKind, text
/// `raw` keeps the full JSON object as the core sent it. The typed accessors read from and write to `raw`,
/// so unknown keys survive an edit and the edited payload can be sent back to `accept`.
struct SuggestionPayload: Codable, Equatable {
    var raw: [String: JSONValue]

    init(raw: [String: JSONValue] = [:]) { self.raw = raw }
    init(from decoder: Decoder) throws {
        if case .object(let o)? = try? JSONValue(from: decoder) { raw = o } else { raw = [:] }
    }
    func encode(to encoder: Encoder) throws { try JSONValue.object(raw).encode(to: encoder) }

    private func string(_ k: String) -> String? { raw[k]?.stringValue }
    private mutating func setString(_ k: String, _ v: String?) { raw[k] = v.map { .string($0) } ?? .null }
    private func strings(_ k: String) -> [String]? {
        if case .array(let a)? = raw[k] { return a.compactMap { $0.stringValue } }
        return nil
    }

    var text: String? { get { string("text") } set { setString("text", newValue) } }
    var who: String? { get { string("who") } set { setString("who", newValue) } }
    var dueDate: String? { get { string("due_date") } set { setString("due_date", newValue) } }
    var startDate: String? { get { string("start_date") } set { setString("start_date", newValue) } }
    var untilDate: String? { get { string("until_date") } set { setString("until_date", newValue) } }
    var `repeat`: String? { get { string("repeat") } set { setString("repeat", newValue) } }
    var change: String? { get { string("change") } set { setString("change", newValue) } }
    var whenText: String? { get { string("when_text") } set { setString("when_text", newValue) } }
    var memoryKind: String? { get { string("memory_kind") } set { setString("memory_kind", newValue) } }
    var times: [String]? {
        get { strings("times") }
        set { raw["times"] = newValue.map { .array($0.map { .string($0) }) } ?? .null }
    }
    var routine: Routine? {
        get { Self.convert(raw["routine"]) }
        set { raw["routine"] = Self.convert(newValue) ?? .null }
    }
    var medicine: PayloadMedicine? {
        get { Self.convert(raw["medicine"]) }
        set { raw["medicine"] = Self.convert(newValue) ?? .null }
    }

    private static func convert<T: Decodable>(_ v: JSONValue?) -> T? {
        guard let v, v != .null, let d = try? JSONEncoder().encode(v) else { return nil }
        return try? JSONDecoder().decode(T.self, from: d)
    }
    private static func convert<T: Encodable>(_ v: T?) -> JSONValue? {
        guard let v, let d = try? JSONEncoder().encode(v) else { return nil }
        return try? JSONDecoder().decode(JSONValue.self, from: d)
    }
}

struct Suggestion: Identifiable, Codable, Equatable {
    var id: Int64
    var kind: SuggestionKind
    var status: String?
    var payload: SuggestionPayload
    var quote: String?
    var noteID: Int64?
    var startSec: Int?
    var medicineID: Int64?
    var checkStatus: String?       // "ok" | "check" | nil
    var checkFields: [String]
    var createdAt: Int64?

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenient(.id) ?? 0; kind = c.lenient(.kind) ?? .unknown; status = c.lenient(.status)
        payload = c.lenient(.payload) ?? SuggestionPayload(); quote = c.lenient(.quote)
        noteID = c.lenient(.noteID); startSec = c.lenient(.startSec); medicineID = c.lenient(.medicineID)
        checkStatus = c.lenient(.checkStatus); checkFields = c.lenient(.checkFields) ?? []; createdAt = c.lenient(.createdAt)
    }
    enum CodingKeys: String, CodingKey {
        case id, kind, status, payload, quote
        case noteID = "note_id", startSec = "start_sec", medicineID = "medicine_id"
        case checkStatus = "check_status", checkFields = "check_fields", createdAt = "created_at"
    }
}

/// Result of accepting a suggestion (sve_suggestion_accept).
enum AcceptResult: Equatable {
    case created(Int64)       // > 0: id of the new row (or the deleted medicine's id for stop_medicine)
    case applied              // 0: applied, no row (e.g. a routine change or nothing matched)
    case staleReflagged       // -2: the card went stale and was re-flagged; refresh and ask again
}

extension Notification.Name {
    static let assistantChanged = Notification.Name("sve.assistantChanged")
}
