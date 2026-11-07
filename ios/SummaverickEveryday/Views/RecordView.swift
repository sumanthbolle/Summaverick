import SwiftUI

struct RecordView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var recorder = AudioRecorder()
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

            RecordingPulseView(level: recorder.level, active: recorder.isRecording)
                .frame(width: live.lines.isEmpty ? 320 : 220, height: live.lines.isEmpty ? 320 : 220)
                .animation(.easeInOut, value: live.lines.isEmpty)

            Text(timeString(recorder.elapsed))
                .font(.system(size: 48, weight: .thin, design: .rounded).monospacedDigit())
                .contentTransition(.numericText())

            Text(recorder.isRecording ? "Recording on this phone. Lock the screen if you like; recording continues." : "Starting…")
                .font(.footnote).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 32)

            if let e = recorder.lastError { Text(e).font(.footnote).foregroundStyle(.red) }

            if recorder.isRecording { liveTranscript }

            Spacer()

            Button { stopAndProcess() } label: {
                Label("Stop and make notes", systemImage: "stop.fill")
                    .font(.title3.weight(.semibold))
                    .frame(maxWidth: .infinity).padding(.vertical, 16)
            }
            .buttonStyle(.borderedProminent)
            .buttonBorderShape(.capsule)
            .tint(.primary)
            .foregroundStyle(Color(.systemBackground))
            .disabled(!recorder.isRecording)
            .padding(.horizontal, 24)
            .padding(.bottom, 24)
        }
        .padding(.top)
        .background(Color(.systemBackground))
        .interactiveDismissDisabled()
        .task { await begin() }
        .alert("Microphone access needed", isPresented: $permissionDenied) {
            Button("Open Settings") { if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }; dismiss() }
            Button("Cancel", role: .cancel) { dismiss() }
        } message: { Text("Allow the microphone for Summaverick in Settings.") }
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
                .font(.footnote.weight(.medium)).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 32)
        }
    }

    private func begin() async {
        guard await recorder.requestPermission() else { permissionDenied = true; return }
        recorder.start()
        if recorder.isRecording, let url = recorder.currentURL {
            live.recordingStarted(audio: url) { [recorder] in recorder.elapsed }
        }
    }

    private func cancel() {
        let stopped = recorder.stop()
        dismiss()
        // Drop the piece in flight first, so nothing is written next to the deleted recording.
        Task {
            guard let (url, _) = stopped else { return }
            await live.cancel(audioPath: url.path)
            AudioRecorder.removeAudio(atPath: url.path)
        }
    }

    private func stopAndProcess() {
        guard let (url, duration) = recorder.stop() else { return }
        // Synchronous, before anything awaits: claims the work and the background time right in the tap.
        ProcessingPipeline.shared.recordingStopped(audio: url, duration: duration)
        dismiss()
    }

    private func timeString(_ t: TimeInterval) -> String {
        let s = Int(t)
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60) : String(format: "%02d:%02d", s / 60, s % 60)
    }
}
