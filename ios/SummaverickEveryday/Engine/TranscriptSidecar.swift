// Transcription progress kept next to the recording, in "<audio>.live.json":
// {"cursor_sample":N,"pieces":[{"start_sec":N,"text":"…"}],"sections":[…],"parts":[…]}. Every transcription (live
// while recording, after Stop, after an interruption, for notes that waited for the AI) continues from the cursor,
// so an interruption never starts over. Section summaries written while recording are kept here too, until the note
// stores them, and so are those sections' notes and questions written while recording (parts, LiveParts). Written
// atomically after every piece, section or part; deleted once the transcript is saved to the note, and together
// with the audio. Foundation only, checked with a standalone swiftc script.
import Foundation

/// One section's summary, as the core writes it (sve_section_summarize):
/// {"title","topic","points":[{"text","start_sec"}],"terms":[],"hints":[]} and "fallback":true when no model
/// output could be used. Missing fields read as empty.
struct SectionSummary: Codable, Equatable {
    struct Point: Codable, Equatable {
        var text: String
        var start_sec: Int
    }
    var title: String = ""
    var topic: String = ""
    var points: [Point] = []
    var terms: [String] = []
    var hints: [String] = []
    var fallback: Bool?
}

extension SectionSummary.Point {
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        text = (try? c.decodeIfPresent(String.self, forKey: .text)) ?? ""
        start_sec = (try? c.decodeIfPresent(Int.self, forKey: .start_sec)) ?? 0
    }
}

extension SectionSummary {
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        title = (try? c.decodeIfPresent(String.self, forKey: .title)) ?? ""
        topic = (try? c.decodeIfPresent(String.self, forKey: .topic)) ?? ""
        points = (try? c.decodeIfPresent([Point].self, forKey: .points)) ?? []
        terms = (try? c.decodeIfPresent([String].self, forKey: .terms)) ?? []
        hints = (try? c.decodeIfPresent([String].self, forKey: .hints)) ?? []
        fallback = try? c.decodeIfPresent(Bool.self, forKey: .fallback)
    }
}

/// About 5 minutes of a recording: pieces firstPiece...lastPiece (both included, indices into the note's
/// pieces), as sve_note_set_sections stores it.
struct NoteSection: Codable, Equatable {
    var idx: Int
    var firstPiece: Int
    var lastPiece: Int
    var startSec: Int
    var endSec: Int
    var summary: SectionSummary

    enum CodingKeys: String, CodingKey {
        case idx, summary
        case firstPiece = "first_piece", lastPiece = "last_piece", startSec = "start_sec", endSec = "end_sec"
    }

    /// The leading sections that still fit `pieceCount` pieces: numbered 0, 1, 2…, each starting right after
    /// the one before (the first at piece 0) and ending inside the pieces. Anything after the first that does
    /// not fit is dropped, so it is written again.
    static func validPrefix(_ sections: [NoteSection], pieceCount: Int) -> [NoteSection] {
        var out: [NoteSection] = []
        for s in sections {
            let first = out.last.map { $0.lastPiece + 1 } ?? 0
            guard s.idx == out.count, s.firstPiece == first, s.lastPiece >= s.firstPiece, s.lastPiece < pieceCount else { break }
            out.append(s)
        }
        return out
    }

    /// Pieces already covered by these sections (they come from validPrefix).
    static func covered(_ sections: [NoteSection]) -> Int { sections.last.map { $0.lastPiece + 1 } ?? 0 }
}

/// While recording, transcription comes first: sections are written only when fewer than one section's worth
/// of pieces (10 pieces of 28 s) is still waiting to be transcribed.
enum LiveSections {
    static let pieceSeconds: Double = 28
    static let maxPiecesWaiting = 10

    static func piecesWaiting(recordedSeconds: Double, transcribedSeconds: Double) -> Int {
        let behind = recordedSeconds - transcribedSeconds
        guard behind > 0, behind.isFinite else { return 0 }
        return Int(behind / pieceSeconds)
    }

    static func shouldWrite(recordedSeconds: Double, transcribedSeconds: Double) -> Bool {
        piecesWaiting(recordedSeconds: recordedSeconds, transcribedSeconds: transcribedSeconds) < maxPiecesWaiting
    }
}

/// One section's notes (kind 1) or questions (kind 2) written while recording (sve_section_output_text), for the
/// section's pieces first...last (both included) in lang. Put into the note at Stop (sve_section_output_put) when
/// the note's section idx still has that range.
struct LivePart: Codable, Equatable {
    var idx: Int
    var kind: Int
    var lang: String
    var first: Int
    var last: Int
    var text: String
    var cut: Bool

    static let notes = 1, questions = 2
}

struct TranscriptSidecar: Codable, Equatable {
    struct Piece: Codable, Equatable {
        var start_sec: Int
        var text: String
        /// Mean token probability of the piece (0...1); absent in sidecars from before and when unknown.
        var conf: Double?
        /// Set while Gemma has the piece's audio and the text is still to come (Whisper wrote none); absent otherwise.
        var pending: Bool?
        /// Sample (16 kHz) the piece's audio ends at, kept with a pending piece.
        var end_sample: Int64?
        /// Sample (16 kHz) the piece's audio starts at, kept with a pending piece.
        var start_sample: Int64?
    }

