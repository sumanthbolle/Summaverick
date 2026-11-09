// Settings → Profile (spec §6.4): the same fields as setup, plus the medicines list. The person's own
// edit, so Save applies directly. Saving a medicine adds a reminder card on Today (spec §7.8b).
import SwiftUI

struct ProfileView: View {
    @State private var loaded: Profile?
    @State private var name = ""
    @State private var lang = ""
    @State private var routine = Routine()
    @State private var medicines: [Medicine] = []
    @State private var editing: MedicineEditing?
    @State private var deleting: Medicine?
    @State private var toast: String?
    @State private var saving = false
    @State private var errorShown = false
    @State private var deleteFailed = false
    @State private var ready = false

    var body: some View {
        Form {
            Section("Name") {
                TextField("Your name", text: $name, prompt: Theme.prompt("Your name"))
                    .textContentType(.givenName)
                    .textInputAutocapitalization(.words)
                    .frame(minHeight: 44)
            }
            .listRowBackground(Theme.formRow)

            Section {
                Picker(selection: $lang) {
                    Text("Not set").tag("")
                    ForEach(ProfileLanguage.all, id: \.code) { l in
                        Text(verbatim: l.native).tag(l.code)
                    }
                } label: { Text("Preferred language") }
                .frame(minHeight: 44)
            } footer: {
                Text("I talk to you in this language where I can. Some messages are in English for now.")
            }
            .listRowBackground(Theme.formRow)

            Section {
                ForEach(RoutineSlot.allCases) { slot in
                    RoutineRow(slot: slot, value: Binding(get: { routine[slot] }, set: { routine[slot] = $0 }))
                }
                TextField("Classes or work (optional)", text: Binding(get: { routine.classes ?? "" }, set: { routine.classes = $0 }), prompt: Theme.prompt("Classes or work (optional)"),
                          axis: .vertical)
                    .frame(minHeight: 44)
            } header: { Text("Your day") } footer: {
                Text("Reminders like “after dinner” use these times.")
            }
            .listRowBackground(Theme.formRow)

            Section {
                Button(action: save) {
                    Text("Save profile").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .disabled(!changed || saving)
            }
            .listRowBackground(Theme.formRow)

            Section {
                if medicines.isEmpty {
                    Text("No medicines yet.").foregroundStyle(Theme.secondary).frame(minHeight: 44)
                }
                MedicinesList(medicines: medicines, onEdit: { openForm($0) }, onDelete: { deleting = $0 })
                Button { openForm(nil) } label: {
                    Label("Add a medicine", systemImage: "plus.circle.fill").frame(minHeight: 44)
                }
            } header: { Text("Medicines") } footer: {
                Text("Saving a medicine offers a reminder on Today. Nothing rings until you say Yes.")
            }
            .listRowBackground(Theme.formRow)
        }
        .themedForm()
        .navigationTitle(Text("Profile"))
        .navigationBarTitleDisplayMode(.inline)
        .task { await load() }
        .sheet(item: $editing) { e in
            MedicineForm(original: e.medicine) { _ in
                toast = ProfileActions.savedMedicineToast
                Task { await reloadMedicines() }
            }
        }
        .medicineDeleteConfirmation($deleting) { ok in
            if !ok { deleteFailed = true }
            Task { await reloadMedicines() }
        }
        .alert("I couldn't delete this. Please try again.", isPresented: $deleteFailed) {
            Button("OK", role: .cancel) {}
        }
        .alert("I couldn't save this. Please try again.", isPresented: $errorShown) {
            Button("OK", role: .cancel) {}
        }
        .onChange(of: errorShown || deleteFailed) { _, open in PresentationGate.shared.set("profileError", open) }
        .toast($toast)
    }

    private var edited: Profile {
        var p = loaded ?? Profile()
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
        p.name = n.isEmpty ? nil : n
        p.lang = lang.isEmpty ? nil : lang
        p.routine = cleaned(routine)
        return p
    }

    private var changed: Bool { ready && edited != (loaded ?? Profile()) }

    private func cleaned(_ r: Routine) -> Routine? {
        var r = r
        let c = r.classes?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        r.classes = c.isEmpty ? nil : c
        return r == Routine() ? nil : r
    }

    private func load() async {
        let p = await CoreEngine.shared.profile()
        loaded = p
        name = p?.name ?? ""
        lang = p?.lang ?? ""
        routine = p?.routine ?? Routine()
        await reloadMedicines()
        ready = true
    }

    private func reloadMedicines() async {
        medicines = await CoreEngine.shared.medicines()
    }

    private func save() {
        guard !saving else { return }
        Task { _ = await saveProfile() }
    }

    @discardableResult
    private func saveProfile() async -> Bool {
        saving = true
        defer { saving = false }
        let p = edited
        do {
            try await CoreEngine.shared.setProfile(p)
            loaded = p
            await ProfileActions.changed()
            toast = String(localized: "Profile saved.")
            return true
        } catch {
            errorShown = true
            return false
        }
    }

    /// The form times medicines from the saved routine, so unsaved profile edits are saved first.
    private func openForm(_ m: Medicine?) {
        guard !saving else { return }
        Task {
            if changed, !(await saveProfile()) { return }
            editing = MedicineEditing(medicine: m)
        }
    }
}
