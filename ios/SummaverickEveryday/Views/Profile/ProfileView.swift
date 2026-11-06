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
    @State private var ready = false

    var body: some View {
        Form {
            Section("Name") {
                TextField("Your name", text: $name)
                    .textContentType(.givenName)
                    .textInputAutocapitalization(.words)
                    .frame(minHeight: 44)
            }

            Section {
                Picker(selection: $lang) {
                    Text("Not set").tag("")
                    ForEach(ProfileLanguage.all, id: \.code) { l in
                        Text(verbatim: l.native).tag(l.code)
                    }
                } label: { Text("Preferred language") }
                .frame(minHeight: 44)
            } footer: {
                Text("Summaverick answers and writes reminders in this language.")
            }

            Section {
                ForEach(RoutineSlot.allCases) { slot in
                    RoutineRow(slot: slot, value: Binding(get: { routine[slot] }, set: { routine[slot] = $0 }))
                }
                TextField("Classes or work (optional)", text: Binding(get: { routine.classes ?? "" }, set: { routine.classes = $0 }),
                          axis: .vertical)
                    .frame(minHeight: 44)
            } header: { Text("Your day") } footer: {
                Text("Reminders like “after dinner” use these times.")
            }

            Section {
                Button(action: save) {
                    Text("Save profile").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .disabled(!changed || saving)
            }

            Section {
                if medicines.isEmpty {
                    Text("No medicines yet.").foregroundStyle(.secondary).frame(minHeight: 44)
                }
                MedicinesList(medicines: medicines, onEdit: { editing = MedicineEditing(medicine: $0) },
                              onDelete: { deleting = $0 })
                Button { editing = MedicineEditing(medicine: nil) } label: {
                    Label("Add a medicine", systemImage: "plus.circle.fill").frame(minHeight: 44)
                }
            } header: { Text("Medicines") } footer: {
                Text("Saving a medicine offers a reminder on Today. Nothing rings until you say Yes.")
            }
        }
        .navigationTitle(Text("Profile"))
        .task { await load() }
        .sheet(item: $editing) { e in
            MedicineForm(original: e.medicine, routine: currentRoutine) { _ in
                toast = ProfileActions.savedMedicineToast
                Task { await reloadMedicines() }
            }
        }
        .medicineDeleteConfirmation($deleting) { Task { await reloadMedicines() } }
        .alert("Could not save this. Please try again.", isPresented: $errorShown) {
            Button("OK", role: .cancel) {}
        }
        .onChange(of: errorShown) { _, open in PresentationGate.shared.set("profileError", open) }
        .toast($toast)
    }

    /// The routine the person sees now (saved or not), for timing a new medicine.
    private var currentRoutine: Routine? { cleaned(routine) }

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
        saving = true
        let p = edited
        Task {
            do {
                try await CoreEngine.shared.setProfile(p)
                loaded = p
                await ProfileActions.changed()
                toast = String(localized: "Profile saved.")
            } catch {
                errorShown = true
            }
            saving = false
        }
    }
}
