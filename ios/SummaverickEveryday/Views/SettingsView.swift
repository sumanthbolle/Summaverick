import SwiftUI
import UserNotifications

struct SettingsView: View {
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss
    @State private var models = ModelManager.shared
    @State private var confirmWipe = false
    @State private var confirmRemoveAI = false
    @State private var wipeFailed = false
    @State private var wipeBusy = false
    @State private var wiping = false
    @State private var pipeline = ProcessingPipeline.shared

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
            if !ok { wipeFailed = true }
        }
    }
}
