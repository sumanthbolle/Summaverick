// The end of the first-time setup: four skippable tips over the real tabs, then "try it", which opens the
// recorder. Settings → "Show me around again" replays the tips only.
import SwiftUI

/// One tip of the tour, on the real tab it is about.
enum TourTip: Int, CaseIterable {
    case record, today, deepDive, ask

    var tab: Router.Tab {
        switch self {
        case .record, .today: return .today
        case .deepDive: return .deepDive
        case .ask: return .ask
        }
    }

    var title: LocalizedStringKey {
        switch self {
        case .record: return "This red button is all you need."
        case .today: return "Today keeps your day in one place."
        case .deepDive: return "Every class, kept together."
        case .ask: return "Forgot something? Just ask me."
        }
    }

    var detail: LocalizedStringKey {
        switch self {
        case .record: return "Tap it, then put your phone in your pocket. I keep listening and writing, even when it's locked."
        case .today: return "Reminders waiting for your Yes, things to check and what's coming up. Nothing rings until you say Yes."
        case .deepDive: return "Your notes by topic and by person, so revising before the exam takes one tap."
        case .ask: return "\"What did sir say about the exam date?\" I'll answer and take you to that moment in the recording."
        }
    }
}

/// The scrim and the tip card, over the tab content. The tab bar under it lights only the tip's own tab.
struct TourOverlay: View {
    @Binding var tip: TourTip
    let onEnd: () -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let n = tip.rawValue + 1, total = TourTip.allCases.count
        let last = tip == TourTip.allCases.last
        ZStack(alignment: .bottom) {
            Color.black.opacity(0.55).ignoresSafeArea(edges: .top)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 8) {
                Text(tip.title).font(.title3.bold()).foregroundStyle(Theme.text)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
                Text(tip.detail).font(.body).foregroundStyle(Theme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 12) {
                    Text("\(n) of \(total)").font(.subheadline).foregroundStyle(Theme.secondary)
                    Spacer()
                    if !last {
                        Button("Skip", action: onEnd)
                            .font(.body.weight(.medium))
                            .foregroundStyle(Theme.link)
                            .frame(minWidth: 44, minHeight: 44)
                    }
                    Button(action: advance) {
                        Text(last ? "Got it" : "Next").font(.headline).padding(.horizontal, 10)
                    }
                    .buttonStyle(.pillProminent)
                }
                .padding(.top, 4)
            }
            .padding(18)
            .background(Theme.card, in: RoundedRectangle(cornerRadius: 18))
            .shadow(color: .black.opacity(0.3), radius: 20, y: 10)
            .padding(.horizontal, 16)
            .padding(.bottom, 20)
            .id(tip)
            .transition(.opacity)
        }
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
    }

    private func advance() {
        guard let next = TourTip(rawValue: tip.rawValue + 1) else { onEnd(); return }
        if reduceMotion { tip = next } else { withAnimation(.easeInOut(duration: 0.25)) { tip = next } }
    }
}

/// The last setup screen: try one recording now, or later.
struct TryItView: View {
    let onRecord: () -> Void
    let onLater: () -> Void
    @State private var models = ModelManager.shared

    var body: some View {
        let first = NotifyCopy.storedFirstName()
        VStack(spacing: 0) {
            ScrollView {
                VStack(spacing: 16) {
                    Group {
                        if let first { Text("Let's try one, \(first).") } else { Text("Let's try one.") }
                    }
                    .font(.title.bold()).foregroundStyle(Theme.text)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
                    Text("Tell me what you did today. Ten seconds is plenty, and I'll show you what I can do with it.")
                        .font(.body).foregroundStyle(Theme.secondary)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                    Button(action: onRecord) { RecordPulse() }
                        .buttonStyle(.plain)
                        .padding(.vertical, 28)
                        .accessibilityLabel(Text("Record"))
                    if models.status == .unsupported {
                        note(AIStatus.unsupportedMessage)
                    } else if !models.isReady {
                        note(String(localized: "My language pack is still downloading. I'll write the notes once it finishes."))
                    }
                    Text("Your practice note stays with your other notes. Delete it whenever you like.")
                        .font(.subheadline).foregroundStyle(Theme.secondary)
                        .multilineTextAlignment(.center)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(.horizontal, 24)
                .padding(.top, 48)
                .frame(maxWidth: .infinity)
            }
            VStack(spacing: 8) {
                SetupDots(lit: SetupFlow.dotCount)
                Button(action: onLater) {
                    Text("I'll try it later").font(.body.weight(.medium)).frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.plain)
                .foregroundStyle(Theme.link)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 16)
        }
        .themedPage()
        .onAppear { PresentationGate.shared.set("tryIt", true) }
        .onDisappear { PresentationGate.shared.set("tryIt", false) }
    }

    private func note(_ text: String) -> some View {
        Label { Text(verbatim: text) } icon: { Image(systemName: "arrow.down.circle") }
            .font(.subheadline).foregroundStyle(Theme.text)
            .fixedSize(horizontal: false, vertical: true)
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.card, in: RoundedRectangle(cornerRadius: 14))
    }
}

/// The big red record button with rings of sound leaving it. Reduce Motion: the rings hold still.
private struct RecordPulse: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @ScaledMetric(relativeTo: .largeTitle) private var glyph: CGFloat = 46
    @State private var out = false

    var body: some View {
        ZStack {
            ForEach(0..<2, id: \.self) { i in
                Circle().stroke(Theme.record, lineWidth: 2)
                    .scaleEffect(reduceMotion ? 1.25 + 0.25 * CGFloat(i) : (out ? 1.7 : 1))
                    .opacity(reduceMotion ? 0.35 - 0.15 * Double(i) : (out ? 0 : 0.7))
                    .animation(reduceMotion ? nil : .easeOut(duration: 2.8).repeatForever(autoreverses: false).delay(1.4 * Double(i)),
                               value: out)
            }
            Circle().fill(Theme.record)
            // White on the red record fill is intentional, as on every Record button.
            Image(systemName: "mic.fill").font(.system(size: glyph, weight: .semibold)).foregroundStyle(.white)
        }
        .frame(width: 112, height: 112)
        .contentShape(Circle())
        .onAppear { out = true }
    }
}
