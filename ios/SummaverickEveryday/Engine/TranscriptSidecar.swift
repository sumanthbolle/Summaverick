// Transcription progress kept next to the recording, in "<audio>.live.json":
// {"cursor_sample":N,"pieces":[{"start_sec":N,"text":"…"}],"sections":[…]}. Every transcription (live while
// recording, after Stop, after an interruption, for notes that waited for the AI) continues from the cursor, so an
// interruption never starts over. Section summaries written while recording are kept here too, until the note
// stores them. Written atomically after every piece or section; deleted once the transcript is saved to the note,
// and together with the audio. Foundation only, checked with a standalone swiftc script.
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

struct TranscriptSidecar: Codable, Equatable {
    struct Piece: Codable, Equatable {
        var start_sec: Int
        var text: String
        /// Mean token probability of the piece (0...1); absent in sidecars from before and when unknown.
        var conf: Double?
    }

    /// Next sample (16 kHz) to transcribe from.
    var cursor_sample: Int64 = 0
    var pieces: [Piece] = []
    /// Section summaries written while recording, over `pieces` (validPrefix before use).
    var sections: [NoteSection] = []

    static let sampleRate: Double = 16_000

    var transcribedSeconds: Double { Double(cursor_sample) / Self.sampleRate }
    /// Context for the next piece: the text of the last piece that had any.
    var previousText: String? { pieces.last(where: { !$0.text.isEmpty })?.text }

    /// Adds a finished piece and moves the cursor. Empty text (silence) only moves the cursor.
    /// A piece that does not move the cursor forward is ignored.
    mutating func append(startSec: Int, endSample: Int64, text: String, conf: Double? = nil) {
        guard endSample > cursor_sample else { return }
        cursor_sample = endSample
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty { pieces.append(Piece(start_sec: startSec, text: t, conf: conf)) }
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
    }
}
