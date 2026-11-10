// Swift wrapper over the portable C core (sve_core.h).
// Everything here is thin: the real logic lives in core/, so it moves to Android and the device unchanged.
import Foundation

enum OutputKind: Int, CaseIterable, Identifiable {
    case shortSummary = 0, detailedNotes = 1, revisionQuestions = 2
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .shortSummary: return String(localized: "Summary")
        case .detailedNotes: return String(localized: "Notes")
        case .revisionQuestions: return String(localized: "Questions")
        }
    }
    var cKind: sve_output_kind { sve_output_kind(UInt32(rawValue)) }
}

struct NoteSummary: Identifiable, Hashable, Decodable {
    let id: Int64
    let title: String
    let lang: String
    let created_at: Int64
    let duration_seconds: Int64
    let has_audio: Bool
    let preview: String
    var createdAt: Date { Date(timeIntervalSince1970: TimeInterval(created_at)) }
}

struct Note: Identifiable, Decodable {
    let id: Int64
    var title: String
    let lang: String
    let created_at: Int64
    let duration_seconds: Int64
    var audio_path: String?
    var transcript: String?
    var short_summary: String?
    var detailed_notes: String?
    var revision_questions: String?
    var createdAt: Date { Date(timeIntervalSince1970: TimeInterval(created_at)) }

    func output(_ kind: OutputKind) -> String? {
        switch kind {
        case .shortSummary: return short_summary
        case .detailedNotes: return detailed_notes
        case .revisionQuestions: return revision_questions
        }
    }
}

struct AudioRetentionRow: Decodable {
    let id: Int64
    let audio_path: String
}

enum CoreError: LocalizedError {
    case initFailed, modelLoadFailed(String), generationFailed, notFound, audioModelNotLoaded, transcriptionFailed
    var errorDescription: String? {
        switch self {
        case .initFailed: return String(localized: "I couldn't open your notes.")
        case .modelLoadFailed: return String(localized: "I couldn't start up. Please try again.")
        case .generationFailed: return String(localized: "I couldn't come up with an answer this time.")
        case .notFound: return String(localized: "Note not found.")
        case .audioModelNotLoaded: return String(localized: "I can't listen to audio right now.")
        case .transcriptionFailed: return String(localized: "I couldn't write down what was said in this recording.")
        }
    }
}

