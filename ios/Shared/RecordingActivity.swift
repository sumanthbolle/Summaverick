// The recording's Live Activity: what the app sends and the widget extension shows (compiled into both targets).
// While recording: the elapsed time. After Stop: the stage line and percent of the note's work, then "Notes ready"
// with the title, or the paused / failed line. A quiet line (Stop when quiet) is shown under it when there is one.
// Foundation only apart from the ActivityKit conformance, so the pure rules (LiveActivityLogic) can be checked with
// swiftc on a Mac.
import Foundation
#if canImport(ActivityKit) && os(iOS)
import ActivityKit
#endif

struct RecordingActivity: Codable, Hashable {
    struct ContentState: Codable, Hashable {
        enum Phase: String, Codable, Hashable {
            case recording, processing, ready, paused, failed
        }
        var phase: Phase
        /// When the recording started (the elapsed timer counts from here).
        var startedAt: Date
        /// Processing: the stage line. Ready: the note's title. Paused / failed: their line.
        var stageLine: String
        /// Processing: the work done for this note, 0…100.
        var percent: Int
        /// Known once the note exists (after Stop).
        var noteID: Int64?
        /// Stop when quiet: while recording, the warning that it stops soon; after an automatic stop, why it stopped
        /// ("Stopped after 5 min of quiet"), kept through the note's stages. nil: nothing to say.
        var quietLine: String? = nil
    }
}

#if canImport(ActivityKit) && os(iOS)
extension RecordingActivity: ActivityAttributes {}
#endif
