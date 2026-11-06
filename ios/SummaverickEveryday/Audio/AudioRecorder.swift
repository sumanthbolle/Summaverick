// Records 16 kHz mono 16-bit PCM WAV — the exact format the core's whisper backend reads on the device,
// so a recording made on the phone can be processed by the device build without conversion.
import AVFoundation
import Foundation
import Observation

@MainActor
@Observable
final class AudioRecorder {
    private(set) var isRecording = false
    private(set) var elapsed: TimeInterval = 0
    private(set) var level: Float = 0      // 0…1, for the UI meter
    private(set) var currentURL: URL?
    var lastError: String?

    private var recorder: AVAudioRecorder?
    private var timer: Timer?
    private var startDate: Date?

    static var recordingsDirectory: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Summaverick/recordings", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// Ask's voice questions: short-lived WAVs, deleted after transcription and swept at launch.
    static var askVoiceDirectory: URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ask-voice", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// Removes voice-question files left behind (e.g. the app was closed while transcribing).
    nonisolated static func sweepAskVoice() {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ask-voice", isDirectory: true)
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
        for f in files { try? FileManager.default.removeItem(at: f) }
    }

    func requestPermission() async -> Bool {
        if #available(iOS 17.0, *) {
            return await AVAudioApplication.requestRecordPermission()
        } else {
            return await withCheckedContinuation { c in AVAudioSession.sharedInstance().requestRecordPermission { c.resume(returning: $0) } }
        }
    }

    /// directory: where the WAV goes (default: the recordings folder). Ask's voice questions use a temp folder.
    func start(in directory: URL? = nil) {
        guard !isRecording else { return }
        lastError = nil
        do {
            let session = AVAudioSession.sharedInstance()
            // .record + allowBluetoothHFP so a Bluetooth mic/earpiece can be used; background mode "audio" keeps it alive when locked.
            try session.setCategory(.record, mode: .default, options: [.allowBluetoothHFP])
            try session.setActive(true)

            let url = (directory ?? AudioRecorder.recordingsDirectory)
                .appendingPathComponent("rec-\(Int(Date().timeIntervalSince1970)).wav")
            let settings: [String: Any] = [
                AVFormatIDKey: Int(kAudioFormatLinearPCM),
                AVSampleRateKey: 16_000.0,
                AVNumberOfChannelsKey: 1,
                AVLinearPCMBitDepthKey: 16,
                AVLinearPCMIsFloatKey: false,
                AVLinearPCMIsBigEndianKey: false,
            ]
            let r: AVAudioRecorder
            do {
                r = try AVAudioRecorder(url: url, settings: settings)
            } catch {
                try? FileManager.default.removeItem(at: url)
                throw error
            }
            r.isMeteringEnabled = true
            guard r.record() else {
                r.deleteRecording()
                try? FileManager.default.removeItem(at: url)   // no partial file left behind
                lastError = String(localized: "Could not start recording.")
                return
            }
            recorder = r
            currentURL = url
            startDate = Date()
            isRecording = true
            elapsed = 0
            timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.tick() }
            }
        } catch {
            lastError = error.localizedDescription
        }
    }

    private func tick() {
        guard let r = recorder, let s = startDate else { return }
        elapsed = Date().timeIntervalSince(s)
        r.updateMeters()
        let db = r.averagePower(forChannel: 0) // -160…0
        level = max(0, min(1, (db + 50) / 50))
    }

    /// Stops and returns (file, duration in seconds).
    func stop() -> (URL, Int64)? {
        guard isRecording, let r = recorder, let url = currentURL else { return nil }
        timer?.invalidate(); timer = nil
        let duration = Int64(r.currentTime.rounded())
        r.stop()
        recorder = nil
        isRecording = false
        level = 0
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        return (url, duration)
    }
}
