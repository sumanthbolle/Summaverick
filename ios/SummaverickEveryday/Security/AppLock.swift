// "Authenticate with the device password": on iPhone that is Face ID / Touch ID / passcode via LocalAuthentication.
// On the Summaverick Everyday device the equivalent is the device password on the local web UI.
import Foundation
import LocalAuthentication
import Observation
import SwiftUI

@MainActor
@Observable
final class AppLock {
    static let shared = AppLock()
    private(set) var isLocked = false
    private var authenticating = false

    private init() {
        if AppSettings.shared.requireUnlock { isLocked = true }
    }

    static var isAvailable: Bool {
        LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: nil)
    }

    func lockIfNeeded() {
        if AppSettings.shared.requireUnlock { isLocked = true }
    }

    func unlockIfNeeded() {
        guard isLocked, !authenticating else { return }
        Task { await authenticate() }
    }

    func authenticate() async {
        authenticating = true
        defer { authenticating = false }
        let ctx = LAContext()
        ctx.localizedCancelTitle = String(localized: "Cancel")
        do {
            // .deviceOwnerAuthentication falls back to the phone passcode when biometrics are unavailable.
            let ok = try await ctx.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: String(localized: "Unlock your notes"))
            if ok { withAnimation { isLocked = false } }
        } catch {
            // stay locked
        }
    }
}

struct LockScreen: View {
    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            VStack(spacing: 20) {
                Image(systemName: "lock.fill").font(.system(size: 44)).foregroundStyle(Theme.secondary)
                Text("Summaverick is locked").font(.title3.weight(.semibold))
                Button("Unlock") { Task { await AppLock.shared.authenticate() } }
                    .buttonStyle(.pillProminent)
            }
        }
    }
}
