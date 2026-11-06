// Downloads and keeps track of the on-device language model (GGUF).
// Download is the only time the app uses the network. Model files are excluded from iCloud backup.
import Foundation
import Observation

struct ModelSpec: Identifiable, Hashable {
    let id: String
    let name: String
    let fileName: String
    let url: URL
    let approxMB: Int
    let minRAMGB: Int
    let notes: String
    /// Audio projector for models that listen to audio directly (Gemma 4). Downloaded alongside the model.
    var mmprojFileName: String? = nil
    var mmprojURL: URL? = nil
    var contextTokens: Int32 = 4096
    /// True when this model transcribes the recording itself (any language mix) instead of Apple Speech.
    var hearsAudio: Bool { mmprojFileName != nil }
}

enum ModelCatalog {
    // Official Qwen GGUFs (Apache 2.0). Q4_K_M keeps RAM low; Q8_0 is noticeably better if the phone has the headroom.
    static let all: [ModelSpec] = [
        ModelSpec(id: "qwen3-0.6b-q8", name: "Qwen3 0.6B (recommended)", fileName: "Qwen3-0.6B-Q8_0.gguf",
                  url: URL(string: "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf")!,
                  approxMB: 640, minRAMGB: 3, notes: "Fast on any iPhone from the last five years. Good English; basic Hindi."),
        // Qwen only publishes Q8_0 for 1.7B (1.8 GB); the Q4_K_M build (1.1 GB) is from unsloth's official-weights conversion.
        ModelSpec(id: "qwen3-1.7b-q4", name: "Qwen3 1.7B (better quality)", fileName: "Qwen3-1.7B-Q4_K_M.gguf",
                  url: URL(string: "https://huggingface.co/unsloth/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q4_K_M.gguf")!,
                  approxMB: 1100, minRAMGB: 6, notes: "Clearer notes and better Indian-language output. Needs a 6 GB+ iPhone/iPad."),
        // Google Gemma 4 E4B (Apache 2.0). Hears the audio itself, so it understands Telugu, Hindi and mixed
        // speech that Apple's recogniser cannot. Weights: unsloth Q4_K_M; audio projector: ggml-org Q8_0.
        ModelSpec(id: "gemma4-e4b-q4", name: "Gemma 4 E4B (any language, hears audio)", fileName: "gemma-4-E4B-it-Q4_K_M.gguf",
                  url: URL(string: "https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_K_M.gguf")!,
                  approxMB: 5500, minRAMGB: 8,
                  notes: "Understands mixed languages such as Telugu with English terms and writes the notes. Large download; needs an 8 GB iPhone. Slower: roughly real time.",
                  mmprojFileName: "mmproj-gemma-4-E4B-it-Q8_0.gguf",
                  mmprojURL: URL(string: "https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF/resolve/main/mmproj-gemma-4-E4B-it-Q8_0.gguf")!,
                  contextTokens: 8192),
    ]
    static var recommended: ModelSpec {
        let ramGB = Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824
        return ramGB >= 5.5 ? all[1] : all[0]
    }
}

@MainActor
@Observable
final class ModelManager: NSObject {
    static let shared = ModelManager()

    var selectedID: String {
        didSet { UserDefaults.standard.set(selectedID, forKey: "model.selected") }
    }
    var downloadProgress: [String: Double] = [:]
    var downloadError: String?

    private var tasks: [String: URLSessionDownloadTask] = [:]      // key: spec id, or "<id>#mmproj"
    private var taskBytes: [String: (done: Int64, total: Int64)] = [:]
    @ObservationIgnored private lazy var session: URLSession = {
        let cfg = URLSessionConfiguration.default
        cfg.waitsForConnectivity = true
        return URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
    }()

    private override init() {
        selectedID = UserDefaults.standard.string(forKey: "model.selected") ?? ModelCatalog.recommended.id
        super.init()
    }

    var selected: ModelSpec { ModelCatalog.all.first { $0.id == selectedID } ?? ModelCatalog.recommended }

    nonisolated static var modelsDirectory: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Summaverick/models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func fileURL(for spec: ModelSpec) -> URL { ModelManager.modelsDirectory.appendingPathComponent(spec.fileName) }
    func mmprojURL(for spec: ModelSpec) -> URL? {
        spec.mmprojFileName.map { ModelManager.modelsDirectory.appendingPathComponent($0) }
    }

