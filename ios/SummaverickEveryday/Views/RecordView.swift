import SwiftUI

struct RecordView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var controller = RecordingController.shared
    @State private var live = LiveTranscriber.shared
    @State private var permissionDenied = false

    var body: some View {
        VStack(spacing: 28) {
            HStack {
                Button("Cancel", role: .cancel) { cancel() }
                Spacer()
            }
            .padding(.horizontal)

            Spacer()

            RecordingPulseView(level: controller.recorder.level, active: controller.isRecording)
                .frame(width: live.lines.isEmpty ? 320 : 220, height: live.lines.isEmpty ? 320 : 220)
                .animation(.easeInOut, value: live.lines.isEmpty)

            // 0 until this recording runs (the shared recorder keeps the last one's time after Stop).
            Text(timeString(controller.isRecording ? controller.elapsed : 0))
                .font(.largeTitle.weight(.light).monospacedDigit())
                .contentTransition(.numericText())

            Text(controller.isRecording ? "Recording on this phone. Lock the screen if you like; recording continues." : "Starting…")
                .font(.footnote).foregroundStyle(Theme.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 32)

            if let e = controller.recorder.lastError { Text(e).font(.footnote).foregroundStyle(Theme.warning) }

            // Stop when quiet: in front, this line takes the place of the notification.
            if controller.isRecording && controller.quietWarning { quietWarning }

            if controller.isRecording { liveTranscript }

            Spacer()

            Button { stopAndProcess() } label: {
                Label("Stop and write my notes", systemImage: "stop.fill")
                    .font(.title3.weight(.semibold))
                    .frame(maxWidth: .infinity).padding(.vertical, 16)
            }
            .buttonStyle(.pillProminent)
            .buttonBorderShape(.capsule)
            .disabled(!controller.isRecording)
            .padding(.horizontal, 24)
            .padding(.bottom, 24)
        }
        .padding(.top)
        .background(Theme.background)
        .interactiveDismissDisabled()
        .task { await begin() }
        .onAppear { controller.recordScreenShown = true }
        .onDisappear { controller.recordScreenShown = false }
        // Stopped from the Live Activity, a shortcut or the control: this screen has nothing left to show.
        .onChange(of: controller.isRecording) { was, now in if was && !now { dismiss() } }
        .alert("Microphone access needed", isPresented: $permissionDenied) {
            Button("Open Settings") { if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }; dismiss() }
            Button("Cancel", role: .cancel) { dismiss() }
        } message: { Text("Allow the microphone for Summaverick in Settings.") }
    }

    private var quietWarning: some View {
        VStack(spacing: 8) {
            Text("It's gone quiet. I'll stop recording in 30 seconds.")
                .font(.footnote.weight(.medium)).foregroundStyle(Theme.caution)
                .multilineTextAlignment(.center)
            Button("Keep recording") { controller.keepRecording() }
                .buttonStyle(.pill)
                .frame(minHeight: 44)
        }
        .padding(.horizontal, 32)
    }

    /// The transcript so far (last lines, newest at the bottom) and one status line.
    private var liveTranscript: some View {
        VStack(spacing: 8) {
            if !live.lines.isEmpty {
                ScrollViewReader { proxy in
                    ScrollView {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(Array(live.lines.suffix(6).enumerated()), id: \.offset) { i, line in
                                Text(line).font(.callout).frame(maxWidth: .infinity, alignment: .leading).id(i)
                            }
                        }
                        .padding(.horizontal, 24)
                    }
                    .frame(maxHeight: 150)
                    .onAppear { proxy.scrollTo(min(5, live.lines.count - 1), anchor: .bottom) }
                    .onChange(of: live.lines.count) { _, n in
                        withAnimation { proxy.scrollTo(min(5, n - 1), anchor: .bottom) }
                    }
                }
            }
            Text(live.statusLine)
                .font(.footnote.weight(.medium)).foregroundStyle(Theme.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 32)
        }
    }

    /// Already recording (started by a shortcut): this screen shows that recording. A start that fails shows the
    /// recorder's error, as before.
    private func begin() async {
        switch await controller.start() {
        case .started, .alreadyRecording, .failed: break
        case .micDenied: permissionDenied = true
        case .needsConsent: dismiss()   // the record button asks for it first; never record without it
        case .stoppedWhileStarting: dismiss()   // stopped from a shortcut or the control before it began
        }
    }

    private func cancel() {
        controller.cancel()   // deletes the audio after the piece in flight is dropped
        dismiss()
    }

    private func stopAndProcess() {
        // Synchronous, before anything awaits: claims the work and the background time right in the tap.
        guard controller.stop() else { return }
        dismiss()
    }

    private func timeString(_ t: TimeInterval) -> String {
        let s = Int(t)
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60) : String(format: "%02d:%02d", s / 60, s % 60)
    }
}
