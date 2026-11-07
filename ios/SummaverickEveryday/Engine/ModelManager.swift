// Downloads and keeps track of Summaverick's on-device AI (Gemma 4 E4B: weights + audio projector).
// The person never picks a model: it downloads by itself after install, in a background URLSession,
// resumes after failures, and is checked for size before it is used. Download is the only time the
// app uses the network. Model files are excluded from iCloud backup.
import Foundation
import Network
import Observation
import UIKit

/// One file of the model.
struct ModelPart: Hashable {
    let key: String
    let fileName: String
    let url: URL
    /// Size used for the space check and progress until the server has said the real size.
    let approxBytes: Int64
}

struct ModelSpec: Hashable {
    let id: String
    let weights: ModelPart
    /// Audio projector: lets the model listen to the recording itself (any language mix).
    let projector: ModelPart
    var contextTokens: Int32 = 8192
    var parts: [ModelPart] { [weights, projector] }
}

/// Where the audio projector is downloaded from. Today: the full ggml-org file (has an unused image
/// encoder). After the slim file (tools/model/strip_vision.py) is uploaded, change these two and the
/// file name below; see tools/model/README.md.
enum ModelHost {
    static let projectorURL = URL(string: "https://huggingface.co/ggml-org/gemma-4-E4B-it-GGUF/resolve/main/mmproj-gemma-4-E4B-it-Q8_0.gguf")!
    static let projectorBytes: Int64 = 559_874_816
}

enum ModelCatalog {
    // Google Gemma 4 E4B (Apache 2.0). Hears the audio itself, so it understands Telugu, Hindi and mixed
    // speech. Weights: unsloth Q4_K_M; audio projector: ggml-org Q8_0. Sizes as served on 2026-11-07.
    static let gemma = ModelSpec(
        id: "gemma4-e4b-q4",
        weights: ModelPart(key: "weights", fileName: "gemma-4-E4B-it-Q4_K_M.gguf",
                           url: URL(string: "https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF/resolve/main/gemma-4-E4B-it-Q4_K_M.gguf")!,
                           approxBytes: 4_977_171_584),
        projector: ModelPart(key: "projector", fileName: "mmproj-gemma-4-E4B-it-Q8_0.gguf",
                             url: ModelHost.projectorURL,
                             approxBytes: ModelHost.projectorBytes))

    /// Models earlier versions could download; deleted at launch to free the space.
    static let retiredFilePrefixes = ["Qwen3-", "qwen3-"]
}

/// What the person sees about the AI. No model names.
enum AIStatus: Equatable {
    case ready(bytes: Int64)
    case unsupported
    case downloading(percent: Int, minutesLeft: Int?)
    case waitingForWiFi(percent: Int)
    case waitingForNetwork(percent: Int)
    case noSpace(gigabytes: Int)
    case failed
    case notDownloaded

    var isReady: Bool { if case .ready = self { return true } else { return false } }
    /// 0…1 while a download is under way or waiting.
    var progress: Double? {
        switch self {
        case .downloading(let p, _), .waitingForWiFi(let p), .waitingForNetwork(let p): return Double(p) / 100
        default: return nil
        }
    }
}

extension Notification.Name {
    /// Posted when the last model file has finished downloading and the AI can be used.
    static let aiReady = Notification.Name("sve.aiReady")
}

@MainActor
@Observable
final class ModelManager: NSObject {
    static let shared = ModelManager()
    static let sessionIdentifier = "com.summaverick.everyday.model"
    nonisolated static let deviceSupported = DeviceGate.isSupported(physicalMemory: ProcessInfo.processInfo.physicalMemory)

    let spec = ModelCatalog.gemma

    /// Both files are on disk (each was size-checked before it was moved into place).
    private(set) var isReady = false
    /// Mobile data allowed for the download (default: Wi-Fi only).
    var allowCellular: Bool {
        didSet {
            guard allowCellular != oldValue else { return }
            UserDefaults.standard.set(allowCellular, forKey: Keys.cellular)
            restartSession()
        }
    }

    private enum Phase: Equatable { case idle, downloading, failed, noSpace(Int) }
    private var phase: Phase = .idle
    private var bytes: [String: (done: Int64, total: Int64)] = [:]
    private var wifi = true
    private var online = true
    private var speed: Double = 0                     // bytes per second, smoothed
    @ObservationIgnored private var speedSample: (time: Date, done: Int64)?

