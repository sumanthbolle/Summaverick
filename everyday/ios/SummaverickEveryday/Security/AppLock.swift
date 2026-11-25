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
    @ScaledMetric(relativeTo: .largeTitle) private var lockGlyph: CGFloat = 44
    var body: some View {
        ZStack {
            Theme.background.ignoresSafeArea()
            VStack(spacing: 20) {
                Image(systemName: "lock.fill").font(.system(size: lockGlyph)).foregroundStyle(Theme.secondary)
                Text("Summaverick is locked").font(.title3.weight(.semibold))
                Button("Unlock") { Task { await AppLock.shared.authenticate() } }
                    .buttonStyle(.pillProminent)
            }
        }
    }
}

/// Notes, recordings, transcript checkpoints, chats and the activity log stay on this phone: their folders are
/// excluded from iCloud and computer backups. A folder's exclusion covers everything in it, including files written
/// later (the database's -wal/-shm, new recordings). Runs at every launch, so installs from before this rule are
/// excluded too; models are excluded file by file where ModelManager stores them.
enum PrivateStorage {
    /// The folders under Application Support that hold what was said.
    nonisolated static var folders: [URL] {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return [base.appendingPathComponent("Summaverick", isDirectory: true),
                base.appendingPathComponent("Chats", isDirectory: true)]
    }

    /// Creates each folder if needed and marks it excluded. Returns the folders still not excluded (empty: all done).
    @discardableResult
    nonisolated static func excludeFromBackup() -> [URL] {
        var failed: [URL] = []
        for var dir in folders {
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            var rv = URLResourceValues()
            rv.isExcludedFromBackup = true
            do { try dir.setResourceValues(rv) } catch { failed.append(dir) }
            dir.removeAllCachedResourceValues()
            if (try? dir.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup) != true,
               !failed.contains(dir) { failed.append(dir) }
        }
        if !failed.isEmpty { ActivityLog.log("backup_exclude_failed", ["folders": failed.map(\.lastPathComponent).joined(separator: ",")]) }
        return failed
    }
}
