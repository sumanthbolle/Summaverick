// The app's root: three tabs (Today, Deep dive, Ask) and a red Record button in the tab bar
// (order: Today · Record · Deep dive · Ask).
// RootView owns the UI every tab shares: the settings sheet, the recording consent alert, the recorder,
// the note sheet opened from links and notifications, and the "deleted recording" alert.
import SwiftUI

/// Shared navigation state, injected into the environment by RootView.
@MainActor
@Observable
final class Router {
    enum Tab: Hashable { case today, deepDive, ask }

    var tab: Tab = .today
    /// Set to open a recording as RootView's sheet, from any tab; NoteDetailView starts at startSec.
    var openNote: (id: Int64, startSec: Int?)?
    /// Text to put into the Ask box when the Ask tab opens.
    var askDraft: String?
    var showSettings = false
    var showDeletedRecording = false

    func handle(_ route: ReminderRoute) {
        switch route {
        case .note(let id, let startSec): openNote = (id, startSec)
        case .profile: showSettings = true
        case .deletedRecording: showDeletedRecording = true
        }
    }
}

/// Screens with their own sheets or alerts report them here, so RootView never tries to present a
/// note on top of them (SwiftUI drops a presentation while another is up). A notification route waits
/// until this is empty.
@MainActor
@Observable
final class PresentationGate {
    static let shared = PresentationGate()
    private(set) var open: Set<String> = []
    func set(_ key: String, _ isOpen: Bool) {
        if isOpen { open.insert(key) } else { open.remove(key) }
    }
}

/// A moment in a recording: what a source link opens. Used by the note sheet (`sheet(item:)`) and as a
/// navigation value inside a tab (Deep dive, the Edit form).
struct NoteMoment: Identifiable, Hashable {
    let noteID: Int64
    let startSec: Int?
    var id: String { "\(noteID)@\(startSec.map(String.init) ?? "-")" }
}

struct RootView: View {
    @Environment(AppSettings.self) private var settings
    @State private var router = Router()
    @State private var scheduler = ReminderScheduler.shared
    @State private var lock = AppLock.shared
    @State private var gate = PresentationGate.shared
    @State private var recording = RecordingController.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var showRecorder = false
    @State private var showConsent = false
    /// The tab bar hides while a keyboard is up, so screens below it keep normal keyboard avoidance.
    @State private var keyboardUp = false

    var body: some View {
        ZStack {
            tabContent(.today) { TodayView() }
            tabContent(.deepDive) { DeepDiveView() }
            tabContent(.ask) { AskView() }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            if !keyboardUp { RootTabBar(selection: $router.tab) { startRecordingFlow() } }
        }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { _ in keyboardUp = true }
        .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in keyboardUp = false }
        .environment(router)
        .sheet(isPresented: $router.showSettings, onDismiss: handlePendingRoute) { SettingsView().environment(settings) }
        .sheet(item: noteLink, onDismiss: handlePendingRoute) { link in
            NavigationStack {
                NoteDetailView(noteID: link.noteID, startAt: link.startSec)
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) { Button("Done") { router.openNote = nil } }
                    }
            }
            .environment(settings)
        }
        .fullScreenCover(isPresented: $showRecorder) { RecordView().environment(settings) }
        .alert("Before you record", isPresented: $showConsent) {
            Button("I understand") { settings.consentAcknowledged = true; showRecorder = true }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Only record people who know they are being recorded and are fine with it. Recordings and notes stay on this phone; nothing is uploaded.")
        }
        .alert("This came from a deleted recording.", isPresented: $router.showDeletedRecording) {
            Button("OK", role: .cancel) {}
        }
        .onAppear { handlePendingRoute(); showConsentAskedByShortcut() }
        // A shortcut asked to record before the reminder was acknowledged, and the app came to the front for it.
        .onChange(of: recording.consentRequestedAt) { _, _ in showConsentAskedByShortcut() }
        .onChange(of: scenePhase) { _, _ in showConsentAskedByShortcut() }
        .onChange(of: scheduler.pendingRoute) { _, _ in handlePendingRoute() }
        .onChange(of: lock.isLocked) { _, _ in handlePendingRoute(); showConsentAskedByShortcut() }
        // These flip when a presentation starts to close; wait for it to finish before presenting.
        .onChange(of: showRecorder) { _, _ in handlePendingRouteSoon() }
        .onChange(of: showConsent) { _, _ in handlePendingRouteSoon() }
        .onChange(of: router.showDeletedRecording) { _, _ in handlePendingRouteSoon() }
        .onChange(of: gate.open) { _, _ in handlePendingRouteSoon() }
    }

    /// Keeps every tab alive (so its navigation and scroll position survive a tab switch) and shows one.
    @ViewBuilder
    private func tabContent<V: View>(_ tab: Router.Tab, @ViewBuilder _ content: () -> V) -> some View {
        let visible = router.tab == tab
        content()
            .opacity(visible ? 1 : 0)
            .allowsHitTesting(visible)
            .accessibilityHidden(!visible)
    }

    private var noteLink: Binding<NoteMoment?> {
        Binding(
            get: { router.openNote.map { NoteMoment(noteID: $0.id, startSec: $0.startSec) } },
            set: { router.openNote = $0.map { ($0.noteID, $0.startSec) } })
    }

    private func startRecordingFlow() {
        if settings.consentAcknowledged { showRecorder = true } else { showConsent = true }
    }

    /// The shortcut's request, once the app is in front and unlocked (a request from long ago is dropped).
    private func showConsentAskedByShortcut() {
        guard scenePhase == .active, !lock.isLocked, recording.consentRequestedAt != nil,
              recording.takeConsentRequest(), !showRecorder else { return }
        startRecordingFlow()
    }

    /// Handles the route once a closing presentation has finished animating away.
    private func handlePendingRouteSoon() {
        Task {
            try? await Task.sleep(for: .milliseconds(600))
            handlePendingRoute()
        }
    }

    /// A tapped notification sets `pendingRoute`. Wait until the app is unlocked (so a sheet never opens
    /// on top of the lock screen) and the recorder is closed (so a recording is never interrupted),
    /// then route and clear it.
    ///
    /// Presentations are sequenced, never swapped in one update: if RootView's own sheet or alert is up,
    /// it is closed first and the route is handled when it has gone (sheet onDismiss / onChange). While
    /// a child screen's own sheet, alert or recorder is open (PresentationGate), the route is kept.
    private func handlePendingRoute() {
        guard !lock.isLocked, !showRecorder, !showConsent, gate.open.isEmpty,
              let route = scheduler.pendingRoute else { return }
        if router.showSettings || router.openNote != nil || router.showDeletedRecording {
            router.showSettings = false
            router.openNote = nil
            router.showDeletedRecording = false
            return
        }
        scheduler.pendingRoute = nil
        router.handle(route)
    }
}