    /// Next sample (16 kHz) to transcribe from.
    var cursor_sample: Int64 = 0
    var pieces: [Piece] = []
    /// Section summaries written while recording, over `pieces` (validPrefix before use).
    var sections: [NoteSection] = []
    /// Notes and questions of `sections` written while recording.
    var parts: [LivePart] = []

    static let sampleRate: Double = 16_000

    var transcribedSeconds: Double { Double(cursor_sample) / Self.sampleRate }
    /// Context for the next piece: the text of the last piece that had any.
    var previousText: String? { pieces.last(where: { !$0.text.isEmpty && $0.pending != true })?.text }
    /// Indices of the pieces whose text is still to come.
    var pendingIndices: [Int] { pieces.indices.filter { pieces[$0].pending == true } }

    /// Adds a finished piece and moves the cursor. Empty text (silence) only moves the cursor.
    /// A piece that does not move the cursor forward is ignored.
    mutating func append(startSec: Int, endSample: Int64, text: String, conf: Double? = nil) {
        guard endSample > cursor_sample else { return }
        cursor_sample = endSample
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty { pieces.append(Piece(start_sec: startSec, text: t, conf: conf, pending: nil, end_sample: nil, start_sample: nil)) }
    }

    /// Adds an empty piece marked pending, for text to be filled in later, and moves the cursor.
    mutating func appendPending(startSec: Int, endSample: Int64) {
        guard endSample > cursor_sample else { return }
        let start = cursor_sample
        cursor_sample = endSample
        pieces.append(Piece(start_sec: startSec, text: "", conf: nil, pending: true, end_sample: endSample, start_sample: start))
    }

    /// Sets a pending piece's text and clears the mark. Empty text removes the piece, which shifts the indices of
    /// the later pieces: fill several in descending index order, or re-read pendingIndices after each fill (as
    /// TranscriptRun.fillPending does). A piece that is not pending is left alone.
    mutating func fill(_ index: Int, text: String, conf: Double?) {
        guard pieces.indices.contains(index), pieces[index].pending == true else { return }
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.isEmpty {
            pieces.remove(at: index)
        } else {
            pieces[index].text = t
            pieces[index].conf = conf
            pieces[index].pending = nil
            pieces[index].end_sample = nil
            pieces[index].start_sample = nil
        }
    }

    static func path(forAudio audioPath: String) -> String { audioPath + ".live.json" }

    /// The saved progress, or a fresh start when there is none (or it cannot be read).
    static func load(audioPath: String) -> TranscriptSidecar {
        guard let data = FileManager.default.contents(atPath: path(forAudio: audioPath)),
              let s = try? JSONDecoder().decode(TranscriptSidecar.self, from: data), s.cursor_sample >= 0 else {
            return TranscriptSidecar()
        }
        return s
    }

    /// Atomic write. Not written when the audio is gone (deleted while a piece was in flight).
    @discardableResult
    func save(audioPath: String) -> Bool {
        guard FileManager.default.fileExists(atPath: audioPath), let data = try? JSONEncoder().encode(self) else { return false }
        do {
            try data.write(to: URL(fileURLWithPath: Self.path(forAudio: audioPath)), options: .atomic)
            return true
        } catch {
            return false
        }
    }

    static func remove(audioPath: String) {
        try? FileManager.default.removeItem(atPath: path(forAudio: audioPath))
    }
}

extension TranscriptSidecar {
    /// Sidecars written before sections existed have no "sections".
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        cursor_sample = try c.decode(Int64.self, forKey: .cursor_sample)
        pieces = try c.decode([Piece].self, forKey: .pieces)
        sections = (try? c.decodeIfPresent([NoteSection].self, forKey: .sections)) ?? []
        parts = (try? c.decodeIfPresent([LivePart].self, forKey: .parts)) ?? []
    }

    /// The parts written for `section` as it is now in lang.
    func part(_ section: NoteSection, kind: Int, lang: String) -> LivePart? {
        parts.first { $0.idx == section.idx && $0.kind == kind && $0.lang == lang
            && $0.first == section.firstPiece && $0.last == section.lastPiece }
    }

    /// The oldest section (of the valid ones) with a kind still to write in lang, and those kinds (notes first), or
    /// nil when every section has both. `skip`: sections left for after Stop.
    func partsDue(lang: String, skip: Set<Int> = []) -> (section: NoteSection, kinds: [Int])? {
        for s in NoteSection.validPrefix(sections, pieceCount: pieces.count) where !skip.contains(s.idx) {
            let kinds = [LivePart.notes, LivePart.questions].filter { part(s, kind: $0, lang: lang) == nil }
            if !kinds.isEmpty { return (s, kinds) }
        }
        return nil
    }

    /// Keeps a part, in place of an older one of the same section and kind.
    mutating func keep(_ p: LivePart) {
        parts.removeAll { $0.idx == p.idx && $0.kind == p.kind }
        parts.append(p)
    }
}

// MARK: - lenient decoding
// Shared by the models the core's JSON decodes into (AssistantModels, MemoryModels). Kept here, with JSONValue,
// so the Foundation-only swiftc checks need no other file.

extension KeyedDecodingContainer {
    /// nil when the key is missing, null, or of an unexpected type.
    func lenient<T: Decodable>(_ key: Key) -> T? { try? decode(T.self, forKey: key) }
    /// The core stores booleans as 0/1 integers in some places and true/false in others.
    func flag(_ key: Key) -> Bool? {
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
