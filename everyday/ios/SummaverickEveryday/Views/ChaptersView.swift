// The chapters of a recording, as a strip at the top of the note: "0:00 Introduction · 12:30 Why stocks fell".
// Tapping one plays from its start with the note's player. Hidden when there are fewer than 2 chapters.
import SwiftUI

struct ChaptersView: View {
    let chapters: [Chapter]
    /// nil when the audio is gone: the chapters are shown, not playable.
    let onPlay: ((Int) -> Void)?

    var body: some View {
        if chapters.count >= 2 {
            VStack(alignment: .leading, spacing: 6) {
                Text("Chapters").font(.subheadline.weight(.semibold)).foregroundStyle(Theme.secondary)
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(Array(chapters.enumerated()), id: \.offset) { _, c in chip(c) }
                    }
                }
            }
        }
    }

    @ViewBuilder private func chip(_ c: Chapter) -> some View {
        let stamp = AssistantFormat.mmss(c.start_sec)
        let title = SectionText.clean(c.title)
        let label = HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(verbatim: stamp).font(.callout.monospacedDigit().weight(.semibold))
            Text(verbatim: title).font(.callout).lineLimit(1)
        }
        .padding(.horizontal, 12)
        .frame(minHeight: 44)
        .background(Theme.chip, in: Capsule())
        if let onPlay {
            Button { onPlay(c.start_sec) } label: { label.foregroundStyle(Theme.text) }
                .buttonStyle(.plain)
                .accessibilityLabel(Text(verbatim: "\(stamp) \(title)"))
                .accessibilityHint(Text("Play from \(stamp)"))
        } else {
            label.foregroundStyle(Theme.secondary)
                .accessibilityElement(children: .combine)
        }
    }
}
