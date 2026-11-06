// Recording → transcript → title → three outputs. Everything after recording is local.
// If iOS suspends the app mid-way, the note keeps whatever was finished; the user can tap
// "Generate notes" on the note to resume. Same stages the device firmware will run.
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class ProcessingPipeline {
    static let shared = ProcessingPipeline()

    enum Stage: Equatable {
        case idle
        case transcribing(Double)
        case loadingModel
        case extracting
        case generating(OutputKind, String, Int, Int)   // kind, phase, done, total
        case done
        case failed(String)

        var label: String {
            switch self {
            case .idle: return ""
            case .transcribing(let p): return String(localized: "Transcribing… \(Int(p * 100))%")
            case .loadingModel: return String(localized: "Loading AI model…")
            case .extracting: return String(localized: "Finding reminders and tasks…")
            case .generating(let k, let phase, let d, let t):
                return phase == "chunk" && t > 1 ? String(localized: "Reading part \(d + 1) of \(t) for \(k.title.lowercased())…") : String(localized: "Writing \(k.title.lowercased())…")
            case .done: return String(localized: "Done")
            case .failed(let e): return String(localized: "Failed: \(e)")
            }
        }
    }

    private(set) var stage: Stage = .idle {
        didSet { if case .failed(let msg) = stage, let id = activeNoteID { lastFailure[id] = msg } }
    }
    private(set) var activeNoteID: Int64?
    /// Last failure message per note, so the note screen can show why nothing was generated.
    private(set) var lastFailure: [Int64: String] = [:]
    var isBusy: Bool {
        switch stage { case .idle, .done, .failed: return false; default: return true }
    }

    private let transcriber = SpeechTranscriber()
    private var bgTask: UIBackgroundTaskIdentifier = .invalid

    /// Full run for a fresh recording. Returns the note id as soon as it exists.
    @discardableResult
    func process(audio: URL, duration: Int64, language: SpeechLanguage) async -> Int64? {
        guard !isBusy else { return nil }
        beginBackground()
        defer { endBackground() }

        let engine = CoreEngine.shared
        let title = "Recording " + Self.titleFormatter.string(from: Date())
        let noteLang = notesLang(fallback: language.shortCode)
        let id = await engine.createNote(title: title, lang: noteLang, audioPath: audio.path, transcript: nil, durationSeconds: duration)
        guard id > 0 else { stage = .failed(String(localized: "Could not save the note.")); return nil }
        activeNoteID = id
        NotificationCenter.default.post(name: .notesChanged, object: nil)

        // 1. transcript
        lastFailure[id] = nil
        stage = .transcribing(0)
        let pieces: [TranscriptPiece]
        do {
            pieces = try await transcribe(audio, language: language)
        } catch {
            NSLog("[sve] transcription failed: %@", String(describing: error))
            stage = .failed(error.localizedDescription)
            return id
        }
        let transcript = Self.joined(pieces)
        await engine.setPieces(id, pieces)
        await engine.setTranscript(id, transcript)
        NotificationCenter.default.post(name: .notesChanged, object: nil)
        if transcript.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            stage = .failed(String(localized: "No speech was detected in the recording."))
            return id
        }

        // 2. notes
        await generateOutputs(noteID: id, transcript: transcript, lang: noteLang, setTitle: true)
        return id
    }

    /// Re-runs the AI stage for an existing note (resume after interruption, or after a model change).
    func regenerate(noteID: Int64) async {
        guard !isBusy, let note = await CoreEngine.shared.note(noteID) else { return }
        beginBackground()
        defer { endBackground() }
        activeNoteID = noteID
        lastFailure[noteID] = nil

        // No transcript yet (earlier transcription failed): redo it from the audio if we still have it.
        var t = note.transcript ?? ""
        if t.isEmpty {
            guard let path = note.audio_path, FileManager.default.fileExists(atPath: path) else {
                stage = .failed(String(localized: "No transcript and the audio is no longer available.")); return
            }
            let lang = SpeechTranscriber.onDeviceLanguages().first { $0.shortCode == note.lang } ?? AppSettings.shared.language
            stage = .transcribing(0)
            var pieces: [TranscriptPiece] = []
            do {
                pieces = try await transcribe(URL(fileURLWithPath: path), language: lang)
                t = Self.joined(pieces)
            } catch {
                NSLog("[sve] transcription failed: %@", String(describing: error))
                stage = .failed(error.localizedDescription); return
            }
            if t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                stage = .failed(String(localized: "No speech was detected in the recording.")); return
            }
            await CoreEngine.shared.setPieces(noteID, pieces)
            await CoreEngine.shared.setTranscript(noteID, t)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }
        await generateOutputs(noteID: noteID, transcript: t, lang: notesLang(fallback: note.lang), setTitle: note.title.hasPrefix("Recording "))
    }

    /// Audio → text. An audio-capable model (Gemma 4) hears the recording itself, so any language mix works;
    /// otherwise Apple's on-device recogniser is used for the chosen language.
    private func transcribe(_ audio: URL, language: SpeechLanguage) async throws -> [TranscriptPiece] {
        let mm = ModelManager.shared
        let spec = mm.selected
        if spec.hearsAudio && mm.isInstalled(spec) {
            try await loadSelectedModel()
            stage = .transcribing(0)
            return try await CoreEngine.shared.transcribeAudioPieces(path: audio.path) { done, total in
                Task { @MainActor in self.stage = .transcribing(total > 0 ? Double(done) / Double(total) : 0) }
            }
        }
        return try await transcriber.transcribe(fileURL: audio, locale: language.locale) { p in
            Task { @MainActor in self.stage = .transcribing(p) }
        }
    }

    private static func joined(_ pieces: [TranscriptPiece]) -> String {
        pieces.map(\.text).joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Notes are written in the Notes language when the model hears audio; otherwise in the recording's language.
    private func notesLang(fallback: String) -> String {
        let mm = ModelManager.shared
        return mm.selected.hearsAudio && mm.isInstalled(mm.selected) ? AppSettings.shared.notesLanguage : fallback
    }

    private func loadSelectedModel() async throws {
        let mm = ModelManager.shared
        let spec = mm.selected
        stage = .loadingModel
        try await CoreEngine.shared.loadModel(at: mm.fileURL(for: spec).path, contextTokens: spec.contextTokens,
                                              mmprojPath: mm.mmprojURL(for: spec)?.path)
    }

    private func generateOutputs(noteID id: Int64, transcript: String, lang: String, setTitle: Bool) async {
        let engine = CoreEngine.shared
        let mm = ModelManager.shared
        guard mm.selectedInstalled else {
            stage = .failed(String(localized: "Download the AI model in Settings, then tap Generate notes."))
            return
        }
        do {
            try await loadSelectedModel()
        } catch {
            stage = .failed(error.localizedDescription)
            return
        }

        // Reminders, tasks and profile changes first, so they are ready even if note writing is interrupted.
        // A failure here never fails the notes.
        let assistant = mm.selected.hearsAudio
        if assistant {
            if await engine.pieces(id).isEmpty {   // note from before pieces existed
                await engine.setPieces(id, [TranscriptPiece(startSec: 0, text: transcript)])
            }
            stage = .extracting
            do {
                let n = try await engine.extract(noteID: id)
                NSLog("[sve] extract added %d suggestions", n)
            } catch {
                NSLog("[sve] extract failed: %@", String(describing: error))
            }
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
        }

        if setTitle, let t = await engine.suggestTitle(transcript: transcript, lang: lang) {
            await engine.setTitle(id, t)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
        }

        for kind in OutputKind.allCases {
            stage = .generating(kind, "final", 0, 1)
            do {
                let text = try await engine.generate(transcript: transcript, kind: kind, lang: lang) { phase, done, total in
                    Task { @MainActor in self.stage = .generating(kind, phase, done, total) }
                }
                await engine.setOutput(id, kind, text)
                NotificationCenter.default.post(name: .notesChanged, object: nil)
            } catch {
                stage = .failed(error.localizedDescription)
                return
            }
        }
        if assistant {
            _ = await engine.generateInsight(noteID: id)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
        }
        stage = .done
    }

    private func beginBackground() {
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "sve.process") { [weak self] in
            self?.endBackground()
        }
    }
    private func endBackground() {
        if bgTask != .invalid { UIApplication.shared.endBackgroundTask(bgTask); bgTask = .invalid }
    }

    private static let titleFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "d MMM, h:mm a"
        return f
    }()
}

extension Notification.Name {
    static let notesChanged = Notification.Name("sve.notesChanged")
}