/// The bottom bar, in the owner-approved order: Today, the red Record button, Deep dive, Ask.
private struct RootTabBar: View {
    @Binding var selection: Router.Tab
    let onRecord: () -> Void

    var body: some View {
        HStack(alignment: .bottom, spacing: 0) {
            item(.today, "Today", "sun.max")
            recordButton
            item(.deepDive, "Deep dive", "books.vertical")
            item(.ask, "Ask", "bubble.left.and.bubble.right")
        }
        .padding(.top, 4)
        .padding(.bottom, 2)
        // Keep the bar at its natural height. Without this the record slot's Color.clear stretches to
        // the full screen and the .bar material behind it frosts every tab.
        .fixedSize(horizontal: false, vertical: true)
        .background {
            Rectangle().fill(.bar).ignoresSafeArea(edges: .bottom)
                .overlay(alignment: .top) { Divider() }
        }
    }

    private func item(_ tab: Router.Tab, _ title: LocalizedStringKey, _ icon: String) -> some View {
        let selected = selection == tab
        return Button { selection = tab } label: {
            VStack(spacing: 3) {
                Image(systemName: icon).font(.title3)
                Text(title).font(.caption).lineLimit(1).minimumScaleFactor(0.7)
            }
            .frame(maxWidth: .infinity, minHeight: 49)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(selected ? Color.accentColor : Color.secondary)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    /// A 52 pt red circle raised above the bar, with a caption under it. The slot keeps the bar's
    /// height; the button sits in an overlay so the circle can rise above the bar's top edge and the
    /// raised part still takes taps.
    private var recordButton: some View {
        Color.clear
            .frame(maxWidth: .infinity, minHeight: 49, maxHeight: 49)
            .overlay(alignment: .bottom) {
                Button(action: onRecord) {
                    VStack(spacing: 3) {
                        ZStack {
                            Circle().fill(Brand.record)
                                .shadow(color: Brand.record.opacity(0.35), radius: 6, y: 2)
                            Image(systemName: "mic.fill").font(.title2.weight(.semibold)).foregroundStyle(.white)
                        }
                        .frame(width: 52, height: 52)
                        .overlay(Circle().stroke(Color(.systemBackground), lineWidth: 3))
                        Text("Record").font(.caption.weight(.semibold)).foregroundStyle(Brand.record)
                            .lineLimit(1).minimumScaleFactor(0.7)
                    }
                    .padding(.bottom, 4)
                    .frame(minWidth: 72)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Record"))
            }
    }
}
