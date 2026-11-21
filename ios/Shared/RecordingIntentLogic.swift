// Pure rules of the recording shortcuts (RecordingIntents.swift): what a start or stop says, and when the app opens
// instead. Foundation only and free of app state, so they can be checked with a standalone swiftc script.
import Foundation

enum RecordingIntentLogic {
    /// RecordingController.StartResult, one for one (the app maps it with an exhaustive switch).
    enum Start: CaseIterable, Equatable {
        case started, alreadyRecording, needsConsent, micDenied, failed
        /// A stop came while starting (the microphone question): nothing was recorded.
        case stoppedWhileStarting
    }

    /// What a start does after the controller answered.
    enum Reply: Equatable {
        /// Finish with this line (spoken by Siri, shown by Shortcuts).
        case say(Line)
        /// The recording reminder was never acknowledged: open the app at it. Nothing records.
        case openForConsent
    }

    enum Line: CaseIterable, Equatable {
        case recording, alreadyRecording, allowMicrophone, couldNotStart, stopped, notRecording
        /// Shown when the app asks to be opened for the recording reminder.
        case consentFirst
    }

    /// The Control Center toggle: what the new value asks for.
    enum Toggle: Equatable { case start, stop }

    static func reply(to start: Start) -> Reply {
        switch start {
        case .started: return .say(.recording)
        case .alreadyRecording: return .say(.alreadyRecording)
        case .needsConsent: return .openForConsent
        case .micDenied: return .say(.allowMicrophone)
        case .failed: return .say(.couldNotStart)
        case .stoppedWhileStarting: return .say(.notRecording)
        }
    }

    /// After the start's awaits: a stop (the control turned off, a Stop shortcut) or a cancel that came meanwhile
    /// wins, so nothing records after it.
    static func startMayRecord(stopRequested: Bool) -> Bool { !stopRequested }

    static func reply(stopped: Bool) -> Line { stopped ? .stopped : .notRecording }

    static func toggle(on: Bool) -> Toggle { on ? .start : .stop }

    static func text(_ line: Line) -> LocalizedStringResource {
        switch line {
        case .recording: return LocalizedStringResource("I'm recording.")
        case .alreadyRecording: return LocalizedStringResource("I'm already recording.")
        case .allowMicrophone: return LocalizedStringResource("Allow the microphone for Summaverick in Settings.")
        case .couldNotStart: return LocalizedStringResource("I couldn't start recording.")
        case .stopped: return LocalizedStringResource("Stopped. I'm writing your notes now.")
        case .notRecording: return LocalizedStringResource("I'm not recording anything.")
        case .consentFirst: return LocalizedStringResource("Open Summaverick to read the recording reminder first.")
        }
    }

    /// How long a shortcut's request to show the recording reminder is kept: opening the app much later must not
    /// show it (its "I understand" opens the recorder).
    static let consentRequestWindow: TimeInterval = 120

    static func consentRequestIsFresh(requestedAt: Date?, now: Date) -> Bool {
        guard let requestedAt else { return false }
        let age = now.timeIntervalSince(requestedAt)
        return age >= 0 && age <= consentRequestWindow
    }
}

// MARK: - the assistant shortcuts (What's on today, Last note summary, Add a reminder)

/// Pure rules of the assistant shortcuts in RecordingIntents.swift: what they say. Foundation only, like the above.
enum AssistantIntentLogic {
    /// One reminder time still to come today, already formatted ("8:30 AM").
    struct TodayReminder: Equatable {
        let time: String
        let text: String
    }

    /// Siri reads the whole answer: longer lists end with "and N more".
    static let maxSpokenReminders = 6
    static let maxSpokenTodos = 5

    /// "What's on today": the reminders still to come today, soonest first, then the open to-dos.
    static func todayText(reminders: [TodayReminder], todos: [String]) -> String {
        let todos = todos.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        guard !reminders.isEmpty || !todos.isEmpty else { return String(localized: "Nothing on today.") }
        var parts: [String] = []
        if !reminders.isEmpty {
            var lines = reminders.prefix(maxSpokenReminders).map { "\($0.time): \($0.text)" }
            if reminders.count > maxSpokenReminders { lines.append(more(reminders.count - maxSpokenReminders)) }
            parts.append(String(localized: "Reminders today:") + "\n" + lines.joined(separator: "\n"))
        }
        if !todos.isEmpty {
            var lines = Array(todos.prefix(maxSpokenTodos))
            if todos.count > maxSpokenTodos { lines.append(more(todos.count - maxSpokenTodos)) }
            parts.append(String(localized: "To do:") + "\n" + lines.joined(separator: "\n"))
        }
        return parts.joined(separator: "\n\n")
    }

