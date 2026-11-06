// Small pure rules behind the AI download, the device check, heat and the waiting-notes queue.
// Foundation only and free of app state, so they can be checked with a standalone swiftc script.
import Foundation

enum DownloadMath {
    static let gigabyte: Int64 = 1_000_000_000   // decimal GB, as iOS Settings > Storage counts

    /// Combined progress over several files, weighted by bytes. 0 when nothing is known.
    static func progress(_ parts: [(done: Int64, total: Int64)]) -> Double {
        let total = parts.reduce(Int64(0)) { $0 + max(0, $1.total) }
        guard total > 0 else { return 0 }
        let done = parts.reduce(Int64(0)) { $0 + min(max(0, $1.done), max(0, $1.total)) }
        return Double(done) / Double(total)
    }

    /// Bytes still to download over all files.
    static func remaining(_ parts: [(done: Int64, total: Int64)]) -> Int64 {
        parts.reduce(Int64(0)) { $0 + max(0, $1.total - max(0, $1.done)) }
    }

    /// "about X min left": nil until a speed is known; at least 1.
    static func minutesLeft(remainingBytes: Int64, bytesPerSecond: Double) -> Int? {
        guard bytesPerSecond > 0, remainingBytes > 0 else { return nil }
        return max(1, Int((Double(remainingBytes) / bytesPerSecond / 60).rounded(.up)))
    }
}

enum SpaceCheck {
    /// Head room kept free on top of the download itself.
    static let margin: Int64 = DownloadMath.gigabyte

    /// True when the download fits: available ≥ remaining + 1 GB. Unknown free space never blocks.
    static func hasRoom(available: Int64?, remaining: Int64) -> Bool {
        guard let available else { return true }
        return available >= remaining + margin
    }

    /// Whole GB to free ("Free up about N GB"), rounded up, at least 1. 0 when it already fits.
    static func gigabytesToFree(available: Int64?, remaining: Int64) -> Int {
        guard let available, !hasRoom(available: available, remaining: remaining) else { return 0 }
        let short = remaining + margin - available
        return max(1, Int((Double(short) / Double(DownloadMath.gigabyte)).rounded(.up)))
    }
}

enum RetrySchedule {
    /// Wait before the next automatic try after `failures` failures in a row (30 s, 2 min, 10 min).
    /// nil: stop retrying in this launch; the next launch starts again.
    static func delay(afterFailures failures: Int) -> TimeInterval? {
        let steps: [TimeInterval] = [30, 120, 600]
        guard failures >= 1, failures <= steps.count else { return nil }
        return steps[failures - 1]
    }
}

enum DeviceGate {
    /// Physical memory a phone must report to run the AI. iPhones ship with 4, 6 or 8 GB; a 6 GB
    /// iPhone reports a little under 6 GiB (the system keeps some), so the line sits well between
    /// 4 GB and 6 GB phones rather than at 5.5 GiB, where a 6 GB phone could fall just below.
    static let minimumGiB = 5.0

    static func isSupported(physicalMemory: UInt64) -> Bool {
        Double(physicalMemory) / 1_073_741_824 >= minimumGiB
    }
}

enum Heat {
    /// Serious or critical: stop model work and let the phone cool down.
    static func isTooHot(_ state: ProcessInfo.ThermalState) -> Bool {
        state == .serious || state == .critical
    }
}

enum SizeCheck {
    /// The full size the finished file must have: the total of a Content-Range ("bytes 0-9/10") on a
    /// resumed (206) response, Content-Length on a 200, else the size recorded while downloading.
    static func expectedSize(status: Int, contentLength: Int64, contentRange: String?, recorded: Int64?) -> Int64? {
        if status == 206, let r = contentRange, let slash = r.lastIndex(of: "/"),
           let total = Int64(r[r.index(after: slash)...].trimmingCharacters(in: .whitespaces)), total > 0 {
            return total
        }
        if status == 200, contentLength > 0 { return contentLength }
        if let recorded, recorded > 0 { return recorded }
        return nil
    }

    /// Unknown expected size is accepted (nothing to compare with); otherwise sizes must be equal.
    static func isComplete(actual: Int64, expected: Int64?) -> Bool {
        guard let expected else { return actual > 0 }
        return actual == expected
    }
}

enum WaitingNotes {
    struct Candidate: Equatable {
        let id: Int64
        let hasAudio: Bool
        let hasTranscript: Bool
    }

    /// Notes to process once the AI is ready: audio still there, no transcript yet, and not already
    /// tried automatically (so a silent recording is not re-run at every launch). Oldest first.
    static func toProcess(_ notes: [Candidate], alreadyTried: Set<Int64>) -> [Int64] {
        notes.filter { $0.hasAudio && !$0.hasTranscript && !alreadyTried.contains($0.id) }
            .map(\.id)
            .sorted()
    }
}