    @ObservationIgnored private var session: URLSession?
    @ObservationIgnored private var tasks: [String: URLSessionDownloadTask] = [:]
    @ObservationIgnored private var activated = false
    @ObservationIgnored private var attached = false
    @ObservationIgnored private var restarting = false
    @ObservationIgnored private var failures = 0
    @ObservationIgnored private var retryTask: Task<Void, Never>?
    /// Tasks started from scratch (no resume data): replaced if resume data for that file turns up late.
    @ObservationIgnored private var freshTaskIDs: Set<Int> = []
    @ObservationIgnored private nonisolated let throttle = ProgressThrottle()
    @ObservationIgnored private let pathMonitor = NWPathMonitor()
    /// Handed over by the app delegate when iOS wakes the app for finished background downloads.
    @ObservationIgnored var backgroundCompletionHandler: (() -> Void)?

    private enum Keys {
        static let cellular = "model.cellular"
        static let removed = "model.userRemoved"
        static func expected(_ key: String) -> String { "model.expected.\(key)" }
    }

    private override init() {
        allowCellular = UserDefaults.standard.bool(forKey: Keys.cellular)
        super.init()
        isReady = Self.deviceSupported && spec.parts.allSatisfy { present($0) }
    }

    // MARK: files

    nonisolated static var modelsDirectory: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Summaverick/models", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    nonisolated static func fileURL(_ part: ModelPart) -> URL { modelsDirectory.appendingPathComponent(part.fileName) }
    nonisolated static func resumeURL(_ part: ModelPart) -> URL { modelsDirectory.appendingPathComponent(part.fileName + ".resume") }

    nonisolated private static func size(_ url: URL) -> Int64? {
        (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int64
    }

    private func present(_ part: ModelPart) -> Bool { (Self.size(Self.fileURL(part)) ?? 0) > 10_000_000 }

    private func refreshReady() {
        // A phone below the memory line never uses the model, even one left from an older version.
        let ready = Self.deviceSupported && spec.parts.allSatisfy { present($0) }
        let became = ready && !isReady
        isReady = ready
        if became {
            failures = 0
            phase = .idle
            for t in tasks.values { t.cancel() }   // a duplicate left from a relaunch
            tasks.removeAll()
            NotificationCenter.default.post(name: .aiReady, object: nil)
        }
    }

    /// Some model file is on disk (also on an unsupported phone, from an older version), so
    /// "Remove to free space" has something to remove.
    var hasFiles: Bool { spec.parts.contains { present($0) } }

    /// Loads the model into the core for the next run (no-op when it is already loaded on the right
    /// backend). Transcription runs on the CPU; the rest on the GPU only while the app is in front and
    /// not recording (BackendChoice). Returns the backend used.
    @discardableResult
    func loadIntoCore(for purpose: ModelPurpose = .generation) async throws -> ModelBackend {
        // .inactive (Control Center, a banner, the app switcher) still counts as in front: switching would
        // cost two ~5 s reloads; GPU work is only refused once the app is in the background.
        let backend = BackendChoice.backend(for: purpose, appActive: UIApplication.shared.applicationState != .background,
                                            recording: LiveTranscriber.shared.isRecording)
        try await loadIntoCore(backend: backend)
        return backend
    }

    func loadIntoCore(backend: ModelBackend) async throws {
        try await CoreEngine.shared.loadModel(at: Self.fileURL(spec.weights).path, contextTokens: spec.contextTokens,
                                              mmprojPath: Self.fileURL(spec.projector).path, backend: backend)
    }

    // MARK: status

    var status: AIStatus {
        if !Self.deviceSupported { return .unsupported }
        if isReady {
            return .ready(bytes: spec.parts.reduce(Int64(0)) { $0 + (Self.size(Self.fileURL($1)) ?? 0) })
        }
        let percent = min(99, Int(DownloadMath.progress(partSizes) * 100))
        switch phase {
        case .noSpace(let gb): return .noSpace(gigabytes: gb)
        case .failed: return .failed
        case .idle where tasks.isEmpty && retryTask == nil:
            return UserDefaults.standard.bool(forKey: Keys.removed) ? .notDownloaded : .downloading(percent: percent, minutesLeft: nil)
        default:
            if !online { return allowCellular ? .waitingForNetwork(percent: percent) : .waitingForWiFi(percent: percent) }
            if !allowCellular && !wifi { return .waitingForWiFi(percent: percent) }
            let left = DownloadMath.minutesLeft(remainingBytes: DownloadMath.remaining(partSizes), bytesPerSecond: speed)
            return .downloading(percent: percent, minutesLeft: left)
        }
    }

    /// done/total per file: finished files count whole, running ones as reported, others not started.
    private var partSizes: [(done: Int64, total: Int64)] {
        spec.parts.map { part in
            if present(part) { let s = Self.size(Self.fileURL(part)) ?? part.approxBytes; return (s, s) }
            let total = bytes[part.key]?.total ?? recordedSize(part) ?? part.approxBytes
            return (bytes[part.key]?.done ?? 0, total)
        }
    }

    private func recordedSize(_ part: ModelPart) -> Int64? {
        let v = UserDefaults.standard.object(forKey: Keys.expected(part.key)) as? Int64
        return (v ?? 0) > 0 ? v : nil
    }

    // MARK: lifecycle

    /// Call once at launch: removes retired models, reconnects to downloads that ran while the app was
    /// closed, then starts what is missing.
    func activate() {
        guard !activated else { return }
        activated = true
        removeRetiredModels()
        pathMonitor.pathUpdateHandler = { [weak self] path in
            let ok = path.status == .satisfied
            let wifi = ok && (path.usesInterfaceType(.wifi) || path.usesInterfaceType(.wiredEthernet))
            Task { @MainActor in
                self?.online = ok
                self?.wifi = wifi
            }
        }
        pathMonitor.start(queue: .main)
        attach()
    }

    /// Qwen came with earlier versions; the app now uses only Gemma 4, so free that space.
    private func removeRetiredModels() {
        let dir = Self.modelsDirectory
        let files = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        for f in files where ModelCatalog.retiredFilePrefixes.contains(where: { f.hasPrefix($0) }) {
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(f))
            NSLog("[sve] removed retired model %@", f)
        }
        UserDefaults.standard.removeObject(forKey: "model.selected")
    }