    private func present(_ url: URL) -> Bool {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = attrs[.size] as? Int64 else { return false }
        return size > 10_000_000
    }

    func isInstalled(_ spec: ModelSpec) -> Bool {
        guard present(fileURL(for: spec)) else { return false }
        if let mm = mmprojURL(for: spec) { return present(mm) }
        return true
    }

    var selectedInstalled: Bool { isInstalled(selected) }

    func download(_ spec: ModelSpec) {
        guard tasks[spec.id] == nil, tasks[spec.id + "#mmproj"] == nil else { return }
        downloadError = nil
        downloadProgress[spec.id] = 0
        var jobs: [(String, URL)] = []
        if !present(fileURL(for: spec)) { jobs.append((spec.id, spec.url)) }
        if let u = spec.mmprojURL, let local = mmprojURL(for: spec), !present(local) { jobs.append((spec.id + "#mmproj", u)) }
        for (key, url) in jobs {
            let task = session.downloadTask(with: url)
            task.taskDescription = key
            tasks[key] = task
            task.resume()
        }
        if jobs.isEmpty { downloadProgress[spec.id] = nil }
    }

    func cancelDownload(_ spec: ModelSpec) {
        for key in [spec.id, spec.id + "#mmproj"] {
            tasks[key]?.cancel()
            tasks[key] = nil
            taskBytes[key] = nil
        }
        downloadProgress[spec.id] = nil
    }

    func delete(_ spec: ModelSpec) {
        try? FileManager.default.removeItem(at: fileURL(for: spec))
        if let mm = mmprojURL(for: spec) { try? FileManager.default.removeItem(at: mm) }
    }

    /// Combined progress over the model file and its projector, weighted by size.
    fileprivate func updateProgress(key: String, done: Int64, total: Int64) {
        taskBytes[key] = (done, total)
        let specID = key.replacingOccurrences(of: "#mmproj", with: "")
        let parts = [specID, specID + "#mmproj"].compactMap { taskBytes[$0] }
        let t = parts.reduce(Int64(0)) { $0 + $1.total }
        if t > 0 { downloadProgress[specID] = Double(parts.reduce(Int64(0)) { $0 + $1.done }) / Double(t) }
    }

    fileprivate func finished(key: String, error: String?) {
        tasks[key] = nil
        let specID = key.replacingOccurrences(of: "#mmproj", with: "")
        if let error {
            cancelDownload(ModelCatalog.all.first { $0.id == specID } ?? selected)
            downloadError = error
        } else if tasks[specID] == nil && tasks[specID + "#mmproj"] == nil {
            taskBytes[specID] = nil; taskBytes[specID + "#mmproj"] = nil
            downloadProgress[specID] = nil
        }
    }
}

extension ModelManager: URLSessionDownloadDelegate {
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                                didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let key = downloadTask.taskDescription, totalBytesExpectedToWrite > 0 else { return }
        Task { @MainActor in self.updateProgress(key: key, done: totalBytesWritten, total: totalBytesExpectedToWrite) }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let key = downloadTask.taskDescription else { return }
        // Move synchronously: `location` is deleted when this method returns.
        let isProjector = key.hasSuffix("#mmproj")
        let spec = ModelCatalog.all.first { $0.id == key.replacingOccurrences(of: "#mmproj", with: "") }
        var moveError: String?
        if let http = downloadTask.response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            moveError = "Download failed (HTTP \(http.statusCode))."
        } else if let spec, let name = isProjector ? spec.mmprojFileName : spec.fileName {
            let dest = ModelManager.modelsDirectory.appendingPathComponent(name)
            do {
                try? FileManager.default.removeItem(at: dest)
                try FileManager.default.moveItem(at: location, to: dest)
                var rv = URLResourceValues()
                rv.isExcludedFromBackup = true
                var d = dest
                try? d.setResourceValues(rv)
            } catch {
                moveError = error.localizedDescription
            }
        }
        Task { @MainActor in self.finished(key: key, error: moveError) }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let key = task.taskDescription, let error, (error as NSError).code != NSURLErrorCancelled else { return }
        Task { @MainActor in self.finished(key: key, error: error.localizedDescription) }
    }
}
