// The logo, the wordmark and the shared pills. Apple-classic: system fonts only.
import SwiftUI

enum Brand {
    static let record = Theme.record
}

/// The owner's artwork, unchanged, in a black rounded tile so it looks the same on white and on black.
struct BrandMark: View {
    var size: CGFloat = 28
    var body: some View {
        Image("BrandLogo")
            .resizable()
            .interpolation(.high)
            .scaledToFit()
            .frame(width: size, height: size)
            .background(Color.black)
            .clipShape(RoundedRectangle(cornerRadius: size * 0.22, style: .continuous))
            .accessibilityHidden(true)
    }
}

/// Large header shown at the top of the home screen (replaces the system large title).
struct BrandHeader: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .center, spacing: 10) {
                BrandMark(size: 34)
                (Text("Summaverick").font(.largeTitle.bold()).foregroundStyle(Theme.text)
                 + Text(" Everyday").font(.largeTitle.weight(.semibold)).foregroundStyle(Theme.secondary))
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            Text("Your personal note-taker. Everything stays on this device.")
                .font(.subheadline).foregroundStyle(Theme.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 20).padding(.top, 8).padding(.bottom, 4)
    }
}

/// Compact version for the inline navigation bar.
struct BrandTitleInline: View {
    var body: some View {
        HStack(spacing: 6) {
            BrandMark(size: 24)
            Text("Summaverick").font(.headline)
        }
    }
}

/// Apple-style pill. `prominent` is the main action on a screen, `plain` the rest; the others carry a meaning.
struct PillButtonStyle: ButtonStyle {
    enum Kind { case prominent, plain, warning, positiveFilled, positive, negative }
    let kind: Kind
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.body.weight(.semibold))
            .foregroundStyle(foreground(configuration.role))
            .multilineTextAlignment(.center)
            .padding(.horizontal, 14)
            .frame(minHeight: 44)
            .background(fill, in: Capsule())
            .overlay(Capsule().stroke(filled ? Color.clear : Theme.pillBorder, lineWidth: 1))
            .contentShape(Capsule())
            .opacity(!isEnabled ? 0.45 : configuration.isPressed ? 0.7 : 1)
    }

    private var filled: Bool { kind == .prominent || kind == .positiveFilled }

    private var fill: Color {
        switch kind {
        case .prominent: return Theme.primaryFill
        case .positiveFilled: return Theme.positiveIcon
        case .plain, .warning, .positive, .negative: return Theme.pillFill
        }
    }

    private func foreground(_ role: ButtonRole?) -> Color {
        switch kind {
        case .prominent: return Theme.primaryLabel
        case .positiveFilled: return Theme.positiveLabel
        case .positive: return Theme.positive
        case .negative: return Theme.negative
        case .warning: return Theme.warning
        case .plain: return role == .destructive ? Theme.warning : Theme.text
        }
    }
}

extension ButtonStyle where Self == PillButtonStyle {
    static var pillProminent: PillButtonStyle { PillButtonStyle(kind: .prominent) }
    static var pill: PillButtonStyle { PillButtonStyle(kind: .plain) }
    static var pillWarning: PillButtonStyle { PillButtonStyle(kind: .warning) }
    static var pillPositiveFilled: PillButtonStyle { PillButtonStyle(kind: .positiveFilled) }
    static var pillPositive: PillButtonStyle { PillButtonStyle(kind: .positive) }
    static var pillNegative: PillButtonStyle { PillButtonStyle(kind: .negative) }
}

extension View {
    /// A screen's own content on the page colour.
    func themedPage() -> some View {
        background(Theme.background.ignoresSafeArea())
    }

    /// A Form or List on the page colour, with its text in the theme's colours.
    func themedForm() -> some View {
        scrollContentBackground(.hidden)
            .background(Theme.formPage)
            .foregroundStyle(Theme.text, Theme.secondary, Theme.secondary)
    }
}