    private func makeSession() -> URLSession {
        let cfg = URLSessionConfiguration.background(withIdentifier: Self.sessionIdentifier)
        cfg.isDiscretionary = false
        cfg.sessionSendsLaunchEvents = true
        cfg.allowsCellularAccess = allowCellular
        let s = URLSession(configuration: cfg, delegate: self, delegateQueue: nil)
        session = s
        return s
    }

    /// Picks up tasks the system kept running for us, then starts whatever is still missing.
    private func attach() {
        attached = false
        let s = session ?? makeSession()
        s.getAllTasks { all in
            let found = all.compactMap { $0 as? URLSessionDownloadTask }
                .filter { $0.state == .running || $0.state == .suspended }
            Task { @MainActor in
                for t in found {
                    guard let key = t.taskDescription, self.spec.parts.contains(where: { $0.key == key }),
                          self.tasks[key] == nil else { t.cancel(); continue }
                    self.tasks[key] = t
                    if t.countOfBytesExpectedToReceive > 0 {
                        self.bytes[key] = (t.countOfBytesReceived, t.countOfBytesExpectedToReceive)
                    }
                    if t.state == .suspended { t.resume() }
                }
                if !self.tasks.isEmpty { self.phase = .downloading }
                // Results for tasks that ended while the app was closed (with their resume data) are
                // delivered separately and in no fixed order; give them a moment before starting
                // anything new, so a download resumes instead of starting over.
                try? await Task.sleep(for: .seconds(2))
                self.attached = true
                self.startIfNeeded()
            }
        }
    }