/// Serialises all access to the model's sve_ctx. Call from anywhere; work happens off the main thread.
/// Store-only calls (notes, settings, assistant rows, memories: no model) are `nonisolated` and run on
/// CoreStore's own sve_ctx instead, so a screen's read or write never waits behind a model call that holds
/// this actor for minutes. Both contexts open the same SQLite file (WAL, busy timeout in the core); neither
/// caches rows, so each sees the other's committed writes.
actor CoreEngine {
    static let shared = CoreEngine()

    var ctx: OpaquePointer?
    private(set) var loadedModelPath: String?

    static var databaseURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Summaverick", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("notes.sqlite")
    }

    /// The core's log goes to NSLog; set once, by whichever context opens first.
    static let logInstalled: Void = {
        sve_set_log({ level, msg, _ in
            if let msg { NSLog("[sve %d] %@", level, String(cString: msg)) }
        }, nil)
    }()

    private init() {
        _ = CoreEngine.logInstalled
        ctx = sve_init(CoreEngine.databaseURL.path)
        if ctx == nil { NSLog("sve_init failed") }
    }

    /// Runs a store-only call on CoreStore's context (never behind a model call on this actor).
    nonisolated func store<T>(_ body: @Sendable (OpaquePointer?) throws -> T) async rethrows -> T {
        try await CoreStore.shared.run(body)
    }

    nonisolated func take(_ p: UnsafeMutablePointer<CChar>?) -> String? {
        guard let p else { return nil }
        defer { sve_free_string(p) }
        return String(cString: p)
    }

    nonisolated func decode<T: Decodable>(_ type: T.Type, _ json: String?) -> T? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    // MARK: model

    var isModelLoaded: Bool { sve_llm_loaded(ctx) != 0 }

    /// Where the loaded model runs; the audio projector follows it.
    private(set) var loadedBackend: ModelBackend?
    /// What the last loadModel asked for, so a run can switch backend itself (ensureBackend).
    private var lastLoad: (path: String, contextTokens: Int32, mmprojPath: String?)?

    /// Loads the model (and projector) on `backend`. A no-op when that is what is loaded; reloads
    /// (about 5 s) when the backend differs. Call between model runs, never during one.
    func loadModel(at path: String, contextTokens: Int32 = 4096, mmprojPath: String? = nil, backend: ModelBackend = .gpu) throws {
        guard let ctx else { throw CoreError.initFailed }
        lastLoad = (path, contextTokens, mmprojPath)
        // After a failed GPU run the Metal state stays broken (owner's iPhone: every later run failed at once with
        // "command buffer 0 failed with status 5", in front too), so the same backend is loaded again.
        let broken = sve_llm_needs_reload(ctx) != 0
        if broken { NSLog("[sve] a GPU run failed; loading the model again") }
        if !broken, loadedModelPath == path, loadedBackend == backend, sve_llm_loaded(ctx) != 0,
           mmprojPath == nil || sve_audio_loaded(ctx) != 0 { applyHeatThreads(); return }
        cpuThreads = nil
        if sve_load_llm(ctx, path, contextTokens, backend.threads, backend.gpuLayers) != 0 {
            loadedModelPath = nil
            loadedBackend = nil
            throw CoreError.modelLoadFailed(path)
        }
        loadedModelPath = path
        loadedBackend = backend
        NSLog("[sve] model loaded on the %@", backend == .gpu ? "GPU" : "CPU")
        applyHeatThreads()   // before the projector: it loads with the batch threads, so it needs no second load
        if let mmprojPath, sve_load_audio(ctx, mmprojPath) != 0 {
            throw CoreError.modelLoadFailed(mmprojPath)
        }
        // The heat ladder's memory input is log-only until measured on the phone (R-N6-b).
        NSLog("[sve] heat: memory after load %d MB", Int32(clamping: HeatGovernor.availableMemoryMB))
    }

    /// CPU threads set on the loaded CPU model since its load; nil: the load-time ones (the heat ladder's full).
    private var cpuThreads: (decode: Int32, batch: Int32)?

    /// Every model run (and every live piece) passes through loadModel first: on the CPU it takes the threads
    /// HeatGovernor wants (the heat ladder's, capped in the background: ThreadChoice), and only when they
    /// changed; the audio encoder follows the batch count (the core loads it again only when that differs).
    /// A run already going keeps its threads to its end. GPU runs keep the GPU and only rest.
    private func applyHeatThreads() {
        guard loadedBackend == .cpu, let ctx, sve_llm_loaded(ctx) != 0 else { return }
        let want = HeatGovernor.threads.wanted
        if want != (cpuThreads ?? (ModelBackend.cpu.threads, 0)), sve_set_threads(ctx, want.decode, want.batch) == 0 {
            cpuThreads = want
            NSLog("[sve] threads: CPU %d decode, %d batch (0: the core's rule)", want.decode, want.batch)
        }
        if sve_audio_loaded(ctx) != 0, sve_set_audio_threads(ctx, 0) != 0 {
            NSLog("[sve] threads: the audio encoder could not take the new count")
        }
    }

    /// A GPU run failed since the load: the next loadModel loads it again.
    var modelNeedsReload: Bool { sve_llm_needs_reload(ctx) != 0 }

    var isAudioLoaded: Bool { sve_audio_loaded(ctx) != 0 }

    /// Inside a run: makes sure the model is on `backend` (same files as the last load), so the choice and
    /// the run cannot be split by another caller reloading in between.
    func ensureBackend(_ backend: ModelBackend) throws {
        guard let l = lastLoad else { throw CoreError.audioModelNotLoaded }
        try loadModel(at: l.path, contextTokens: l.contextTokens, mmprojPath: l.mmprojPath, backend: backend)
    }

    /// Transcribes a 16 kHz WAV with the loaded audio-capable model. progress: (done pieces, total pieces).
    func transcribeAudio(path: String, onProgress: @escaping (Int, Int) -> Void = { _, _ in }) throws -> String {
        guard let ctx else { throw CoreError.initFailed }
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = ProgressBox(onProgress: { _, d, t in onProgress(d, t) }, isCancelled: { Task.isCancelled })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_transcribe_audio(ctx, path, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.transcriptionFailed
        }
        return out
    }

    func unloadModel() {
        sve_unload_llm(ctx)
        loadedModelPath = nil
        loadedBackend = nil
        cpuThreads = nil
    }

    // MARK: generation

    /// Progress box passed through the C callback as an opaque pointer.
    final class ProgressBox {
        let onProgress: (String, Int, Int) -> Void
        let isCancelled: () -> Bool
        init(onProgress: @escaping (String, Int, Int) -> Void, isCancelled: @escaping () -> Bool) {
            self.onProgress = onProgress; self.isCancelled = isCancelled
        }
    }

    static let progressTrampoline: sve_progress_cb = { phase, done, total, user in
        guard let user else { return 0 }
        let box = Unmanaged<ProgressBox>.fromOpaque(user).takeUnretainedValue()
        box.onProgress(phase.map { String(cString: $0) } ?? "", Int(done), Int(total))
        return box.isCancelled() ? 1 : 0
    }

    func generate(transcript: String, kind: OutputKind, lang: String, shouldStop: @escaping () -> Bool = { false },
                  onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> String {
        guard let ctx else { throw CoreError.initFailed }
        let box = ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_generate(ctx, transcript, kind.cKind, lang, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        return out
    }

    func ask(context: String, question: String, lang: String) throws -> String {
        guard let ctx else { throw CoreError.initFailed }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_ask(ctx, context, question, lang, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        return out
    }

    /// Whether `generate` takes the whole transcript in one prompt (no chunking). nil with no model.
    func transcriptFits(_ transcript: String, lang: String) -> Bool? {
        guard let ctx else { return nil }
        let r = sve_transcript_fits(ctx, transcript, lang)
        return r < 0 ? nil : r == 1
    }

    func suggestTitle(transcript: String, lang: String) -> String? {
        guard let ctx else { return nil }
        let t = take(sve_suggest_title(ctx, transcript, lang))?.trimmingCharacters(in: .whitespacesAndNewlines)
        return (t?.isEmpty ?? true) ? nil : t
    }

    // MARK: notes (store context)

    nonisolated func createNote(title: String, lang: String, audioPath: String?, transcript: String?, durationSeconds: Int64) async -> Int64 {
        await store { sve_note_create($0, title, lang, audioPath, transcript, durationSeconds) }
    }

    nonisolated func setTranscript(_ id: Int64, _ transcript: String) async { await store { _ = sve_note_set_transcript($0, id, transcript) } }
    nonisolated func setOutput(_ id: Int64, _ kind: OutputKind, _ text: String) async {
        await store { _ = sve_note_set_output($0, id, kind.cKind, text) }
    }
    nonisolated func setTitle(_ id: Int64, _ title: String) async { await store { _ = sve_note_set_title($0, id, title) } }
    nonisolated func deleteNote(_ id: Int64) async { await store { _ = sve_note_delete($0, id) } }
    nonisolated func clearAudio(_ id: Int64) async { await store { _ = sve_note_clear_audio($0, id) } }
    nonisolated func setAudioPath(_ id: Int64, _ path: String) async -> Bool { await store { sve_note_set_audio_path($0, id, path) == 0 } }
    nonisolated func clearTranscript(_ id: Int64) async { await store { _ = sve_note_clear_transcript($0, id) } }

    nonisolated func note(_ id: Int64) async -> Note? { await store { self.decode(Note.self, self.take(sve_note_get_json($0, id))) } }

    nonisolated func notes(query: String = "", limit: Int32 = 200) async -> [NoteSummary] {
        await store { self.decode([NoteSummary].self, self.take(sve_notes_list_json($0, query, limit))) ?? [] }
    }

    nonisolated func audioOlderThan(days: Int32) async -> [AudioRetentionRow] {
        await store { self.decode([AudioRetentionRow].self, self.take(sve_notes_audio_older_than_json($0, days))) ?? [] }
    }

    // MARK: settings (store context)

    nonisolated func setting(_ key: String) async -> String? { await store { self.take(sve_setting_get($0, key)) } }
    nonisolated func setSetting(_ key: String, _ value: String) async { await store { _ = sve_setting_set($0, key, value) } }
}

/// The second sve_ctx on the notes database, with no model: every store-only call of CoreEngine runs here, one
/// at a time (the core's rule: one thread per context). Model calls stay on CoreEngine's own context. Writes from
/// both contexts are serialised by SQLite (a writer waits for the other's write to commit).
actor CoreStore {
    static let shared = CoreStore()

    private let ctx: OpaquePointer?

    private init() {
        _ = CoreEngine.logInstalled
        ctx = sve_init(CoreEngine.databaseURL.path)
        if ctx == nil { NSLog("sve_init (store context) failed") }
    }

    func run<T>(_ body: (OpaquePointer?) throws -> T) rethrows -> T { try body(ctx) }
}
