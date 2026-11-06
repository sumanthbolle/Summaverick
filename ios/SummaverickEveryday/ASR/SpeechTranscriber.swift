// On-device speech recognition using Apple's Speech framework.
// Hard rule: requiresOnDeviceRecognition is always true. If a language is not available on-device,
// the app refuses rather than silently sending audio to Apple's servers.
//
// On the Summaverick Everyday device this file is replaced by the core's whisper/IndicConformer backend;
// the pipeline only sees `transcribe(fileURL:) -> String` either way.
import AVFoundation
import Foundation
import Speech

struct SpeechLanguage: Identifiable, Hashable {
    let locale: Locale
    var id: String { locale.identifier }
    var name: String { Locale.current.localizedString(forIdentifier: locale.identifier) ?? locale.identifier }
    /// Two-letter code handed to the core ("en", "hi", "te") so the model writes in that language.
    var shortCode: String { locale.language.languageCode?.identifier ?? "en" }
}

enum TranscribeError: LocalizedError {
    case notAuthorized, notAvailableOnDevice(String), audioUnreadable, recognizerBusy
    var errorDescription: String? {
        switch self {
        case .notAuthorized: return "Speech recognition permission was not granted."
        case .notAvailableOnDevice(let l): return "On-device speech recognition for \(l) is not available on this iPhone. Open Settings > General > Keyboard > Dictation and add the language, then try again."
        case .audioUnreadable: return "The recording could not be read."
        case .recognizerBusy: return "The speech recogniser is busy."
        }
    }
}

final class SpeechTranscriber {
    static let chunkSeconds: Double = 55

    static func requestAuthorization() async -> Bool {
        await withCheckedContinuation { c in
            SFSpeechRecognizer.requestAuthorization { c.resume(returning: $0 == .authorized) }
        }
    }

    /// Languages that can be recognised fully on this device, most useful first.
    static func onDeviceLanguages() -> [SpeechLanguage] {
        let preferred = ["en-IN", "en-US", "en-GB", "hi-IN", "te-IN", "ta-IN", "kn-IN", "ml-IN", "mr-IN", "bn-IN", "gu-IN", "pa-IN", "ur-IN", "zh-CN", "ms-MY"]
        let supported = SFSpeechRecognizer.supportedLocales()
        let onDevice = supported.filter { SFSpeechRecognizer(locale: $0)?.supportsOnDeviceRecognition == true }
        let sorted = onDevice.sorted { a, b in
            let ia = preferred.firstIndex(of: a.identifier) ?? Int.max
            let ib = preferred.firstIndex(of: b.identifier) ?? Int.max
            return ia != ib ? ia < ib : a.identifier < b.identifier
        }
        return sorted.map { SpeechLanguage(locale: $0) }
    }

    /// Transcribes a WAV/CAF file in ~55 s chunks. progress: 0…1.
    func transcribe(fileURL: URL, locale: Locale, progress: @escaping (Double) -> Void) async throws -> String {
        guard let recognizer = SFSpeechRecognizer(locale: locale) else {
            throw TranscribeError.notAvailableOnDevice(locale.identifier)
        }
        guard recognizer.supportsOnDeviceRecognition else {
            throw TranscribeError.notAvailableOnDevice(locale.identifier)
        }
        guard recognizer.isAvailable else { throw TranscribeError.recognizerBusy }

        let chunks = try splitIntoChunks(fileURL)
        // Only remove temp chunk files, never the original recording (short files are passed through as-is).
        defer { chunks.filter { $0 != fileURL }.forEach { try? FileManager.default.removeItem(at: $0) } }

        var pieces: [String] = []
        for (i, chunk) in chunks.enumerated() {
            try Task.checkCancellation()
            let text = try await recognizeChunk(chunk, recognizer: recognizer)
            if !text.isEmpty { pieces.append(text) }
            progress(Double(i + 1) / Double(chunks.count))
        }
        return pieces.joined(separator: "\n")
    }

    private func recognizeChunk(_ url: URL, recognizer: SFSpeechRecognizer) async throws -> String {
        let request = SFSpeechURLRecognitionRequest(url: url)
        request.requiresOnDeviceRecognition = true   // never send audio off the phone
        request.shouldReportPartialResults = false
        request.addsPunctuation = true
        request.taskHint = .dictation

        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<String, Error>) in
            var finished = false
            var task: SFSpeechRecognitionTask?
            task = recognizer.recognitionTask(with: request) { result, error in
                guard !finished else { return }
                if let result, result.isFinal {
                    finished = true
                    c.resume(returning: result.bestTranscription.formattedString)
                } else if let error {
                    finished = true
                    let ns = error as NSError
                    // "No speech detected" (silent chunk) is not a failure.
                    if ns.domain == "kAFAssistantErrorDomain" && (ns.code == 1110 || ns.code == 216) {
                        c.resume(returning: "")
                    } else if ns.domain == "kLSRErrorDomain" && ns.code == 301 { // request cancelled/empty
                        c.resume(returning: "")
                    } else {
                        c.resume(throwing: error)
                    }
                }
            }
            _ = task
        }
    }

    /// Splits the recording into chunk files of the same format (sample rate/channels preserved).
    private func splitIntoChunks(_ url: URL) throws -> [URL] {
        guard let input = try? AVAudioFile(forReading: url) else { throw TranscribeError.audioUnreadable }
        let format = input.processingFormat
        let framesPerChunk = AVAudioFrameCount(SpeechTranscriber.chunkSeconds * format.sampleRate)
        let total = AVAudioFrameCount(input.length)
        if total <= framesPerChunk { return [url] }

        var out: [URL] = []
        let tmp = FileManager.default.temporaryDirectory
        var position: AVAudioFramePosition = 0
        var index = 0
        while position < AVAudioFramePosition(total) {
            let n = min(framesPerChunk, AVAudioFrameCount(AVAudioFramePosition(total) - position))
            guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: n) else { throw TranscribeError.audioUnreadable }
            input.framePosition = position
            try input.read(into: buffer, frameCount: n)
            let chunkURL = tmp.appendingPathComponent("sve-chunk-\(index).caf")
            try? FileManager.default.removeItem(at: chunkURL)
            let writer = try AVAudioFile(forWriting: chunkURL, settings: format.settings)
            try writer.write(from: buffer)
            out.append(chunkURL)
            position += AVAudioFramePosition(n)
            index += 1
        }
        return out
    }
}
