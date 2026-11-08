// The recording's Live Activity. The iPhone 16e has no Dynamic Island, so the Lock Screen / banner view is the one
// that matters; the island regions are kept short. Recording: a red dot, the elapsed time and Stop (a plain mark
// here; R3 makes it stop the recording). After Stop: the stage line and a progress bar. Then "Notes ready" with the
// title, or the paused / failed line.
import ActivityKit
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
            }
            Spacer(minLength: 0)
            if state.phase == .recording {
                // Inert in R2: R3 turns it into Button(intent: StopRecordingIntent()).
                Label("Stop", systemImage: "stop.fill")
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(Capsule().fill(Color.white.opacity(0.2)))
            }
        }
        .foregroundStyle(.white)
    }

    private var title: Text {
        switch state.phase {
        case .recording: return Text("Recording")
        case .processing: return Text("Writing your notes")
        case .ready: return Text("Notes ready")
        case .paused: return Text("Paused")
        case .failed: return Text("Notes could not be written")
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