    /// Starts the download when a file is missing, the phone is supported and there is space.
    /// `userInitiated`: "Download again" / "Try again", which also clears an earlier "Remove".
    func startIfNeeded(userInitiated: Bool = false) {
        guard Self.deviceSupported else { return }
        refreshReady()
        guard !isReady else { return }
        let d = UserDefaults.standard
        if userInitiated {
            d.set(false, forKey: Keys.removed)
            failures = 0
            retryTask?.cancel(); retryTask = nil
        } else if d.bool(forKey: Keys.removed) {
            return
        }
        guard attached, !restarting else { return }   // attach() / sessionGone() call this again

        let missing = spec.parts.filter { !present($0) && tasks[$0.key] == nil }
        guard !missing.isEmpty else { return }

        let remaining = DownloadMath.remaining(partSizes)
        let available = (try? URL(fileURLWithPath: NSHomeDirectory())
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage
        guard SpaceCheck.hasRoom(available: available, remaining: remaining) else {
            phase = .noSpace(SpaceCheck.gigabytesToFree(available: available, remaining: remaining))
            NSLog("[sve] model download needs space: %lld free, %lld needed", available ?? -1, remaining)
            return
        }

        let s = session ?? makeSession()
        phase = .downloading
        speedSample = nil
        for part in missing {
            let task: URLSessionDownloadTask
            if let data = try? Data(contentsOf: Self.resumeURL(part)) {
                task = s.downloadTask(withResumeData: data)
                try? FileManager.default.removeItem(at: Self.resumeURL(part))
            } else {
                task = s.downloadTask(with: part.url)
                freshTaskIDs.insert(task.taskIdentifier)
            }
            task.taskDescription = part.key
            tasks[part.key] = task
            task.resume()
        }
    }

    /// The mobile-data choice is a session setting: stop the tasks keeping their resume data, then
    /// start again in a new session once the old one is gone (didBecomeInvalidWithError).
    private func restartSession() {
        guard let s = session else { return }   // the next session is made with the new setting
        restarting = true
        let running = Array(tasks.values)
        tasks.removeAll()
        let group = DispatchGroup()
        for t in running {
            group.enter()
            t.cancel { data in
                if let data, let key = t.taskDescription, let part = ModelCatalog.gemma.parts.first(where: { $0.key == key }) {
                    try? data.write(to: Self.resumeURL(part), options: .atomic)
                }
                group.leave()
            }
        }
        group.notify(queue: .main) { s.invalidateAndCancel() }
    }

    /// "Remove to free space": stops any download and deletes the files. It is not downloaded again
    /// until the person taps "Download again".
    func remove() async {
        await CoreEngine.shared.unloadModel()
        UserDefaults.standard.set(true, forKey: Keys.removed)
        retryTask?.cancel(); retryTask = nil
        for t in tasks.values { t.cancel() }
        tasks.removeAll()
        bytes.removeAll()
        for part in spec.parts {
            try? FileManager.default.removeItem(at: Self.fileURL(part))
            try? FileManager.default.removeItem(at: Self.resumeURL(part))
        }
        phase = .idle
        refreshReady()
    }

    // MARK: delegate results (main actor)

    private func progressed(key: String, done: Int64, total: Int64) {
        bytes[key] = (done, total)
        if let part = spec.parts.first(where: { $0.key == key }), recordedSize(part) != total {
            UserDefaults.standard.set(total, forKey: Keys.expected(key))
        }
        let all = bytes.values.reduce(Int64(0)) { $0 + $1.done }
        let now = Date()
        if let s = speedSample {
            let dt = now.timeIntervalSince(s.time)
            if dt >= 5 {
                let r = Double(all - s.done) / dt
                if r >= 0 { speed = speed > 0 ? speed * 0.7 + r * 0.3 : r }
                speedSample = (now, all)
            }
        } else {
            speedSample = (now, all)
        }
    }

    /// Results name the task they came from: after a relaunch a late result for an old task must not
    /// forget the task that is running now.
    private func forget(_ key: String, taskID: Int) {
        if tasks[key]?.taskIdentifier == taskID { tasks[key] = nil }
        freshTaskIDs.remove(taskID)
    }

    private func finished(key: String, taskID: Int) {
        forget(key, taskID: taskID)
        bytes[key] = nil
        if let part = spec.parts.first(where: { $0.key == key }) { try? FileManager.default.removeItem(at: Self.resumeURL(part)) }
        refreshReady()
        if !isReady && tasks.isEmpty && retryTask == nil { startIfNeeded() }
    }

    /// `cancelled`: stopped by us (Remove, mobile-data change). `systemCancelled`: stopped by iOS, for
    /// example when the app was force-quit; resumes without counting as a failure.
    private func failed(key: String, taskID: Int, message: String, resumeData: Data?, cancelled: Bool, systemCancelled: Bool = false) {
        let current = tasks[key]?.taskIdentifier == taskID
        forget(key, taskID: taskID)
        if let part = spec.parts.first(where: { $0.key == key }) {
            if let resumeData {
                if !current, let running = tasks[key] {
                    // Late resume data for a file that is already downloading again.
                    guard freshTaskIDs.contains(running.taskIdentifier) else { return }   // that one resumed: keep it
                    // It started over from 0: swap it for the resumed download.
                    NSLog("[sve] model download %@: late resume data, resuming instead of starting over", key)
                    tasks[key] = nil
                    freshTaskIDs.remove(running.taskIdentifier)
                    bytes[key] = nil
                    running.cancel()
                    try? resumeData.write(to: Self.resumeURL(part), options: .atomic)
                    startIfNeeded()
                    return
                }
                try? resumeData.write(to: Self.resumeURL(part), options: .atomic)
            } else if !cancelled && !systemCancelled {
                try? FileManager.default.removeItem(at: Self.resumeURL(part))   // cannot resume: start over
                bytes[key] = nil
            }
        }
        if systemCancelled {
            NSLog("[sve] model download %@ stopped by the system; resuming in 10 s", key)
            if retryTask == nil {
                retryTask = Task { [weak self] in
                    try? await Task.sleep(for: .seconds(10))
                    guard !Task.isCancelled, let self else { return }
                    self.retryTask = nil
                    self.startIfNeeded()
                }
            }
            return
        }
        if cancelled { return }
        NSLog("[sve] model download %@ failed: %@", key, message)
        guard retryTask == nil else { return }   // the other file failed at the same moment
        failures += 1
        guard let delay = RetrySchedule.delay(afterFailures: failures) else {
            phase = .failed
            return
        }
        retryTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self else { return }
            self.retryTask = nil
            self.startIfNeeded()
        }
    }

