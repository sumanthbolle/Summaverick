// Brand tokens and the wordmark. Classic Apple feel: SF Pro, generous whitespace, one accent gradient,
// nothing decorative that doesn't carry meaning.
import SwiftUI

enum Brand {
    /// Apple-blue to cyan. Used for the mark, the wordmark accent and the recording pulse.
    static let gradient = LinearGradient(
        colors: [Color(red: 0.05, green: 0.45, blue: 1.0), Color(red: 0.30, green: 0.80, blue: 1.0)],
        startPoint: .topLeading, endPoint: .bottomTrailing)
    static let record = Color(red: 1.0, green: 0.23, blue: 0.19)   // systemRed, slightly warmer
}

/// The mark: a filled circle with three concentric arcs — "a voice, remembered".
struct BrandMark: View {
    var size: CGFloat = 28
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.24, style: .continuous)
                .fill(Brand.gradient)
            Canvas { ctx, sz in
                let c = CGPoint(x: sz.width * 0.40, y: sz.height * 0.5)
                var dot = Path(); dot.addEllipse(in: CGRect(x: c.x - sz.width * 0.085, y: c.y - sz.width * 0.085, width: sz.width * 0.17, height: sz.width * 0.17))
                ctx.fill(dot, with: .color(.white))
                for (i, r) in [0.22, 0.34, 0.46].enumerated() {
                    var arc = Path()
                    arc.addArc(center: c, radius: sz.width * r, startAngle: .degrees(-38), endAngle: .degrees(38), clockwise: false)
                    ctx.stroke(arc, with: .color(.white.opacity(1 - Double(i) * 0.22)), style: StrokeStyle(lineWidth: sz.width * 0.07, lineCap: .round))
                }
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

/// Large header shown at the top of the home screen (replaces the system large title).
struct BrandHeader: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .center, spacing: 10) {
                BrandMark(size: 34)
                (Text("Summaverick").font(.system(size: 34, weight: .bold, design: .default)).foregroundStyle(.primary)
                 + Text(" Everyday").font(.system(size: 34, weight: .semibold, design: .rounded)).foregroundStyle(Brand.gradient))
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            Text("Your personal note-taker. Everything stays on this device.")
                .font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 20).padding(.top, 8).padding(.bottom, 4)
    }
}

/// Compact version for the inline navigation bar.
struct BrandTitleInline: View {
    var body: some View {
        HStack(spacing: 6) {
            BrandMark(size: 18)
            Text("Summaverick").font(.headline)
        }
    }
}
