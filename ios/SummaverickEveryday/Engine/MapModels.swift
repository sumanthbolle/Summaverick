// Typed models for the map (sve_brain_map_json, core Task 16). Foundation only; decoding is lenient, as in
// MemoryModels: a missing key or an unexpected type becomes nil/default instead of failing the whole map.
// Facts and ideas are not items: they count under their person or topic and are reached through its list.
import Foundation

/// One point on a branch: a person or topic ("t<id>") or a promise, decision or question ("m<id>").
struct MapItem: Decodable, Equatable, Hashable {
    var ref: String
    var label: String
    var importance: Double
    /// New since the person last looked (only when memories go straight to the map).
    var isNew: Bool
    /// The words were heard with low confidence.
    var lowConfidence: Bool
    var done: Bool
    /// People and topics: how many memories sit under them. 0 for a memory.
    var count: Int

    init(ref: String, label: String, importance: Double = 0, isNew: Bool = false, lowConfidence: Bool = false,
         done: Bool = false, count: Int = 0) {
        self.ref = ref; self.label = label; self.importance = importance; self.isNew = isNew
        self.lowConfidence = lowConfidence; self.done = done; self.count = count
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ref = c.lenient(.ref) ?? ""; label = c.lenient(.label) ?? ""; importance = c.lenient(.importance) ?? 0
        isNew = c.flag(.isNew) ?? false; lowConfidence = c.flag(.lowConfidence) ?? false
        done = c.flag(.done) ?? false; count = c.lenient(.count) ?? 0
    }
    enum CodingKeys: String, CodingKey {
        case ref, label, importance, done, count
        case isNew = "new", lowConfidence = "low_confidence"
    }

    /// A person or a topic (its memories open as a list).
    var isTopic: Bool { ref.hasPrefix("t") }
    /// The row id after the letter ("t10" -> 10, "m1" -> 1); nil when the ref is not one.
    var rowID: Int64? { Int64(ref.dropFirst()) }
}

/// One branch around the centre, in the core's order: people, topics, promises, decisions, questions.
struct MapBranch: Decodable, Equatable, Hashable {
    /// "people" | "topics" | "promises" | "decisions" | "questions"
    var kind: String
    var items: [MapItem]
    /// Items not shown on the map (the branch keeps its strongest); the "more" node lists them.
    var more: Int

    init(kind: String, items: [MapItem], more: Int = 0) { self.kind = kind; self.items = items; self.more = more }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = c.lenient(.kind) ?? ""; items = c.lenient(.items) ?? []; more = c.lenient(.more) ?? 0
    }
    enum CodingKeys: String, CodingKey { case kind, items, more }

    /// Everything the branch holds, shown or not.
    var total: Int { items.count + more }
}

/// Two shown memories that share a person, a topic or a meaning.
struct MapLink: Decodable, Equatable, Hashable {
    var a: String
    var b: String
    /// "person" | "topic" | "meaning"
    var kind: String

    init(a: String, b: String, kind: String) { self.a = a; self.b = b; self.kind = kind }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        a = c.lenient(.a) ?? ""; b = c.lenient(.b) ?? ""; kind = c.lenient(.kind) ?? ""
    }
    enum CodingKeys: String, CodingKey { case a, b, kind }
}

/// The whole map, or one recording's (center.kind "recording").
struct BrainMap: Decodable, Equatable {
    /// "you" | "recording"
    var centerKind: String
    /// The recording's id for a recording's map, else 0.
    var noteID: Int64
    /// Fewer than 8 points: a plain grouped list reads better than a map.
    var list: Bool
    var branches: [MapBranch]
    var links: [MapLink]

    init(centerKind: String = "you", noteID: Int64 = 0, list: Bool = false, branches: [MapBranch], links: [MapLink] = []) {
        self.centerKind = centerKind; self.noteID = noteID; self.list = list; self.branches = branches; self.links = links
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let center = try? c.nestedContainer(keyedBy: CenterKeys.self, forKey: .center)
        centerKind = center?.lenient(.kind) ?? "you"; noteID = center?.lenient(.noteID) ?? 0
        list = c.flag(.list) ?? false
        // A branch the app does not know (a later core) is left out rather than drawn without a colour.
        branches = ((c.lenient(.branches) as [MapBranch]?) ?? []).filter { BrainMap.kinds.contains($0.kind) && $0.total > 0 }
        links = c.lenient(.links) ?? []
    }
    enum CodingKeys: String, CodingKey { case center, list, branches, links }
    enum CenterKeys: String, CodingKey { case kind, noteID = "note_id" }

    static let kinds = ["people", "topics", "promises", "decisions", "questions"]

    /// The map from the core's JSON; nil when it could not be read.
    static func from(json: String?) -> BrainMap? {
        guard let json, let m = try? JSONDecoder().decode(BrainMap.self, from: Data(json.utf8)) else { return nil }
        return m
    }

    var isEmpty: Bool { branches.isEmpty }
    /// Branches that hold something (the core leaves out empty ones).
    var branchesWithItems: Int { branches.filter { !$0.items.isEmpty }.count }
}
