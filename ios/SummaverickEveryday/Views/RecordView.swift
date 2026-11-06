import SwiftUI

struct RecordView: View {
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss
    @State private var recorder = AudioRecorder()
    @State private var permissionDenied = false
    @State private var language: SpeechLanguage?   // resolved once; the lookup is expensive

    var body: some View {
        VStack(spacing: 28) {
            HStack {
                Button("Cancel", role: .cancel) { cancel() }
                Spacer()
                Text(language?.name ?? "").font(.footnote).foregroundStyle(.secondary)
            }
            .padding(.horizontal)

            Spacer()

            RecordingPulseView(level: recorder.level, active: recorder.isRecording)
                .frame(width: 320, height: 320)

            Text(timeString(recorder.elapsed))
                .font(.system(size: 48, weight: .thin, design: .rounded).monospacedDigit())
                .contentTransition(.numericText())

            Text(recorder.isRecording ? "Recording on this phone. Lock the screen if you like; recording continues." : "Starting…")
                .font(.footnote).foregroundStyle(.secondary)
                .multilineTextAlignment(.center).padding(.horizontal, 32)

            if let e = recorder.lastError { Text(e).font(.footnote).foregroundStyle(.red) }

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
        } message: { Text("Allow microphone and speech recognition for Summaverick in Settings.") }
    }

    private func begin() async {
        language = settings.language
        let mic = await recorder.requestPermission()
        let speech = await SpeechTranscriber.requestAuthorization()
        guard mic && speech else { permissionDenied = true; return }
        recorder.start()
    }

    private func cancel() {
        if let (url, _) = recorder.stop() { try? FileManager.default.removeItem(at: url) }
        dismiss()
    }

    private func stopAndProcess() {
        guard let (url, duration) = recorder.stop() else { return }
        let lang = language ?? settings.language
        dismiss()
        Task { await ProcessingPipeline.shared.process(audio: url, duration: duration, language: lang) }
    }

    private func timeString(_ t: TimeInterval) -> String {
        let s = Int(t)
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60) : String(format: "%02d:%02d", s / 60, s % 60)
    }
}
