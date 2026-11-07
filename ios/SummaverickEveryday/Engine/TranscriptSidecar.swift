// Transcription progress kept next to the recording, in "<audio>.live.json":
// {"cursor_sample":N,"pieces":[{"start_sec":N,"text":"…"}]}. Every transcription (live while recording,
// after Stop, after an interruption, for notes that waited for the AI) continues from the cursor, so an
// interruption never starts over. Written atomically after every piece; deleted once the transcript is
// saved to the note, and together with the audio. Foundation only, checked with a standalone swiftc script.
import Foundation

struct TranscriptSidecar: Codable, Equatable {
    struct Piece: Codable, Equatable {
        var start_sec: Int
        var text: String
    }

    /// Next sample (16 kHz) to transcribe from.
    var cursor_sample: Int64 = 0
    var pieces: [Piece] = []

    static let sampleRate: Double = 16_000

    var transcribedSeconds: Double { Double(cursor_sample) / Self.sampleRate }
    /// Context for the next piece: the text of the last piece that had any.
    var previousText: String? { pieces.last(where: { !$0.text.isEmpty })?.text }

    /// Adds a finished piece and moves the cursor. Empty text (silence) only moves the cursor.
    /// A piece that does not move the cursor forward is ignored.
    mutating func append(startSec: Int, endSample: Int64, text: String) {
        guard endSample > cursor_sample else { return }
        cursor_sample = endSample
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !t.isEmpty { pieces.append(Piece(start_sec: startSec, text: t)) }
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
