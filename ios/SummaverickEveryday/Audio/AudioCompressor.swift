// Turns a finished note's WAV (16 kHz mono 16-bit, ~115 MB/hour) into AAC (32 kbit/s, ~14 MB/hour).
// This file only converts and checks; the caller swaps the note over and deletes the WAV afterwards.
import AVFoundation
import Foundation

enum AudioCompressor {
    enum Failure: Error { case notWav, empty }

    /// Writes `<name>.m4a.part` and renames it to `<name>.m4a` when complete, so a file with the final
    /// name is never half written. Any error removes the part file; the WAV is never touched.
    static func compress(wav: URL) async throws -> URL {
        try await Task.detached(priority: .utility) { try convert(wav: wav) }.value
    }

    nonisolated static func m4aURL(for wav: URL) -> URL { wav.deletingPathExtension().appendingPathExtension("m4a") }
    nonisolated static func partURL(for wav: URL) -> URL { m4aURL(for: wav).appendingPathExtension("part") }

    private nonisolated static func convert(wav: URL) throws -> URL {
        guard wav.pathExtension.lowercased() == "wav" else { throw Failure.notWav }
        let fm = FileManager.default
        let part = partURL(for: wav), final = m4aURL(for: wav)
        try? fm.removeItem(at: part)
        do {
            let input = try AVAudioFile(forReading: wav)
            guard input.length > 0 else { throw Failure.empty }
            let settings: [String: Any] = [
                AVFormatIDKey: kAudioFormatMPEG4AAC,
                AVSampleRateKey: 16_000.0,
                AVNumberOfChannelsKey: 1,
                AVEncoderBitRateKey: 32_000,
            ]
            var output: AVAudioFile? = try AVAudioFile(forWriting: part, settings: settings,
                                                       commonFormat: input.processingFormat.commonFormat,
                                                       interleaved: input.processingFormat.isInterleaved)
            let chunk: AVAudioFrameCount = 16_000 * 30
            guard let buffer = AVAudioPCMBuffer(pcmFormat: input.processingFormat, frameCapacity: chunk) else { throw Failure.empty }
            while input.framePosition < input.length {
                try input.read(into: buffer, frameCount: chunk)
                if buffer.frameLength == 0 { break }
                try output?.write(from: buffer)
            }
            output = nil   // closes the file so it is complete on disk
            try? fm.removeItem(at: final)   // left from an earlier attempt that was not swapped in
            try fm.moveItem(at: part, to: final)
            return final
        } catch {
            try? fm.removeItem(at: part)
            throw error
        }
    }

    /// The WAV this .m4a was made from (same folder, same name).
    nonisolated static func wavSibling(ofM4A path: String) -> String {
        ((path as NSString).deletingPathExtension) + ".wav"
    }

    /// The file opens as audio and is as long as the note says (within one second).
    nonisolated static func verify(m4a: URL, expectedSeconds: Double) -> Bool {
        guard let f = try? AVAudioFile(forReading: m4a), f.length > 0, f.processingFormat.sampleRate > 0 else { return false }
        let seconds = Double(f.length) / f.processingFormat.sampleRate
        return abs(seconds - expectedSeconds) <= 1.0
    }

    /// Deletes half-written `*.m4a.part` files left by an app kill. Returns how many.
    @discardableResult
    nonisolated static func sweepParts(in dir: URL) -> Int {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        var n = 0
        for f in files where f.hasSuffix(".m4a.part") {
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(f)); n += 1
        }
        return n
    }
}