    private func sessionGone() {
        session = nil
        restarting = false
        attach()
    }

    fileprivate func finishBackgroundEvents() {
        backgroundCompletionHandler?()
        backgroundCompletionHandler = nil
    }
}

extension ModelManager: URLSessionDownloadDelegate {
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                                didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let key = downloadTask.taskDescription, totalBytesExpectedToWrite > 0,
              throttle.allow(key, final: totalBytesWritten >= totalBytesExpectedToWrite) else { return }
        Task { @MainActor in self.progressed(key: key, done: totalBytesWritten, total: totalBytesExpectedToWrite) }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                                didResumeAtOffset fileOffset: Int64, expectedTotalBytes: Int64) {
        guard let key = downloadTask.taskDescription, expectedTotalBytes > 0 else { return }
        Task { @MainActor in self.progressed(key: key, done: fileOffset, total: expectedTotalBytes) }
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let key = downloadTask.taskDescription,
              let part = ModelCatalog.gemma.parts.first(where: { $0.key == key }) else { return }
        // Move synchronously: `location` is deleted when this method returns.
        let http = downloadTask.response as? HTTPURLResponse
        let status = http?.statusCode ?? 0
        var problem: String?
        if !(200..<300).contains(status) {
            problem = "HTTP \(status)"
        } else {
            let actual = Self.size(location) ?? 0
            let recorded = UserDefaults.standard.object(forKey: "model.expected.\(key)") as? Int64
            let expected = SizeCheck.expectedSize(status: status, contentLength: http?.expectedContentLength ?? -1,
                                                  contentRange: http?.value(forHTTPHeaderField: "Content-Range"),
                                                  recorded: recorded)
            if !SizeCheck.isComplete(actual: actual, expected: expected) {
                problem = "size \(actual) != expected \(expected ?? -1)"
            } else {
                let dest = Self.fileURL(part)
                do {
                    try? FileManager.default.removeItem(at: dest)
                    try FileManager.default.moveItem(at: location, to: dest)
                    var rv = URLResourceValues()
                    rv.isExcludedFromBackup = true
                    var d = dest
                    try? d.setResourceValues(rv)
                } catch {
                    problem = error.localizedDescription
                }
            }
        }
        let id = downloadTask.taskIdentifier
        if let problem {
            // A wrong or broken file: never resume from it.
            Task { @MainActor in self.failed(key: key, taskID: id, message: problem, resumeData: nil, cancelled: false) }
        } else {
            Task { @MainActor in self.finished(key: key, taskID: id) }
        }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let key = task.taskDescription, let error else { return }
        let ns = error as NSError
        let resume = ns.userInfo[NSURLSessionDownloadTaskResumeData] as? Data
        let cancelled = ns.code == NSURLErrorCancelled
        // Set when iOS (not the app) stopped a background transfer, e.g. the app was force-quit.
        let bySystem = cancelled && ns.userInfo[NSURLErrorBackgroundTaskCancelledReasonKey] != nil
        let id = task.taskIdentifier
        Task { @MainActor in
            self.failed(key: key, taskID: id, message: error.localizedDescription, resumeData: resume,
                        cancelled: cancelled && !bySystem, systemCancelled: bySystem)
        }
    }

    nonisolated func urlSession(_ session: URLSession, didBecomeInvalidWithError error: Error?) {
        Task { @MainActor in self.sessionGone() }
    }

    nonisolated func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        Task { @MainActor in self.finishBackgroundEvents() }
    }
}

/// Lets through at most about two progress updates a second per file (plus the last one).
final class ProgressThrottle: @unchecked Sendable {
    private let lock = NSLock()
    private var last: [String: TimeInterval] = [:]
    func allow(_ key: String, final: Bool) -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock(); defer { lock.unlock() }
        guard final || now - (last[key] ?? 0) >= 0.5 else { return false }
        last[key] = now
        return true
    }
}

/// Receives iOS's wake-up for finished background downloads.
final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        guard identifier == ModelManager.sessionIdentifier else { completionHandler(); return }
        MainActor.assumeIsolated {
            ModelManager.shared.backgroundCompletionHandler = completionHandler
            ModelManager.shared.activate()
        }
    }
}
