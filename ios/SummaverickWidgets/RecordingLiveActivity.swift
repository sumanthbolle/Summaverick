// The recording's Live Activity. The iPhone 16e has no Dynamic Island, so the Lock Screen / banner view is the one
// that matters; the island regions are kept short. Recording: a red dot, the elapsed time and Stop
// (StopRecordingIntent, performed in the app). After Stop: the stage line and a progress bar. Then "Your notes are ready" with the
// title, or the paused / failed line. Under it, the quiet line when there is one (Stop when quiet).
import ActivityKit
import AppIntents
import SwiftUI
import WidgetKit

struct RecordingLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: RecordingActivity.self) { context in
            RecordingActivityView(state: context.state)
                .padding(16)
                .activityBackgroundTint(Color.black.opacity(0.6))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.center) {
                    RecordingActivityView(state: context.state)
                }
            } compactLeading: {
                PhaseMark(phase: context.state.phase)
            } compactTrailing: {
                CompactDetail(state: context.state)
            } minimal: {
                PhaseMark(phase: context.state.phase)
            }
        }
    }
}

private struct RecordingActivityView: View {
    let state: RecordingActivity.ContentState

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            PhaseMark(phase: state.phase)
            VStack(alignment: .leading, spacing: 4) {
                title.font(.headline)
                switch state.phase {
                case .recording:
                    Text(timerInterval: state.startedAt...Date.distantFuture, countsDown: false)
                        .font(.title2.monospacedDigit().weight(.semibold))
                case .processing:
                    Text(state.stageLine).font(.subheadline).lineLimit(2)
                    ProgressView(value: Double(state.percent), total: 100)
                        .tint(.white)
                case .ready, .paused, .failed:
                    Text(state.stageLine).font(.subheadline).lineLimit(3)
                }
                // Stop when quiet: the warning while recording, or why it stopped (written by the app, localized there).
                if let quiet = state.quietLine, !quiet.isEmpty {
                    Text(verbatim: quiet).font(.caption).lineLimit(2).opacity(0.85)
                }
            }
            Spacer(minLength: 0)
            if state.phase == .recording {
                Button(intent: StopRecordingIntent()) {
                    Label("Stop", systemImage: "stop.fill")
                        .font(.subheadline.weight(.semibold))
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(Capsule().fill(Color.white.opacity(0.2)))
                }
                .buttonStyle(.plain)
            }
        }
        .foregroundStyle(.white)
    }

    private var title: Text {
        switch state.phase {
        case .recording: return Text("Recording")
        case .processing: return Text("Writing your notes")
        case .ready: return Text("Your notes are ready")
        case .paused: return Text("Taking a short break")
        case .failed: return Text("I couldn't write your notes")
        }
    }
}

private struct PhaseMark: View {
    let phase: RecordingActivity.ContentState.Phase

    var body: some View {
        switch phase {
        case .recording: Circle().fill(Color.red).frame(width: 12, height: 12)
        case .processing: Image(systemName: "text.badge.star")
        case .ready: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .paused: Image(systemName: "pause.circle.fill").foregroundStyle(.orange)
        case .failed: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.red)
        }
    }
}

private struct CompactDetail: View {
    let state: RecordingActivity.ContentState

    var body: some View {
        switch state.phase {
        case .recording:
            Text(timerInterval: state.startedAt...Date.distantFuture, countsDown: false)
                .monospacedDigit().frame(maxWidth: 56)
        case .processing:
            Text(verbatim: "\(state.percent)%").monospacedDigit()
        case .ready, .paused, .failed:
            EmptyView()
        }
    }
}
