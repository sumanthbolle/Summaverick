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
    /// The backend of the last loadModel, so ensureGemmaLoaded loads Gemma back as its caller had it.
    private var lastGemmaBackend: ModelBackend?
    /// Whether Whisper freed Gemma since Gemma's last load (ensureGemmaLoaded).
    private var slot = ModelSlot()

    /// Loads the model (and projector) on `backend`. A no-op when that is what is loaded; reloads
    /// (about 5 s) when the backend differs. Call between model runs, never during one.
    func loadModel(at path: String, contextTokens: Int32 = 4096, mmprojPath: String? = nil, backend: ModelBackend = .gpu) throws {
        guard let ctx else { throw CoreError.initFailed }
        // One model at a time (spec section 3): Whisper is freed before Gemma loads, here for every caller.
        let swapStart: Date? = sve_asr_loaded(ctx) != 0 ? Date() : nil
        unloadWhisper()
        lastLoad = (path, contextTokens, mmprojPath)
        lastGemmaBackend = backend
        // After a failed GPU run the Metal state stays broken (owner's iPhone: every later run failed at once with
        // "command buffer 0 failed with status 5", in front too), so the same backend is loaded again.
        let broken = sve_llm_needs_reload(ctx) != 0
        if broken { NSLog("[sve] a GPU run failed; loading the model again") }
        if !broken, loadedModelPath == path, loadedBackend == backend, sve_llm_loaded(ctx) != 0,
           mmprojPath == nil || sve_audio_loaded(ctx) != 0 { slot.gemmaLoaded(); applyHeatThreads(); return }
        cpuThreads = nil
        let pair = CoreEngine.pairWriting
        sve_set_pair_enabled(ctx, pair ? 1 : 0)   // before the load: off loads the model with one sequence
        NSLog("[sve] pair writing %@", pair ? "on" : "off")
        if sve_load_llm(ctx, path, contextTokens, backend.threads, backend.gpuLayers) != 0 {
            loadedModelPath = nil
            loadedBackend = nil
            throw CoreError.modelLoadFailed(path)
        }
        loadedModelPath = path
        loadedBackend = backend
        slot.gemmaLoaded()
        NSLog("[sve] model loaded on the %@", backend == .gpu ? "GPU" : "CPU")
        applyHeatThreads()   // before the projector: it loads with the batch threads, so it needs no second load
        if let mmprojPath, sve_load_audio(ctx, mmprojPath) != 0 {
            throw CoreError.modelLoadFailed(mmprojPath)
        }
        if let swapStart { NSLog("[sve] model swap: whisper→gemma %.1fs", Date().timeIntervalSince(swapStart)) }
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

    /// First thing in every call that runs Gemma, in the same actor turn as the run: when Whisper freed Gemma since
    /// its caller loaded it (the caller's load and run are separate turns, and runWhisper's next piece may come in
    /// between), Gemma is loaded again as last asked, which frees Whisper and logs the swap. A no-op when Gemma is
    /// loaded, was never loaded, or was freed on purpose. Internal only because the Gemma calls span three files.
    func ensureGemmaLoaded() throws {
        guard slot.needsGemmaReload(gemmaLoaded: sve_llm_loaded(ctx) != 0),
              let l = lastLoad, let b = lastGemmaBackend else { return }
        NSLog("[sve] Gemma was freed for Whisper; loading it again for this run")
        try loadModel(at: l.path, contextTokens: l.contextTokens, mmprojPath: l.mmprojPath, backend: b)
    }

    /// Transcribes a 16 kHz WAV with the loaded audio-capable model. progress: (done pieces, total pieces).
    func transcribeAudio(path: String, onProgress: @escaping (Int, Int) -> Void = { _, _ in }) throws -> String {
        guard let ctx else { throw CoreError.initFailed }
        try ensureGemmaLoaded()
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = ProgressBox(onProgress: { _, d, t in onProgress(d, t) }, isCancelled: { Task.isCancelled })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_transcribe_audio(ctx, path, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.transcriptionFailed
        }
        return out
    }

    /// Frees Gemma on purpose: it is not loaded again by itself (ensureGemmaLoaded).
    func unloadModel() {
        freeGemma()
        slot.gemmaUnloaded()
    }

    private func freeGemma() {
        sve_unload_llm(ctx)
        loadedModelPath = nil
        loadedBackend = nil
        cpuThreads = nil
    }

    // MARK: Whisper (speech to text; never in memory together with Gemma)

    /// What the last loadWhisper asked for, so a piece can load Whisper again when Ask or a recording freed it.
    private var lastWhisper: (path: String, backend: ModelBackend)?
    private var loadedWhisperBackend: ModelBackend?

    var isWhisperLoaded: Bool { sve_asr_loaded(ctx) != 0 }

    /// Loads Whisper on `backend`, freeing Gemma first (the one-model rule). A no-op when it is already loaded
    /// there with Gemma out. Call between runs, never during one.
    func loadWhisper(path: String, backend: ModelBackend) throws {
        guard let ctx else { throw CoreError.initFailed }
        lastWhisper = (path, backend)
        if sve_asr_loaded(ctx) != 0, loadedWhisperBackend == backend, sve_llm_loaded(ctx) == 0 { return }
        let start = Date()
        let swap = sve_llm_loaded(ctx) != 0
        slot.whisperLoading(gemmaWasLoaded: swap)   // its next run loads it back (ensureGemmaLoaded)
        freeGemma()
        if sve_load_asr(ctx, path, backend == .gpu ? 1 : 0, backend.threads) != 0 {
            loadedWhisperBackend = nil
            throw CoreError.modelLoadFailed(path)
        }
        loadedWhisperBackend = backend
        let secs = Date().timeIntervalSince(start)
        NSLog("[sve] whisper loaded on the %@ in %.1f s", backend == .gpu ? "GPU" : "CPU", secs)
        if swap { NSLog("[sve] model swap: gemma→whisper %.1fs", secs) }
        NSLog("[sve] heat: memory after whisper load %d MB", Int32(clamping: HeatGovernor.availableMemoryMB))
    }

    func unloadWhisper() {
        guard sve_asr_loaded(ctx) != 0 else { return }
        sve_unload_asr(ctx)
        loadedWhisperBackend = nil
        NSLog("[sve] whisper freed")
    }

    /// One step of Whisper's transcript (sve_whisper_next_piece).
    enum WhisperPiece: Equatable {
        /// [start, endSample) heard; needsGemma: not clearly Telugu, so Gemma transcribes it (text "").
        case piece(startSec: Int, endSample: Int64, text: String, conf: Double?, needsGemma: Bool)
        case wait
        case end
    }

    private struct WhisperPieceJSON: Decodable {
        let status: String
        let start_sec: Int?
        let end_sample: Int64?
        let text: String?
        let conf: Double?
        let needs_gemma: Bool?
    }

    /// The next group of Whisper pieces from `startSample` of the WAV at `path`. Loads Whisper again first (in the
    /// same actor turn) when another caller freed it since loadWhisper. Throws on failure; cancels (throws) when
    /// `shouldStop` turns true during the run.
    func whisperNextPiece(path: String, startSample: Int64, isFinal: Bool,
                          shouldStop: @escaping () -> Bool = { false }) throws -> WhisperPiece {
        guard let ctx else { throw CoreError.initFailed }
        if sve_asr_loaded(ctx) == 0 || sve_llm_loaded(ctx) != 0 {
            guard let w = lastWhisper else { throw CoreError.audioModelNotLoaded }
            try loadWhisper(path: w.path, backend: w.backend)
        }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_whisper_next_piece(ctx, path, startSample, isFinal ? 1 : 0,
                                                     CoreEngine.progressTrampoline, user)),
              let r = decode(WhisperPieceJSON.self, json) else {
            throw CoreError.transcriptionFailed
        }
        switch r.status {
        case "wait": return .wait
        case "end": return .end
        case "piece":
            guard let end = r.end_sample, end > startSample else { throw CoreError.transcriptionFailed }
            return .piece(startSec: r.start_sec ?? Int(startSample / 16_000), endSample: end, text: r.text ?? "",
                          conf: r.conf, needsGemma: r.needs_gemma ?? false)
        default: throw CoreError.transcriptionFailed
        }
    }

    private struct SpanJSON: Decodable {
        let text: String
        let conf: Double?
    }

    /// Gemma on exactly [start, end) of a finished WAV (a piece Whisper left pending), on `backend` (switched here,
    /// which frees Whisper). Throws on failure; cancels (throws) when `shouldStop` turns true during the run.
    func transcribeSpan(path: String, start: Int64, end: Int64, previousText: String?, backend: ModelBackend,
                        shouldStop: @escaping () -> Bool = { false }) throws -> (String, Double?) {
        guard let ctx else { throw CoreError.initFailed }
        try ensureBackend(backend)
        guard sve_audio_loaded(ctx) != 0 else { throw CoreError.audioModelNotLoaded }
        let box = ProgressBox(onProgress: { _, _, _ in }, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_transcribe_span(ctx, path, start, end, previousText, CoreEngine.progressTrampoline, user)),
              let r = decode(SpanJSON.self, json) else {
            throw CoreError.transcriptionFailed
        }
        return (r.text, r.conf)
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
        try ensureGemmaLoaded()
        let box = ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let out = take(sve_generate(ctx, transcript, kind.cKind, lang, CoreEngine.progressTrampoline, user)) else {
            throw CoreError.generationFailed
        }
        return out
    }

    /// A part's notes and questions written together (sectionOutputsPair). On by default (the owner approved the
    /// speed-up); off with the UserDefaults bool "engine.pairWrite" set to false, or SVE_PAIR=0 in the launch
    /// environment (the phone gate). Read at every model load.
    nonisolated static var pairWriting: Bool {
        if ProcessInfo.processInfo.environment["SVE_PAIR"] == "0" { return false }
        return UserDefaults.standard.object(forKey: "engine.pairWrite") as? Bool ?? true
    }

    /// One part's notes and questions written together (one reading of the part, two replies), each stored by the
    /// core as it ends; the core writes them one at a time when they do not fit together. Returns how many were
    /// stored (1 or 2). Throws when none was: a cancel (shouldStop), no model, the note or its sections gone, or
    /// both tries failed (the core counts them; sectionOutputsState says how many and why).
    func sectionOutputsPair(noteID: Int64, idx: Int, lang: String, shouldStop: @escaping () -> Bool = { false },
                            onProgress: @escaping (String, Int, Int) -> Void = { _, _, _ in }) throws -> Int {
        guard let ctx else { throw CoreError.initFailed }
        try ensureGemmaLoaded()
        let box = ProgressBox(onProgress: onProgress, isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        let stored = Int(sve_section_outputs_pair(ctx, noteID, Int32(idx), lang, CoreEngine.progressTrampoline, user))
        guard stored > 0 else { throw CoreError.generationFailed }
        return stored
    }

    func ask(context: String, question: String, lang: String) throws -> String {
        guard let ctx else { throw CoreError.initFailed }
        try ensureGemmaLoaded()
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
        try? ensureGemmaLoaded()
        let r = sve_transcript_fits(ctx, transcript, lang)
        return r < 0 ? nil : r == 1
    }

    /// Whether the title, summary, notes and questions fit one prompt for this transcript (writeAll). False with no model.
    func allFits(_ transcript: String) async -> Bool {
        guard let ctx else { return false }
        try? ensureGemmaLoaded()
        return sve_all_fits(ctx, transcript) == 1
    }

    /// The title, summary, detailed notes and questions from one generation. nil when the reply did not split (or
    /// the transcript does not fit); throws when cancelled (shouldStop) or no model. progress: 0...1, an estimate
    /// from the tokens written so far.
    func writeAll(transcript: String, lang: String, shouldStop: @escaping () -> Bool,
                  progress: @escaping (Double) -> Void) async throws -> (title: String, summary: String, notes: String, questions: String)? {
        guard let ctx else { throw CoreError.initFailed }
        try ensureGemmaLoaded()
        let counter = ProgressCounter()
        let box = ProgressBox(onProgress: { _, _, _ in progress(min(0.95, Double(counter.next()) / 1100.0)) },
                              isCancelled: { Task.isCancelled || shouldStop() })
        let user = Unmanaged.passUnretained(box).toOpaque()
        guard let json = take(sve_generate_all(ctx, transcript, lang, CoreEngine.progressTrampoline, user)) else {
            if Task.isCancelled || shouldStop() { throw CoreError.generationFailed }
            return nil
        }
        struct All: Decodable { let title, summary, notes, questions: String }
        guard let data = json.data(using: .utf8), let a = try? JSONDecoder().decode(All.self, from: data) else { return nil }
        return (a.title, a.summary, a.notes, a.questions)
    }

    private final class ProgressCounter: @unchecked Sendable {
        private let lock = NSLock()
        private var n = 0
        func next() -> Int { lock.lock(); defer { lock.unlock() }; n += 1; return n }
    }

    func suggestTitle(transcript: String, lang: String) -> String? {
        guard let ctx else { return nil }
        try? ensureGemmaLoaded()
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
    nonisolated func deleteNote(_ id: Int64) async {
        await store { _ = sve_note_delete($0, id) }
        ProcessingPipeline.forgetNoteMarks(id)   // the pipeline's small per-note marks go with it
    }
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

extension NoteSteps.Kind {
    /// The note output this part kind belongs to.
    var outputKind: OutputKind { self == .notes ? .detailedNotes : .revisionQuestions }
    init?(_ kind: OutputKind) {
        switch kind {
        case .detailedNotes: self = .notes
        case .revisionQuestions: self = .questions
        default: return nil
        }
    }
}