    private static func more(_ n: Int) -> String { String(localized: "and \(n) more") }

    /// The most recent note: the latest recording time, the higher id on a tie (ids only grow).
    static func newestNote(_ notes: [(id: Int64, createdAt: Int64)]) -> Int64? {
        notes.max { $0.createdAt != $1.createdAt ? $0.createdAt < $1.createdAt : $0.id < $1.id }?.id
    }

    enum NoteState: Equatable {
        /// Summary, notes and questions are all written.
        case ready
        /// The pipeline is writing this note's notes now.
        case writing
        /// Not all written and nothing is writing them now (waiting, paused, overnight or failed).
        case notFinished
    }

    /// Never "ready" while the notes are being written or any part is missing.
    static func noteState(summary: String?, notes: String?, questions: String?, writingNow: Bool) -> NoteState {
        if writingNow { return .writing }
        let all = [summary, notes, questions].allSatisfy { !($0 ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
        return all ? .ready : .notFinished
    }

    /// "Last note summary": the title, the summary when there is one, and a plain line when the notes are not done.
    static func lastNoteText(title: String, summary: String?, state: NoteState) -> String {
        let t = title.trimmingCharacters(in: .whitespacesAndNewlines)
        var lines = [t.isEmpty ? String(localized: "Your last recording") : t]
        let s = (summary ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if !s.isEmpty { lines.append(s) }
        switch state {
        case .ready: if s.isEmpty { lines.append(String(localized: "This note has no summary.")) }
        case .writing: lines.append(String(localized: "I'm still writing its notes."))
        case .notFinished: lines.append(String(localized: "Its notes aren't finished yet."))
        }
        return lines.joined(separator: "\n")
    }

    static func noNotesText() -> String { String(localized: "You don't have any notes yet.") }

    /// "Add a reminder": a one-time reminder on the chosen day and minute, as the core takes it
    /// (start_date YYYY-MM-DD, times ["HH:MM"]). nil when the time has already passed (it could never ring).
    static func oneTimeReminder(at date: Date, now: Date, calendar: Calendar) -> (day: String, time: String)? {
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute], from: date)
        guard let y = c.year, let mo = c.month, let d = c.day, let h = c.hour, let mi = c.minute,
              let minute = calendar.date(from: DateComponents(year: y, month: mo, day: d, hour: h, minute: mi)),
              minute > now else { return nil }
        return (String(format: "%04d-%02d-%02d", y, mo, d), String(format: "%02d:%02d", h, mi))
    }

    /// How often a reminder from the shortcut rings.
    enum RepeatRule: String, CaseIterable { case once, daily, weekly }

    /// The first day and the time a reminder starts. Once: that minute, if still to come (else nil). Every day or
    /// week: that time of day, from that day if the time is still to come, else from the next day (week).
    static func reminderStart(at date: Date, rule: RepeatRule, now: Date, calendar: Calendar) -> (day: String, time: String)? {
        if rule == .once { return oneTimeReminder(at: date, now: now, calendar: calendar) }
        if let today = oneTimeReminder(at: date, now: now, calendar: calendar) { return today }
        guard let next = calendar.date(byAdding: .day, value: rule == .daily ? 1 : 7, to: date) else { return nil }
        return oneTimeReminder(at: next, now: now, calendar: calendar)
    }

    /// "every day at 8:00 AM" / "every Saturday at 8:00 AM", for what the shortcut says after saving.
    static func repeatWhen(rule: RepeatRule, time: String, weekday: String) -> String {
        switch rule {
        case .daily: return String(localized: "every day at \(time)")
        case .weekly: return String(localized: "every \(weekday) at \(time)")
        case .once: return time
        }
    }

    static func cleanedReminderText(_ text: String) -> String? {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return t.isEmpty ? nil : t
    }

    /// What "Add a reminder" says after saving. `when` is the day and time in the phone's style.
    static func reminderSavedText(text: String, when: String, canRing: Bool) -> String {
        let saved = String(localized: "Saved. I'll remind you: \(text), \(when).")
        return canRing ? saved : saved + " " + String(localized: "It can't ring until notifications are on for Summaverick.")
    }
}
