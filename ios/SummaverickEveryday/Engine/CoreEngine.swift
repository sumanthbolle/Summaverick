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
        case .initFailed: return String(localized: "Could not open the notes database.")
        case .modelLoadFailed(let p): return String(localized: "Could not load the AI model at \(p).")
        case .generationFailed: return String(localized: "The AI model could not produce an answer.")
        case .notFound: return String(localized: "Note not found.")
        case .audioModelNotLoaded: return String(localized: "The selected AI model cannot listen to audio.")
        case .transcriptionFailed: return String(localized: "The AI model could not transcribe the recording.")
        }
    }
}

/// Serialises all access to one sve_ctx. Call from anywhere; work happens off the main thread.
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

    private init() {
        sve_set_log({ level, msg, _ in
            if let msg { NSLog("[sve %d] %@", level, String(cString: msg)) }
        }, nil)
        ctx = sve_init(CoreEngine.databaseURL.path)
        if ctx == nil { NSLog("sve_init failed") }
    }

    func take(_ p: UnsafeMutablePointer<CChar>?) -> String? {
        guard let p else { return nil }
        defer { sve_free_string(p) }
        return String(cString: p)
    }

    func decode<T: Decodable>(_ type: T.Type, _ json: String?) -> T? {
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
        if loadedModelPath == path, loadedBackend == backend, sve_llm_loaded(ctx) != 0,
           mmprojPath == nil || sve_audio_loaded(ctx) != 0 { return }
        if sve_load_llm(ctx, path, contextTokens, backend.threads, backend.gpuLayers) != 0 {
            loadedModelPath = nil
            loadedBackend = nil
            throw CoreError.modelLoadFailed(path)
        }
        loadedModelPath = path
        loadedBackend = backend
        NSLog("[sve] model loaded on the %@", backend == .gpu ? "GPU" : "CPU")
        if let mmprojPath, sve_load_audio(ctx, mmprojPath) != 0 {
            throw CoreError.modelLoadFailed(mmprojPath)
        }
    }

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

    // MARK: notes

    func createNote(title: String, lang: String, audioPath: String?, transcript: String?, durationSeconds: Int64) -> Int64 {
        sve_note_create(ctx, title, lang, audioPath, transcript, durationSeconds)
    }

    func setTranscript(_ id: Int64, _ transcript: String) { _ = sve_note_set_transcript(ctx, id, transcript) }
    func setOutput(_ id: Int64, _ kind: OutputKind, _ text: String) { _ = sve_note_set_output(ctx, id, kind.cKind, text) }
    func setTitle(_ id: Int64, _ title: String) { _ = sve_note_set_title(ctx, id, title) }
    func deleteNote(_ id: Int64) { _ = sve_note_delete(ctx, id) }
    func clearAudio(_ id: Int64) { _ = sve_note_clear_audio(ctx, id) }
    func setAudioPath(_ id: Int64, _ path: String) -> Bool { sve_note_set_audio_path(ctx, id, path) == 0 }
    func clearTranscript(_ id: Int64) { _ = sve_note_clear_transcript(ctx, id) }

    func note(_ id: Int64) -> Note? { decode(Note.self, take(sve_note_get_json(ctx, id))) }

    func notes(query: String = "", limit: Int32 = 200) -> [NoteSummary] {
        decode([NoteSummary].self, take(sve_notes_list_json(ctx, query, limit))) ?? []
    }

    func audioOlderThan(days: Int32) -> [AudioRetentionRow] {
        decode([AudioRetentionRow].self, take(sve_notes_audio_older_than_json(ctx, days))) ?? []
    }

    // MARK: settings

    func setting(_ key: String) -> String? { take(sve_setting_get(ctx, key)) }
    func setSetting(_ key: String, _ value: String) { _ = sve_setting_set(ctx, key, value) }
}
