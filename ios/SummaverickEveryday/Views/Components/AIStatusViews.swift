// How the AI download shows up: the Today card, the one-line status (profile setup, Deep dive) and the
// "still getting ready" panel where Ask and the assistant would be. No model names anywhere.
import SwiftUI

extension AIStatus {
    /// One line for Today, profile setup and the "getting ready" panels.
    var line: String {
        switch self {
        case .ready: return String(localized: "Ready")
        case .unsupported: return AIStatus.unsupportedMessage
        case .downloading(let p, let m?): return String(localized: "Getting Summaverick ready… \(p)% · about \(m) min left")
        case .downloading(let p, nil): return String(localized: "Getting Summaverick ready… \(p)%")
        case .waitingForWiFi: return String(localized: "Waiting for Wi-Fi")
        case .waitingForNetwork: return String(localized: "Waiting for a connection")
        case .noSpace: return String(localized: "Paused — no space")
        case .failed: return String(localized: "Download failed")
        case .notDownloaded: return String(localized: "Not downloaded")
        }
    }

    /// The status row in Settings.
    var settingsLine: String {
        switch self {
        case .ready(let bytes): return String(localized: "Ready · \(bytes.formatted(.byteCount(style: .file)))")
        case .downloading(let p, _): return String(localized: "Downloading \(p)%")
        default: return line
        }
    }

    /// Extra sentence under the line, when there is one.
    var detail: String? {
        if case .noSpace(let gb) = self { return String(localized: "Free up about \(gb) GB to download Summaverick's AI.") }
        return nil
    }

    var canRetry: Bool {
        switch self { case .failed, .notDownloaded, .noSpace: return true; default: return false }
    }

    var symbol: String {
        switch self {
        case .ready: return "checkmark.circle.fill"
        case .unsupported: return "memorychip"
        case .downloading: return "arrow.down.circle"
        case .waitingForWiFi: return "wifi"
        case .waitingForNetwork: return "wifi.slash"
        case .noSpace: return "externaldrive.badge.exclamationmark"
        case .failed: return "exclamationmark.triangle.fill"
        case .notDownloaded: return "arrow.down.circle"
        }
    }

    static var unsupportedMessage: String {
        String(localized: "This iPhone doesn't have enough memory for Summaverick's AI. You can still record and play back; notes need an iPhone with 6 GB of memory or more.")
    }
}

/// "Use mobile data for the download".
struct AICellularToggle: View {
    @State private var models = ModelManager.shared
    var body: some View {
        Toggle("Use mobile data for the download", isOn: $models.allowCellular)
            .frame(minHeight: 44)
    }
}

/// Today, while the AI is not ready: progress, what it is waiting for, Try again and mobile data.
struct AIDownloadCard: View {
    @State private var models = ModelManager.shared

    var body: some View {
        let status = models.status
        VStack(alignment: .leading, spacing: 10) {
            Label(status.line, systemImage: status.symbol)
                .font(.headline).fixedSize(horizontal: false, vertical: true)
            if let p = status.progress { ProgressView(value: p) }
            if let d = status.detail {
                Text(d).font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if status.canRetry {
                Button { models.startIfNeeded(userInitiated: true) } label: {
                    Text(status == .notDownloaded ? "Download again" : "Try again")
                        .font(.headline).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.borderedProminent)
            }
            AICellularToggle().font(.subheadline)
        }
        .padding(16)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
        .accessibilityElement(children: .contain)
    }
}

/// Shown once on a phone with too little memory; Settings keeps saying it.
struct AIUnsupportedCard: View {
    let dismiss: () -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(AIStatus.unsupportedMessage, systemImage: "memorychip")
                .font(.body).fixedSize(horizontal: false, vertical: true)
            Button(action: dismiss) {
                Text("OK").font(.headline).frame(maxWidth: .infinity, minHeight: 44)
            }
            .buttonStyle(.bordered)
        }
        .padding(16)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
    }
}

/// One footnote line (profile setup, Deep dive).
struct AIStatusLine: View {
    @State private var models = ModelManager.shared
    var body: some View {
        let status = models.status
        if !status.isReady {
            HStack(spacing: 8) {
                Image(systemName: status.symbol).accessibilityHidden(true)
                Text(status.line).lineLimit(3)
                Spacer(minLength: 0)
            }
            .font(.footnote).foregroundStyle(.secondary)
        }
    }
}

/// Where Ask or the assistant would be while the AI is not ready.
struct AIGettingReadyPanel: View {
    @State private var models = ModelManager.shared
    var body: some View {
        let status = models.status
        VStack(alignment: .leading, spacing: 10) {
            if status == .unsupported {
                Label(AIStatus.unsupportedMessage, systemImage: status.symbol)
                    .font(.body).fixedSize(horizontal: false, vertical: true)
            } else {
                Label("Summaverick is still getting ready", systemImage: "sparkles").font(.title3.bold())
                Text(status.line).font(.body).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                if let p = status.progress { ProgressView(value: p) }
                if let d = status.detail {
                    Text(d).font(.subheadline).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 16))
    }
}
