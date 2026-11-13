import Foundation
import SherpaOnnxC

/// Telugu speech recognizer on the CPU (sherpa-onnx, Moonshine-style files in the bundle's Shruti folder).
/// Safe to call from any thread: one serial queue guards the recognizer.
final class ShrutiRecognizer: @unchecked Sendable {
    enum Failure: Error { case notLoaded, filesMissing, loadFailed }

    static let shared = ShrutiRecognizer()
    static let sampleRate = 16000

    private let queue = DispatchQueue(label: "com.summaverick.shruti")
    private var recognizer: OpaquePointer?
    private var loadedThreads = 0
    private var readyCache: Bool?
    /// Set while a recognizer is held; read without the queue, so a caller can skip unload without waiting.
    private let loadedFlag = AtomicFlag()

    /// The five files are in the bundle's Shruti folder at the right sizes. Checked once per launch.
    var ready: Bool {
        queue.sync {
            if let readyCache { return readyCache }
            let ok = ShrutiRecognizer.checkFiles() == .ok
            readyCache = ok
            NSLog("[sve] shruti files %@", ok ? "ok" : "not ready")
            return ok
        }
    }

    var isLoaded: Bool { queue.sync { recognizer != nil } }

    /// Lock-free: false means nothing is loaded (no wait behind a decode in flight).
    var mayBeLoaded: Bool { loadedFlag.isSet }

    private static var folder: URL? { Bundle.main.url(forResource: "Shruti", withExtension: nil) }

    private static func checkFiles() -> ShrutiFiles.Verdict {
        guard let dir = folder else { return .missing }
        var sizes: [String: Int64?] = [:]
        for n in ShrutiFiles.names {
            let attrs = try? FileManager.default.attributesOfItem(atPath: dir.appendingPathComponent(n).path)
            sizes[n] = (attrs?[.size] as? NSNumber)?.int64Value
        }
        return ShrutiFiles.verdict(sizes: sizes)
    }

    /// Loads the recognizer with this many threads. No-op when it is already loaded with the same count.
    func load(threads: Int) throws {
        try queue.sync { try loadOnQueue(threads: threads) }
    }

    private func loadOnQueue(threads: Int) throws {
        if recognizer != nil && loadedThreads == threads { return }
        if let old = recognizer {
            SherpaOnnxDestroyOfflineRecognizer(old)
            recognizer = nil
            loadedFlag.set(false)
        }
        guard let dir = ShrutiRecognizer.folder?.path else { throw Failure.filesMissing }
        let started = Date()
        guard let r = ShrutiRecognizer.makeRecognizer(dir: dir, threads: threads) else {
            NSLog("[sve] shruti load failed (threads %d)", threads)
            throw Failure.loadFailed
        }
        recognizer = r
        loadedThreads = threads
        loadedFlag.set(true)
        NSLog("[sve] shruti loaded in %.1f s (threads %d)", Date().timeIntervalSince(started), threads)
    }

    func unload() {
        queue.sync {
            guard let r = recognizer else { return }
            SherpaOnnxDestroyOfflineRecognizer(r)
            recognizer = nil
            loadedThreads = 0
            loadedFlag.set(false)
            NSLog("[sve] shruti unloaded")
        }
    }

    /// Loads (a no-op at the same thread count) and decodes in one turn of the queue, so an unload from elsewhere
    /// can never land between the two: after one, the next piece loads again.
    func decode(_ samples: [Float], threads: Int) throws -> String {
        try queue.sync {
            try loadOnQueue(threads: threads)
            guard let r = recognizer else { throw Failure.notLoaded }
            return samples.withUnsafeBufferPointer { buf -> String in
                guard let base = buf.baseAddress else { return "" }
                return ShrutiRecognizer.decode(r, base, buf.count)
            }
        }
    }

    /// Text for one piece of up to 28 s of 16 kHz mono samples. All-silent input gives "".
    func decode(_ samples: [Float]) throws -> String {
        try queue.sync {
            guard let r = recognizer else { throw Failure.notLoaded }
            return samples.withUnsafeBufferPointer { buf -> String in
                guard let base = buf.baseAddress else { return "" }
                return ShrutiRecognizer.decode(r, base, buf.count)
            }
        }
    }

    /// The recognizer, set up like the Shruti iOS sample. nil when sherpa-onnx refuses the files.
    private static func makeRecognizer(dir: String, threads: Int) -> OpaquePointer? {
        var owned: [UnsafeMutablePointer<CChar>] = []
        defer { owned.forEach { free($0) } }
        func c(_ s: String) -> UnsafePointer<CChar> {
            let p = strdup(s)!
            owned.append(p)
            return UnsafePointer(p)
        }
        func file(_ n: String) -> UnsafePointer<CChar> { c((dir as NSString).appendingPathComponent(n)) }

        // Zeroed like sherpa-onnx's own C examples; unset strings are NULL, which the C API reads as "".
        var config = SherpaOnnxOfflineRecognizerConfig()
        config.feat_config.sample_rate = Int32(sampleRate)
        config.feat_config.feature_dim = 80
        config.model_config.moonshine.preprocessor = file("preprocess.onnx")
        config.model_config.moonshine.encoder = file("encode.onnx")
        config.model_config.moonshine.uncached_decoder = file("uncached_decode.onnx")
        config.model_config.moonshine.cached_decoder = file("cached_decode.onnx")
        config.model_config.tokens = file("tokens.txt")
        config.model_config.num_threads = Int32(threads)
        config.model_config.provider = c("cpu")
        config.model_config.debug = 0
        config.model_config.modeling_unit = c("cjkchar")   // SherpaOnnx.swift's default; not used by Moonshine
        config.decoding_method = c("greedy_search")
        config.max_active_paths = 4
        config.hotwords_score = 1.5
        return SherpaOnnxCreateOfflineRecognizer(&config)
    }

    private static func decode(_ recognizer: OpaquePointer, _ samples: UnsafePointer<Float>, _ count: Int) -> String {
        var silent = true
        for i in 0..<count where abs(samples[i]) >= 1e-4 { silent = false; break }
        if silent { return "" }
        guard let stream = SherpaOnnxCreateOfflineStream(recognizer) else { return "" }
        defer { SherpaOnnxDestroyOfflineStream(stream) }
        SherpaOnnxAcceptWaveformOffline(stream, Int32(sampleRate), samples, Int32(count))
        SherpaOnnxDecodeOfflineStream(recognizer, stream)
        guard let r = SherpaOnnxGetOfflineStreamResult(stream) else { return "" }
        defer { SherpaOnnxDestroyOfflineRecognizerResult(r) }
        guard let t = r.pointee.text else { return "" }
        return String(cString: t).trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
