// Writes section summaries (~5 minutes of transcript each) as the pieces come in: live while recording (into
// the sidecar), after Stop and for older notes (into the note). The core plans the sections
// (sve_plan_sections); this only orchestrates: plan what is new, then summarise one section at a time, each
// after the caller's heat gate, keeping every finished section before the next starts.
import Foundation

@MainActor
enum SectionWriter {
    /// Half-open piece ranges [first, last) the core planner makes of `pieces`. The last piece ends at
    /// `endSec` (live: the cursor; after Stop: the note duration). Not final: only closed ranges.
    static func plan(pieces: [TranscriptPiece], endSec: Int64, isFinal: Bool) -> [Range<Int>] {
        guard !pieces.isEmpty, let data = try? JSONEncoder().encode(pieces),
              let json = String(data: data, encoding: .utf8),
              let out = sve_plan_sections(json, endSec, isFinal ? 1 : 0) else { return [] }
        defer { sve_free_string(out) }
        guard let ranges = try? JSONDecoder().decode([[Int]].self, from: Data(String(cString: out).utf8)) else { return [] }
        var next = 0
        var result: [Range<Int>] = []
        for r in ranges {   // contiguous from 0 and inside the pieces, else stop there
            guard r.count == 2, r[0] == next, r[1] > r[0], r[1] <= pieces.count else { break }
            result.append(r[0]..<r[1])
            next = r[1]
        }
        return result
    }

    /// Sections for `pieces`: `existing` (its valid prefix) plus up to `limit` new ones, planned over the pieces
    /// after it. Each new section: `beforeEach` (heat gate; false stops and keeps what is done), then `summarize`
    /// (the model run; nil = not started, which also stops), then `onSection` with all sections so far (false
    /// stops: for example the note was deleted). Throws what `summarize` throws (a cancel); sections finished
    /// before it were already handed to `onSection`.
    static func writeDue(pieces: [TranscriptPiece], existing: [NoteSection], isFinal: Bool, lang: String,
                         endSec: Int64, limit: Int = .max,
                         beforeEach: () async -> Bool,
                         summarize: ([TranscriptPiece], String) async throws -> SectionSummary?,
                         onSection: ([NoteSection]) async -> Bool = { _ in true }) async throws -> [NoteSection] {
        var done = NoteSection.validPrefix(existing, pieceCount: pieces.count)
        let from = NoteSection.covered(done)
        guard from < pieces.count, limit > 0 else { return done }
        let rest = Array(pieces[from...])
        var written = 0
        for r in plan(pieces: rest, endSec: endSec, isFinal: isFinal) {
            guard written < limit, !Task.isCancelled else { break }
            guard await beforeEach() else { break }
            let first = from + r.lowerBound, last = from + r.upperBound - 1
            guard let summary = try await summarize(Array(pieces[first...last]), lang) else { break }
            let start = max(0, pieces[first].startSec)
            let end = last + 1 < pieces.count ? pieces[last + 1].startSec : Int(clamping: endSec)
            done.append(NoteSection(idx: done.count, firstPiece: first, lastPiece: last,
                                    startSec: start, endSec: max(start, end), summary: summary))
            written += 1
            guard await onSection(done) else { break }
        }
        return done
    }

    /// True when `sections` cover every one of `pieceCount` pieces, so notes can be written from them alone.
    static func coversAll(_ sections: [NoteSection], pieceCount: Int) -> Bool {
        pieceCount > 0 && NoteSection.covered(NoteSection.validPrefix(sections, pieceCount: pieceCount)) == pieceCount
    }
}
