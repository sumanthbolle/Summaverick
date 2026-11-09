// Ask Summaverick's chats, kept on the phone across launches (N9): a JSON file in Application Support
// (Chats/chats.json), not the notes database. The newest 50 chats are kept. "Delete everything" deletes them.
import Foundation
import Observation

/// One question in a chat and what came back.
struct ChatTurn: Codable, Identifiable, Equatable {
    enum State: Codable, Equatable {
        case waiting
        /// The visible answer so far, as the core writes it.
        case streaming(String)
        case answered(AskAnswer)
        /// Stop was tapped: the part written before it.
        case stopped(String)
        case failed
    }

    var id = UUID()
    let question: String
    var state: State

    /// The answer shown for it, for the history sent with a follow-up (ChatWindow).
    var windowTurn: ChatWindow.Turn {
        switch state {
        case .answered(let a):
            let shown = a.answer.trimmingCharacters(in: .whitespacesAndNewlines)
            // A quiz turn: what was put to the person is the question; the expected answer stays out of the history.
            let said = a.kind == "quiz" ? (a.question ?? shown) : shown
            return ChatWindow.Turn(question: question, answer: said.isEmpty ? nil : said, medicine: a.medicine,
                                   sources: a.sources.map { ChatMessage.SourceRef(title: $0.title, start_sec: $0.startSec) })
        case .stopped(let partial):
            return ChatWindow.Turn(question: question, answer: partial)
        case .waiting, .streaming, .failed:
            return ChatWindow.Turn(question: question, answer: nil)
        }
    }
}

struct Chat: Codable, Identifiable, Equatable {
    var id = UUID()
    var created = Date()
    var updated = Date()
    var turns: [ChatTurn] = []
    /// Quiz ids already asked in this chat (sent with the next quiz question). nil in files from before quizzes.
    var quizAsked: [String]?

    /// The first question, for the list of earlier chats.
    var title: String { turns.first?.question ?? "" }
}

@MainActor
@Observable
final class ChatStore {
    static let shared = ChatStore()

    /// Newest change first, at most ChatKeep.limit.
    private(set) var chats: [Chat] = []
    /// The chat on screen; nil = a new chat, created by its first question.
    private(set) var currentID: UUID?

    var current: Chat? { currentID.flatMap { id in chats.first { $0.id == id } } }

    @ObservationIgnored private var saveScheduled = false
    @ObservationIgnored private let writeQueue = DispatchQueue(label: "sve.chats.write", qos: .utility)

    static var fileURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("Chats", isDirectory: true).appendingPathComponent("chats.json")
    }

    private init() {
        load()
        currentID = chats.first?.id
    }

    // MARK: chats

    /// The next question starts a new chat; the one on screen stays in the list.
    func newChat() {
        currentID = nil
    }

    func select(_ id: UUID) {
        guard chats.contains(where: { $0.id == id }) else { return }
        currentID = id
    }

    /// Adds a question to the chat on screen (a new chat when there is none). Returns the turn's id.
    @discardableResult
    func append(_ question: String) -> UUID {
        let turn = ChatTurn(question: question, state: .waiting)
        if let i = currentIndex {
            chats[i].turns.append(turn)
            chats[i].updated = Date()
        } else {
            let chat = Chat(turns: [turn])
            chats.append(chat)
            currentID = chat.id
        }
        tidy()
        scheduleSave()
        return turn.id
    }

    /// Sets a turn's state, in whichever chat holds it. Nothing happens when it is gone (deleted meanwhile).
    func update(_ turnID: UUID, _ state: ChatTurn.State) {
        guard let c = chats.firstIndex(where: { $0.turns.contains { $0.id == turnID } }),
              let t = chats[c].turns.firstIndex(where: { $0.id == turnID }) else { return }
        chats[c].turns[t].state = state
        chats[c].updated = Date()
        // Streaming text redraws often: saved with the next settled state (or a moment later).
        if case .streaming = state { scheduleSave(after: 2) } else { scheduleSave() }
    }

    /// The turns before `turnID` in its chat (all of them when it is not found), for the follow-up's history.
    func turns(before turnID: UUID) -> [ChatTurn] {
        guard let chat = chats.first(where: { $0.turns.contains { $0.id == turnID } }),
              let i = chat.turns.firstIndex(where: { $0.id == turnID }) else { return current?.turns ?? [] }
        return Array(chat.turns[..<i])
    }

    // MARK: quiz

    /// The quiz ids already asked in the chat holding the turn.
    func quizAsked(turn turnID: UUID) -> [String] {
        chats.first { $0.turns.contains { $0.id == turnID } }?.quizAsked ?? []
    }

    func markQuizAsked(_ quizID: String, turn turnID: UUID) {
        guard let c = chats.firstIndex(where: { $0.turns.contains { $0.id == turnID } }) else { return }
        var asked = chats[c].quizAsked ?? []
        if !asked.contains(quizID) { asked.append(quizID) }
        chats[c].quizAsked = asked
        scheduleSave()
    }

    /// "Start over": every question can be asked again in the chat holding the turn.
    func clearQuiz(turn turnID: UUID) {
        guard let c = chats.firstIndex(where: { $0.turns.contains { $0.id == turnID } }) else { return }
        chats[c].quizAsked = nil
        scheduleSave()
    }

    /// "Delete everything": every chat, and the file.
    func deleteAll() {
        chats = []
        currentID = nil
        let url = Self.fileURL
        writeQueue.async { try? FileManager.default.removeItem(at: url) }
    }

    // MARK: storage

    private var currentIndex: Int? { currentID.flatMap { id in chats.firstIndex { $0.id == id } } }

    private func tidy() {
        chats = ChatKeep.kept(chats, updated: { $0.updated })
        if let id = currentID, !chats.contains(where: { $0.id == id }) { currentID = nil }
    }

    private func scheduleSave(after seconds: Double = 0.3) {
        guard !saveScheduled else { return }
        saveScheduled = true
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(seconds))
            saveScheduled = false
            save()
        }
    }

    private func save() {
        let snapshot = chats
        let url = Self.fileURL
        writeQueue.async {
            do {
                let data = try JSONEncoder().encode(snapshot)
                try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                try data.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            } catch {
                NSLog("[sve] chats save failed: %@", String(describing: error))
            }
        }
    }

    private func load() {
        guard let data = try? Data(contentsOf: Self.fileURL) else { return }
        guard var loaded = try? JSONDecoder().decode([Chat].self, from: data) else {
            NSLog("[sve] chats file unreadable; starting empty")
            return
        }
        // An answer that was being written when the app ended: kept as stopped, or failed with nothing written.
        for c in loaded.indices {
            for t in loaded[c].turns.indices {
                switch loaded[c].turns[t].state {
                case .waiting: loaded[c].turns[t].state = .failed
                case .streaming(let partial):
                    loaded[c].turns[t].state = partial.isEmpty ? .failed : .stopped(partial)
                default: break
                }
            }
        }
        chats = ChatKeep.kept(loaded, updated: { $0.updated })
    }
}
