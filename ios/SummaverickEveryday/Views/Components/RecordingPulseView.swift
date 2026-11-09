// Recording animation in the spirit of AirDrop's radar and the particle ring iPhone shows
// during Quick Start transfer: sonar rings breathe outward from the centre, a ring of particles
// orbits and swells with the microphone level. Pure SwiftUI Canvas, 60 fps, no assets.
import SwiftUI

struct RecordingPulseView: View {
    var level: Float          // 0…1 from the recorder's meter
    var active: Bool

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @ScaledMetric(relativeTo: .largeTitle) private var micGlyph: CGFloat = 36

    private let ringCount = 4
    private let ringPeriod: Double = 3.2       // seconds for one ring to travel centre → edge
    private let particleCount = 72

    var body: some View {
        SwiftUI.TimelineView(.animation(paused: !active || reduceMotion)) { timeline in
            Canvas(rendersAsynchronously: true) { ctx, size in
                let t = timeline.date.timeIntervalSinceReferenceDate
                let center = CGPoint(x: size.width / 2, y: size.height / 2)
                let maxR = min(size.width, size.height) / 2
                let lvl = CGFloat(min(max(level, 0), 1))

                // 1. sonar rings — evenly staggered, fading as they expand
                for i in 0..<ringCount {
                    let phase = ((t / ringPeriod) + Double(i) / Double(ringCount)).truncatingRemainder(dividingBy: 1)
                    let r = maxR * (0.28 + 0.72 * CGFloat(phase))
                    let alpha = (1 - phase) * (1 - phase) * 0.7
                    var ring = Path()
                    ring.addEllipse(in: CGRect(x: center.x - r, y: center.y - r, width: 2 * r, height: 2 * r))
                    ctx.stroke(ring, with: .color(Theme.record.opacity(alpha)), lineWidth: 1.5 + (1 - CGFloat(phase)) * 1.5)
                }

                // 2. particle ring — orbits slowly, each particle bobs on its own rhythm and the whole
                //    ring swells with the voice level
                let baseR = maxR * (0.42 + 0.10 * lvl)
                for i in 0..<particleCount {
                    let f = Double(i) / Double(particleCount)
                    let angle = f * 2 * .pi + t * 0.35
                    let bob = sin(t * 2.1 + f * 12.0) * 0.5 + 0.5          // 0…1
                    let r = baseR + CGFloat(bob) * maxR * (0.05 + 0.08 * lvl)
                    let p = CGPoint(x: center.x + cos(angle) * r, y: center.y + sin(angle) * r)
                    let d = 2.5 + CGFloat(bob) * 3.0
                    var dot = Path()
                    dot.addEllipse(in: CGRect(x: p.x - d / 2, y: p.y - d / 2, width: d, height: d))
                    ctx.fill(dot, with: .color(Theme.record.opacity(0.35 + 0.6 * bob)))
                }

                // 3. inner glow that breathes with the level
                let glowR = maxR * (0.30 + 0.06 * lvl)
                var glow = Path()
                glow.addEllipse(in: CGRect(x: center.x - glowR, y: center.y - glowR, width: 2 * glowR, height: 2 * glowR))
                ctx.fill(glow, with: .radialGradient(
                    Gradient(colors: [Theme.record.opacity(0.35 + 0.25 * lvl), Theme.record.opacity(0)]),
                    center: center, startRadius: 0, endRadius: glowR))
            }
            .animation(.easeOut(duration: 0.15), value: level)
        }
        .overlay {
            // 4. the solid record core with the mic glyph
            ZStack {
                Circle()
                    .fill(Brand.record)
                    .shadow(color: Brand.record.opacity(0.45), radius: 16, y: 6)
                // White on the red record fill is intentional.
                Image(systemName: "mic.fill")
                    .font(.system(size: micGlyph, weight: .semibold))
                    .foregroundStyle(.white)
            }
            .frame(width: 104, height: 104)
            .scaleEffect(active && !reduceMotion ? 1 + CGFloat(level) * 0.08 : 1)
            .animation(.spring(response: 0.25, dampingFraction: 0.6), value: level)
        }
        .accessibilityLabel(active ? "Recording" : "Not recording")
    }
}
