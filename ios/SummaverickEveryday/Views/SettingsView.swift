import SwiftUI
import UserNotifications

struct SettingsView: View {
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss
    @State private var models = ModelManager.shared
    @State private var speedRecords: [(noteID: Int64, record: SpeedRecord)] = []
    @State private var confirmWipe = false
    @State private var confirmRemoveAI = false
    @State private var wipeFailed = false
    @State private var wipeBusy = false
    @State private var wiping = false
    @State private var pipeline = ProcessingPipeline.shared

    /// Every note's speed record (SpeedRecord), newest notes first.
    private func loadSpeed() async {
        var out: [(noteID: Int64, record: SpeedRecord)] = []
        for n in await CoreEngine.shared.notes() {
            guard let json = await CoreEngine.shared.setting(SpeedRecord.key(n.id)), let data = json.data(using: .utf8),
                  let r = try? JSONDecoder().decode(SpeedRecord.self, from: data) else { continue }
            out.append((noteID: n.id, record: r))
        }
        speedRecords = out
    }

    var body: some View {
        @Bindable var settings = settings
        NavigationStack {
            Form {
                Section {
                    NavigationLink { ProfileView() } label: {
                        Label("Profile", systemImage: "person.crop.circle").frame(minHeight: 44)
                    }
                    NavigationLink { LearnedView() } label: {
                        Label("Things I remember", systemImage: "brain.head.profile").frame(minHeight: 44)
                    }
                    Button {
                        dismiss()
                        Task {   // once the sheet has gone, so the tips sit over the real tabs
                            try? await Task.sleep(for: .milliseconds(500))
                            SetupState.shared.replayTour()
                        }
                    } label: {
                        Label("Show me around again", systemImage: "questionmark.circle").frame(minHeight: 44)
                    }
                    .foregroundStyle(Theme.text)
                }
                .listRowBackground(Theme.formRow)

                Section("Appearance") {
                    Picker("Appearance", selection: $settings.appearance) {
                        Text("System").tag(AppearanceChoice.system)
                        Text("White").tag(AppearanceChoice.white)
                        Text("Dark").tag(AppearanceChoice.dark)
                    }
                    .frame(minHeight: 44)
                }
                .listRowBackground(Theme.formRow)

                Section("Notes language") {
                    Picker("Notes language", selection: $settings.notesLanguage) {
                        Text("English").tag("en"); Text("Telugu").tag("te"); Text("Hindi").tag("hi")
                        Text("Tamil").tag("ta"); Text("Kannada").tag("kn"); Text("Malayalam").tag("ml")
                    }
                    Text("I listen to the recording itself, so Telugu, Hindi and mixed speech all work. I write the summary, notes and questions in the language above.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)

                aiSection

                Section("Stop when quiet") {
                    Picker("Stop when quiet", selection: $settings.quietStopMinutes) {
                        Text("Off").tag(0); Text("2 min").tag(2); Text("5 min").tag(5); Text("10 min").tag(10)
                    }
                    Text("I stop recording after this long without voice, and keep everything said. I'll tell you 30 seconds before, and you can keep recording.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)

                Section("Keep recordings for") {
                    Picker("Audio retention", selection: $settings.retentionDays) {
                        Text("1 day").tag(1); Text("7 days").tag(7); Text("30 days").tag(30); Text("Forever").tag(0)
                    }
                    Text("Raw audio is deleted after this period to save space. What was said and your notes are kept until you delete them.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)

                Section {
                    NavigationLink { ProcessingActivityView() } label: {
                        Label("Processing activity", systemImage: "waveform.path.ecg")
                    }
                } footer: {
                    Text("What the notes work is doing now, and every step it took, on this phone.")
                }
                .listRowBackground(Theme.formRow)

                Section("Speed on this phone") {
                    ForEach(SpeedMath.summaryLines(speedRecords.map(\.record)), id: \.self) { Text(verbatim: $0) }
                    if speedRecords.contains(where: { $0.record.finishedAt != nil }) {
                        ShareLink(item: SpeedMath.csv(speedRecords.filter { $0.record.finishedAt != nil }),
                                  subject: Text("Summaverick speed on this phone"),
                                  preview: SharePreview("Summaverick speed on this phone")) {
                            Label("Share speed report", systemImage: "square.and.arrow.up")
                        }
                    }
                    Text("Only the time spent working is counted, not pauses. Measured on this phone; nothing is sent anywhere unless you share it.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)
                .task { await loadSpeed() }

                Section("Privacy") {
                    Toggle("Require Face ID / passcode to open", isOn: $settings.requireUnlock)
                        .tint(Theme.toggleOn)
                        .disabled(!AppLock.isAvailable)
                    Toggle("Show recording reminder", isOn: Binding(get: { !settings.consentAcknowledged }, set: { settings.consentAcknowledged = !$0 }))
                        .tint(Theme.toggleOn)
                    Text("Summaverick has no account, no analytics and no server. The only network use is downloading Summaverick's language pack.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)

                Section("About") {
                    Text("Summaverick's notes are written by software running on this phone, and it can get things wrong. Please check anything important.")
                        .font(.footnote).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.formRow)

                Section {
                    Button(role: .destructive) {
                        if pipeline.isBusy { wipeBusy = true } else { confirmWipe = true }
                    } label: {
                        HStack {
                            Text("Delete everything")
                            if wiping { Spacer(); ProgressView() }
                        }
                        .frame(minHeight: 44)
                    }
                    .disabled(wiping)
                } footer: {
                    Text("Summaverick Everyday \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "") · core \(String(cString: sve_version()))")
                }
                .listRowBackground(Theme.formRow)
            }
            .themedForm()
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .confirmationDialog("Delete everything on this phone?", isPresented: $confirmWipe, titleVisibility: .visible) {
                Button("Delete all", role: .destructive) { wipe() }
            } message: {
                Text("All notes and recordings are deleted, and so are your profile, medicines, reminders and everything Summaverick has learned. This cannot be undone.")
            }
            .alert("I couldn't delete everything. Try again.", isPresented: $wipeFailed) {
                Button("Retry", role: .destructive) { wipe() }
                Button("Cancel", role: .cancel) {}
            }
            .alert("I'm still working on a recording. Try again when I've finished.", isPresented: $wipeBusy) {
                Button("OK", role: .cancel) {}
            }
            .confirmationDialog("Remove Summaverick's language pack?", isPresented: $confirmRemoveAI, titleVisibility: .visible) {
                Button("Remove", role: .destructive) {
                    Task {
                        pipeline.memoriesHeld = true   // no memory backfill starts while the AI is removed
                        defer { pipeline.memoriesHeld = false }
                        // Memory work stops for it (K42); a note still being written: busy, as before.
                        guard await pipeline.yieldMemories(), !pipeline.isWorking else { wipeBusy = true; return }
                        await models.remove()
                    }
                }
            } message: {
                Text("Your notes stay. New recordings wait until it is downloaded again.")
            }
            .onChange(of: confirmWipe || wipeFailed || wipeBusy || confirmRemoveAI) { _, open in PresentationGate.shared.set("settingsWipe", open) }
        }
    }

    /// Status, Download again, mobile data and Remove. The model itself is never named.
    @ViewBuilder
    private var aiSection: some View {
        let status = models.status
        Section("Summaverick's language pack") {
            if status == .unsupported {
                Text(AIStatus.unsupportedMessage).font(.footnote).foregroundStyle(Theme.secondary)
                if models.hasFiles {   // left from an older version; never used on this phone
                    Button("Remove to free space", role: .destructive) {
                        if pipeline.isBusy { wipeBusy = true } else { confirmRemoveAI = true }
                    }
                    .frame(minHeight: 44)
                }
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    Label(status.settingsLine, systemImage: status.symbol).frame(minHeight: 44, alignment: .leading)
                    if let p = status.progress { ProgressView(value: p) }
                    if let d = status.detail { Text(d).font(.footnote).foregroundStyle(Theme.secondary) }
                }
                if status.canRetry {
                    Button("Download again") { models.startIfNeeded(userInitiated: true) }.frame(minHeight: 44)
                }
                AICellularToggle()
                if status.isReady {
                    Button("Remove to free space", role: .destructive) {
                        if pipeline.isBusy { wipeBusy = true } else { confirmRemoveAI = true }
                    }
                    .frame(minHeight: 44)
                }
            }
        }
        .listRowBackground(Theme.formRow)
    }

    /// Deletes the notes and their audio, then everything the assistant stored (profile, medicines,
    /// memory, reminders, tasks, topics, insights), then removes every scheduled reminder.
    /// Any failure (a note left, or the core refusing) shows "Could not delete everything" with Retry.
    private func wipe() {
        guard !wiping else { return }
        guard !pipeline.isBusy else { wipeBusy = true; return }
        wiping = true
        Task {
            pipeline.memoriesHeld = true   // no memory backfill starts while everything is deleted
            defer { pipeline.memoriesHeld = false }
            // Memory work stops for it (K42); a note still being written: busy, as before.
            guard await pipeline.yieldMemories(), !pipeline.isWorking else { wiping = false; wipeBusy = true; return }
            let engine = CoreEngine.shared
            for n in await engine.notes(limit: 10_000) {
                pipeline.forget(noteID: n.id)
                if let full = await engine.note(n.id), let p = AudioRecorder.existingAudioPath(full.audio_path) { AudioRecorder.removeAudio(atPath: p) }
                await engine.deleteNote(n.id)
            }
            var ok = await engine.notes(limit: 1).isEmpty
            do { try await engine.wipeAssistant() } catch {
                NSLog("Settings: assistant wipe failed: %@", "\(error)")
                ok = false
            }
            await ReminderScheduler.shared.resync()   // no active reminders left: removes every scheduled one
            UNUserNotificationCenter.current().removeAllDeliveredNotifications()
            NotifyCopy.rememberFirstName(of: nil)   // the notifications no longer greet by name
            ChatStore.shared.deleteAll()   // Ask's chats (Application Support/Chats), even when Ask was never opened
            NotificationCenter.default.post(name: .assistantWiped, object: nil)
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            wiping = false
            if !ok { wipeFailed = true; return }
            // A fresh start: the first-time setup again, asking for agreement to record again.
            settings.consentAcknowledged = false
            for key in SetupFlow.keysClearedByWipe { UserDefaults.standard.removeObject(forKey: key) }
            UserDefaults.standard.set(SetupFlow.afterWipe.rawValue, forKey: SetupFlow.progressKey)
            dismiss()
            try? await Task.sleep(for: .milliseconds(500))   // once Settings has gone, so the welcome can show
            SetupState.shared.restartAfterWipe()
        }
    }
}


/// What the notes work is doing now and the steps it took (ActivityLog), refreshed every 2 s: the stage and for how
/// long, the heat, the last sign of life, and the recent events with their times. Nothing of what was said.
struct ProcessingActivityView: View {
    @State private var pipeline = ProcessingPipeline.shared
    @State private var heat = HeatGovernor.shared
    @State private var entries: [ActivityLog.Entry] = []
    @State private var titles: [Int64: String] = [:]

    var body: some View {
        SwiftUI.TimelineView(.periodic(from: .now, by: 2)) { context in
            List {
                Section("Now") {
                    if pipeline.isWorking, let id = pipeline.activeNoteID {
                        LabeledContent("Recording", value: titles[id] ?? "#\(id)")
                        LabeledContent("Step", value: pipeline.progressLine)
                        LabeledContent("For", value: Duration.seconds(context.date.timeIntervalSince(pipeline.currentStageSince))
                            .formatted(.units(allowed: [.hours, .minutes, .seconds], width: .abbreviated)))
                        let quiet = ContinuedWork.secondsSinceAlive
                        LabeledContent("Last sign of life", value: Duration.seconds(Int(quiet))
                            .formatted(.units(allowed: [.minutes, .seconds], width: .abbreviated)))
                        if Watchdog.stalled(working: true, secondsSinceAlive: quiet) {
                            Label("No progress for a while. It may be stuck; it carries on by itself, or tap Continue now on the note.",
                                  systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                        }
                    } else {
                        Text("Nothing is being written now.").foregroundStyle(Theme.secondary)
                    }
                    LabeledContent("Phone heat", value: heat.level.name)
                }
                .listRowBackground(Theme.formRow)
                Section("Recent steps") {
                    if entries.isEmpty { Text("Nothing logged yet.").foregroundStyle(Theme.secondary) }
                    ForEach(Array(entries.enumerated()), id: \.offset) { _, e in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(e.time, format: .dateTime.hour().minute().second()).font(.caption.monospacedDigit())
                                Text(verbatim: e.event).font(.caption.weight(.semibold))
                                if let n = e.note { Text(verbatim: titles[n] ?? "#\(n)").font(.caption).foregroundStyle(Theme.secondary).lineLimit(1) }
                            }
                            if !e.detail.isEmpty { Text(verbatim: e.detail).font(.caption2.monospaced()).foregroundStyle(Theme.secondary) }
                        }
                    }
                }
                .listRowBackground(Theme.formRow)
            }
            .onChange(of: context.date) { _, _ in entries = ActivityLog.recent(200) }
        }
        .navigationTitle("Processing activity")
        .task {
            entries = ActivityLog.recent(200)
            titles = Dictionary(await CoreEngine.shared.notes().map { ($0.id, $0.title) }, uniquingKeysWith: { a, _ in a })
        }
    }
}
