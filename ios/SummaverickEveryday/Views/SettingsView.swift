import SwiftUI
import UserNotifications

struct SettingsView: View {
    @Environment(AppSettings.self) private var settings
    @Environment(\.dismiss) private var dismiss
    @State private var models = ModelManager.shared
    @State private var languages: [SpeechLanguage] = []
    @State private var confirmWipe = false
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
                        Label("What Summaverick has learned", systemImage: "brain.head.profile").frame(minHeight: 44)
                    }
                }

                if models.selected.hearsAudio {
                    Section("Notes language") {
                        Picker("Notes language", selection: $settings.notesLanguage) {
                            Text("English").tag("en"); Text("Telugu").tag("te"); Text("Hindi").tag("hi")
                            Text("Tamil").tag("ta"); Text("Kannada").tag("kn"); Text("Malayalam").tag("ml")
                        }
                        Text("\(models.selected.name) listens to the recording itself, so Telugu, Hindi and mixed speech work in any language. Summary, notes and questions are written in the language above.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                } else {
                    Section("Language of recordings") {
                        if languages.isEmpty {
                            Text("No on-device speech languages found. Add a dictation language in Settings > General > Keyboard > Dictation.")
                                .font(.footnote).foregroundStyle(.secondary)
                        } else {
                            Picker("Language", selection: $settings.languageID) {
                                ForEach(languages) { Text($0.name).tag($0.id) }
                            }
                            Text("Only languages that work fully on this iPhone are listed. Audio is never sent to a server.")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                    }
                }

                Section("AI model (runs on this phone)") {
                    ForEach(ModelCatalog.all) { spec in
                        ModelRow(spec: spec, models: models, selected: models.selectedID == spec.id) {
                            models.selectedID = spec.id
                            Task { await CoreEngine.shared.unloadModel() }
                        }
                    }
                    if let e = models.downloadError { Text(e).font(.footnote).foregroundStyle(.red) }
                    Text("This phone has \(ramGB) GB of memory. Recommended: \(ModelCatalog.recommended.name).")
                        .font(.footnote).foregroundStyle(.secondary)
                }

                Section("Keep recordings for") {
                    Picker("Audio retention", selection: $settings.retentionDays) {
                        Text("1 day").tag(1); Text("7 days").tag(7); Text("30 days").tag(30); Text("Forever").tag(0)
                    }
                    Text("Raw audio is deleted after this period to save space. Transcripts and notes are kept until you delete them.")
                        .font(.footnote).foregroundStyle(.secondary)
                }

                Section("Privacy") {
                    Toggle("Require Face ID / passcode to open", isOn: $settings.requireUnlock)
                        .disabled(!AppLock.isAvailable)
                    Toggle("Show recording reminder", isOn: Binding(get: { !settings.consentAcknowledged }, set: { settings.consentAcknowledged = !$0 }))
                    Text("Summaverick has no account, no analytics and no server. The only network use is downloading the AI model.")
                        .font(.footnote).foregroundStyle(.secondary)
                }

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
            }
            .navigationTitle("Settings")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .task { languages = SpeechTranscriber.onDeviceLanguages() }
            .confirmationDialog("Delete everything on this phone?", isPresented: $confirmWipe, titleVisibility: .visible) {
                Button("Delete all", role: .destructive) { wipe() }
            } message: {
                Text("All notes and recordings are deleted, and so are your profile, medicines, reminders and everything Summaverick has learned. This cannot be undone.")
            }
            .alert("Could not delete everything. Try again.", isPresented: $wipeFailed) {
                Button("Retry", role: .destructive) { wipe() }
                Button("Cancel", role: .cancel) {}
            }
            .alert("Summaverick is still working on a recording. Try again when it has finished.", isPresented: $wipeBusy) {
                Button("OK", role: .cancel) {}
            }
            .onChange(of: confirmWipe || wipeFailed || wipeBusy) { _, open in PresentationGate.shared.set("settingsWipe", open) }
        }
    }

    private var ramGB: String {
        String(format: "%.0f", (Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824).rounded())
    }

    /// Deletes the notes and their audio, then everything the assistant stored (profile, medicines,
    /// memory, reminders, tasks, topics, insights), then removes every scheduled reminder.
    /// Any failure (a note left, or the core refusing) shows "Could not delete everything" with Retry.
    private func wipe() {
        guard !wiping else { return }
        guard !pipeline.isBusy else { wipeBusy = true; return }
        wiping = true
        Task {
            let engine = CoreEngine.shared
            for n in await engine.notes(limit: 10_000) {
                if let full = await engine.note(n.id), let p = full.audio_path { try? FileManager.default.removeItem(atPath: p) }
                await engine.deleteNote(n.id)
            }
            var ok = await engine.notes(limit: 1).isEmpty
            do { try await engine.wipeAssistant() } catch {
                NSLog("Settings: assistant wipe failed: %@", "\(error)")
                ok = false
            }
            await ReminderScheduler.shared.resync()   // no active reminders left: removes every scheduled one
            UNUserNotificationCenter.current().removeAllDeliveredNotifications()
            NotificationCenter.default.post(name: .assistantChanged, object: nil)
            NotificationCenter.default.post(name: .notesChanged, object: nil)
            wiping = false
            if !ok { wipeFailed = true }
        }
    }
}

struct ModelRow: View {
    let spec: ModelSpec
    let models: ModelManager
    let selected: Bool
    let select: () -> Void

    var body: some View {
        let installed = models.isInstalled(spec)
        let progress = models.downloadProgress[spec.id]
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Button(action: select) {
                    HStack {
                        Image(systemName: selected ? "checkmark.circle.fill" : "circle").foregroundStyle(selected ? Color.accentColor : .secondary)
                        Text(spec.name)
                    }
                }
                .buttonStyle(.plain)
                Spacer()
                if progress != nil {
                    Button("Cancel") { models.cancelDownload(spec) }.font(.footnote)
                } else if installed {
                    Button("Remove") { models.delete(spec) }.font(.footnote).foregroundStyle(.red)
                } else {
                    Button("Download \(spec.approxMB) MB") { models.download(spec) }.font(.footnote)
                }
            }
            if let p = progress { ProgressView(value: p) }
            Text(spec.notes).font(.footnote).foregroundStyle(.secondary)
        }
        .padding(.vertical, 2)
    }
}
