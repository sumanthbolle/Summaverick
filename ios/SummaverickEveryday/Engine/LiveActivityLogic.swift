// Pure rules of the recording's Live Activity: which state each pipeline stage (and each outcome) shows, when the
// activity ends and how it is dismissed, and how often progress is sent. Foundation only and free of app state, so
// they can be checked with a standalone swiftc script (with Shared/RecordingActivity.swift).
import Foundation

enum LiveActivityLogic {
    /// What the activity follows: the recording, then its note's pipeline stages (one case per
    /// ProcessingPipeline.Stage), then how the note's run ended.
    enum Stage: Equatable {
        case recording
        // ProcessingPipeline.Stage, one for one
        case idle
        case transcribing(Double)
        case loadingModel
        case extracting
        case sections(Int, Int)
        case generating(Int, Int)
        case done
        case failed
        case paused
        case cooling
        case overnight
        case memories(Int, Int)
        // Outcomes
        /// Stopped while another note is being written: it waits its turn.
        case queued
        /// The notes are stored.
        case ready
        /// iOS ended the background work; the note finishes by itself later.
        case cutOff
        /// The AI has not finished downloading; the note waits for it.
        case waitingForAI
        /// The recording was cancelled (its audio is deleted).
        case cancelled
        /// The note was deleted.
        case deleted
    }

    /// How an ended activity leaves the Lock Screen.
    enum Ending: Equatable {
        /// At once (cancelled, deleted, or left over from a closed app).
        case immediately
        /// The system's usual: it stays on the Lock Screen until the person clears it, or up to four hours.
        case standard
        /// At this many seconds after it ended.
        case after(TimeInterval)
    }

    /// "Your notes are ready" stays this long on the Lock Screen.
    static let readyStaysFor: TimeInterval = 4 * 3600
    /// Progress within the same stage is sent at most this often.
    static let updateEvery: TimeInterval = 5

    static func phase(for stage: Stage) -> RecordingActivity.ContentState.Phase {
        switch stage {
        case .recording: return .recording
        case .transcribing, .loadingModel, .extracting, .sections, .generating, .cooling, .queued: return .processing
        case .ready, .done, .memories: return .ready
        case .paused, .cutOff, .overnight, .waitingForAI, .idle: return .paused
        case .failed: return .failed
        case .cancelled, .deleted: return .recording   // not shown: the activity goes at once
        }
    }

    /// The state to show for a stage. line: the stage line (processing), the note's title (ready), or the paused or
    /// failed line. startedAt and the note id stay as they were. Paused or failed keeps the percent the work got to:
    /// a cut-off reported as 0 (a note that waited its turn, a run whose progress is not known) never resets it.
    static func state(for stage: Stage, line: String, percent: Int,
                      after previous: RecordingActivity.ContentState) -> RecordingActivity.ContentState {
        if stage == .cancelled || stage == .deleted { return previous }
        var s = previous
        s.phase = phase(for: stage)
        s.stageLine = line
        let p = max(0, min(100, percent))
        switch s.phase {
        case .ready: s.percent = 100
        case .paused, .failed: s.percent = max(previous.percent, p)
        case .recording, .processing: s.percent = p
        }
        return s
    }

    /// nil: the activity stays. Ready, failed, cancelled and deleted end it. A pause (paused, cut off, overnight,
    /// waiting for the AI, idle after one) keeps it, showing the pause, so the same card moves on to the work and
    /// "ready" when the note carries on: an ended "Taking a short break" stayed on the lock screen after the note
    /// finished. An app that is closed meanwhile ends it at the next launch (RecordingActivities.endStale).
    static func ending(for stage: Stage) -> Ending? {
        switch stage {
        case .recording, .transcribing, .loadingModel, .extracting, .sections, .generating, .cooling, .queued: return nil
        case .paused, .cutOff, .overnight, .waitingForAI, .idle: return nil
        case .ready, .done, .memories: return .after(readyStaysFor)
        case .failed: return .standard
        case .cancelled, .deleted: return .immediately
        }
    }

    /// Sent at once on a stage change or an ending; within the same stage, about every `updateEvery` seconds.
    static func updateDue(last: Stage?, lastSentAt: Date?, next: Stage, now: Date) -> Bool {
        guard let last, let lastSentAt else { return true }
        if ending(for: next) != nil || kind(last) != kind(next) { return true }
        let since = now.timeIntervalSince(lastSentAt)
        return since >= updateEvery
    }

    /// The stage without its numbers: a new section or percent is the same stage.
    private static func kind(_ s: Stage) -> Int {
        switch s {
        case .recording: return 0
        case .idle: return 1
        case .transcribing: return 2
        case .loadingModel: return 3
        case .extracting: return 4
        case .sections: return 5
        case .generating: return 6
        case .done: return 7
        case .failed: return 8
        case .paused: return 9
        case .cooling: return 10
        case .overnight: return 11
        case .memories: return 12
        case .queued: return 13
        case .ready: return 14
        case .cutOff: return 15
        case .waitingForAI: return 16
        case .cancelled: return 17
        case .deleted: return 18
        }
    }
}
