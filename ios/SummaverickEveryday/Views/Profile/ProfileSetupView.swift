// First-launch setup (spec §6.4): name → preferred language → routine → medicines. Every step can be
// skipped, and "Later" closes the sheet without saving. The profile is saved when the routine step ends,
// so leaving at the medicines step still keeps it. Notification permission is asked at the end.
import SwiftUI

struct ProfileSetupView: View {
    /// The sheet opens by itself at most once per launch.
    @MainActor static var shownThisLaunch = false

    @Environment(\.dismiss) private var dismiss
    @State private var step = 0
    @State private var name = ""
    @State private var lang: String?
    @State private var routine = Routine.defaults
    @State private var medicines: [Medicine] = []
    @State private var editing: MedicineEditing?
    @State private var deleting: Medicine?
    @State private var toast: String?
    @State private var saving = false
    @State private var errorShown = false
    @State private var deleteFailed = false

    private let stepCount = 4

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Group {
                    switch step {
                    case 0: nameStep
                    case 1: languageStep
                    case 2: routineStep
                    default: medicinesStep
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                buttons
            }
            .navigationTitle(Text("Set up Summaverick"))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    // From the medicines step on the profile is saved, so leaving still asks for notifications.
                    Button("Later") { step >= 3 ? finish() : dismiss() }.frame(minWidth: 44, minHeight: 44)
                }
                if step > 0 && step < 3 {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Back") { step -= 1 }.frame(minWidth: 44, minHeight: 44)
                    }
                }
            }
            .alert("Could not save this. Please try again.", isPresented: $errorShown) {
                Button("OK", role: .cancel) {}
            }
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
            .alert("Could not delete this. Please try again.", isPresented: $deleteFailed) {
                Button("OK", role: .cancel) {}
            }
            .toast($toast)
        }
        .interactiveDismissDisabled(saving)
        .onAppear { PresentationGate.shared.set("profileSetup", true) }
        .onDisappear { PresentationGate.shared.set("profileSetup", false) }
    }

    // MARK: steps

    private func header(_ title: LocalizedStringKey, _ detail: LocalizedStringKey) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Step \(step + 1) of \(stepCount)").font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
            Text(title).font(.title.bold()).fixedSize(horizontal: false, vertical: true)
                .accessibilityAddTraits(.isHeader)
            Text(detail).font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var nameStep: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header("What should Summaverick call you?", "Your name is used in greetings and to understand your recordings.")
                TextField("Your name", text: $name)
                    .textContentType(.givenName)
                    .textInputAutocapitalization(.words)
                    .font(.title3)
                    .padding(14)
                    .frame(minHeight: 44)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
                    .submitLabel(.next)
                    .onSubmit { next() }
            }
            .padding(20)
        }
    }

    private var languageStep: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header("Which language do you prefer?", "Summaverick talks to you in this language where it can. Some messages are in English for now.")
                VStack(spacing: 10) {
                    ForEach(ProfileLanguage.all, id: \.code) { l in
                        Button { lang = l.code } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(verbatim: l.native).font(.title3.weight(.semibold))
                                    if l.code != "en" { Text(l.name).font(.subheadline).foregroundStyle(.secondary) }
                                }
                                Spacer()
                                Image(systemName: lang == l.code ? "checkmark.circle.fill" : "circle")
                                    .font(.title2)
                                    .foregroundStyle(lang == l.code ? Color.accentColor : Color.secondary)
                                    .accessibilityHidden(true)
                            }
                            .padding(14)
                            .frame(maxWidth: .infinity, minHeight: 56, alignment: .leading)
                            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityElement(children: .combine)
                        .accessibilityAddTraits(lang == l.code ? [.isButton, .isSelected] : .isButton)
                    }
                }
            }
            .padding(20)
        }
    }

    private var routineStep: some View {
        Form {
            Section {
                header("What does your day look like?", "Reminders like “after dinner” use these times. Change any of them.")
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 8, leading: 0, bottom: 8, trailing: 0))
            }
            Section {
                ForEach(RoutineSlot.allCases) { slot in
                    RoutineRow(slot: slot, value: Binding(get: { routine[slot] }, set: { routine[slot] = $0 }))
                }
            }
            Section {
                TextField("e.g. college 9 to 4", text: Binding(get: { routine.classes ?? "" }, set: { routine.classes = $0 }),
                          axis: .vertical)
                    .frame(minHeight: 44)
            } header: { Text("Classes or work (optional)") }
        }
    }

    private var medicinesStep: some View {
        Form {
            Section {
                header("Do you take any medicines?", "Add them here and Summaverick will offer a reminder on Today. Nothing rings until you say Yes.")
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 8, leading: 0, bottom: 8, trailing: 0))
            }
            Section {
                MedicinesList(medicines: medicines, onEdit: { editing = MedicineEditing(medicine: $0) },
                              onDelete: { deleting = $0 })
                Button { editing = MedicineEditing(medicine: nil) } label: {
                    Label("Add a medicine", systemImage: "plus.circle.fill").font(.headline).frame(minHeight: 44)
                }
            }
        }
        .task { await reloadMedicines() }
    }

    // MARK: buttons

    private var buttons: some View {
        VStack(spacing: 8) {
            Button(action: next) {
                Text(step == 3 ? "Done" : "Next").font(.headline).frame(maxWidth: .infinity, minHeight: 50)
            }
            .buttonStyle(.borderedProminent)
            .disabled(saving)
            if step < 3 || medicines.isEmpty {
                Button(action: skip) {
                    Text("Skip this step").frame(maxWidth: .infinity, minHeight: 44)
                }
                .disabled(saving)
            }
            AIStatusLine().padding(.top, 4)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        .background(.bar)
    }

    private func next() {
        switch step {
        case 0, 1: step += 1
        case 2: saveProfile(withRoutine: true)
        default: finish()
        }
    }

    private func skip() {
        switch step {
        case 0: name = ""; step = 1
        case 1: lang = nil; step = 2
        case 2: saveProfile(withRoutine: false)
        default: finish()
        }
    }

    /// End of the routine step: save what the person gave. A skipped routine is left out, not defaulted.
    private func saveProfile(withRoutine: Bool) {
        guard !saving else { return }
        saving = true
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        var r = routine
        let classes = r.classes?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        r.classes = classes.isEmpty ? nil : classes
        let profile = Profile(name: trimmed.isEmpty ? nil : trimmed, lang: lang, routine: withRoutine ? r : nil)
        Task {
            do {
                try await CoreEngine.shared.setProfile(profile)
                await ProfileActions.changed()
                saving = false
                step = 3
            } catch {
                saving = false
                errorShown = true
            }
        }
    }

    private func finish() {
        guard !saving else { return }
        saving = true
        Task {
            _ = await ReminderScheduler.shared.requestAuthorization()
            saving = false
            dismiss()
        }
    }

    private func reloadMedicines() async {
        medicines = await CoreEngine.shared.medicines()
    }
}
