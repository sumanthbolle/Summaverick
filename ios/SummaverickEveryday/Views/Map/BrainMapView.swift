// The map (approved mockup, 2026-11-10): what the person has kept, laid out around them in depth. "You" (or a
// recording's title) in the centre; people, topics, promises, decisions and open questions on branches around it.
// Branches start folded with their counts; tap one to open it. Drag to turn the floor (it settles to 15 degree
// steps on release), pinch or + and − to zoom. Nearer points come out larger and sharper, farther ones smaller and
// fainter; labels rise on taller stems so they never overlap (BrainMapLayout). The branches grow from the centre
// once, when the map first appears. Reduce Motion: a flat, still map (drag moves it). While notes are being
// written the map keeps still: no intro, no spin, no settle (the phone stays cool).
// Points still waiting in the review queue are on the map too, drawn lighter with a dashed outline and a "?";
// their card offers the queue's Keep and Wrong first (Keep makes them solid, Wrong takes them off).
// Tapping a memory opens its card with Ask about this, Remind me, Make a task, Done, Edit and Wrong. Tapping a
// person or topic opens its memories ("Who is this?" for a person). Fewer than 8 points: a plain grouped list.
import SwiftUI

// MARK: - branch looks

enum MapStyle {
    static func color(_ branch: String?) -> Color {
        switch branch {
        case "people": return Theme.mapPeople
        case "topics": return Theme.mapTopics
        case "promises": return Theme.mapPromises
        case "decisions": return Theme.mapDecisions
        case "questions": return Theme.mapQuestions
        default: return Theme.primaryFill
        }
    }

    static func icon(_ branch: String) -> String {
        switch branch {
        case "people": return "person.2.fill"
        case "topics": return "tag.fill"
        case "promises": return "hand.raised.fill"
        case "decisions": return "signpost.right.fill"
        default: return "questionmark.bubble.fill"
        }
    }

    static func title(_ branch: String) -> String {
        switch branch {
        case "people": return String(localized: "People")
        case "topics": return String(localized: "Topics")
        case "promises": return String(localized: "Promises")
        case "decisions": return String(localized: "Decisions")
        default: return String(localized: "Open questions")
        }
    }

    static func more(_ n: Int) -> String { String(localized: "\(n) more") }

    /// The memory type a branch of memories holds; nil for people and topics.
    static func memoryType(_ branch: String) -> String? {
        switch branch {
        case "promises": return "promise"
        case "decisions": return "decision"
        case "questions": return "question"
        default: return nil
        }
    }
}

/// What a tap on the map opens.
enum MapSelection: Identifiable, Equatable {
    case memory(Int64)
    case topic(id: Int64, name: String, person: Bool)
    case more(String)

    var id: String {
        switch self {
        case .memory(let id): return "m\(id)"
        case .topic(let id, _, _): return "t\(id)"
        case .more(let b): return "more.\(b)"
        }
    }

    static func of(_ item: MapItem, branch: String) -> MapSelection? {
        guard let id = item.rowID else { return nil }
        return item.isTopic ? .topic(id: id, name: item.label, person: branch == "people") : .memory(id)
    }
}

// MARK: - the screen

/// The whole map (noteID nil, Deep dive) or one recording's (the note screen's Map choice).
struct BrainMapScreen: View {
    var noteID: Int64? = nil
    /// The centre of a recording's map: its title.
    var centerTitle: String? = nil
    /// Inside a scrolling page: sideways drags turn the map, up and down scroll the page.
    var inScroll = false
    /// ▶ on a memory: open the recording at that second. Called once the card has closed.
    let onPlay: (Int64, Int) -> Void

    @Environment(\.scenePhase) private var scenePhase
    @State private var map: BrainMap?
    @State private var loaded = false
    @State private var readFailed = false
    @State private var selection: MapSelection?
    @State private var pendingPlay: (Int64, Int)?
    @State private var gateKey = "map.\(UUID().uuidString)"

    private var centerLabel: String {
        guard noteID != nil else { return String(localized: "You") }
        let t = centerTitle?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return t.isEmpty ? String(localized: "This recording") : t
    }

    var body: some View {
        Group {
            if let map, !map.isEmpty {
                if map.list {
                    BrainMapList(map: map) { selection = $0 }
                } else {
                    BrainMapCanvas(map: map, centerLabel: centerLabel, inScroll: inScroll) { selection = $0 }
                }
            } else if readFailed {
                HStack(spacing: 12) {
                    Label("I couldn't open this.", systemImage: "exclamationmark.triangle.fill")
                        .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
                    Spacer(minLength: 0)
                    Button("Try again") { Task { await reload() } }.buttonStyle(.pill).frame(minHeight: 44)
                }
                .padding()
                .frame(maxHeight: .infinity, alignment: .top)
            } else if loaded {
                ContentUnavailableView {
                    Label("Nothing in the knowledge graph yet", systemImage: "point.3.connected.trianglepath.dotted")
                } description: {
                    Text("Things I remember from your recordings show up here.")
                }
            } else {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .task(id: noteID) { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
        .onReceive(NotificationCenter.default.publisher(for: .notesChanged)) { _ in Task { await reload() } }
        .onChange(of: scenePhase) { _, p in if p == .active { Task { await reload() } } }
        .sheet(item: $selection, onDismiss: playPending) { sel in
            MapSelectionSheet(selection: sel, noteID: noteID, onPlay: { id, sec in
                pendingPlay = (id, sec)
                selection = nil
            }, onClose: { selection = nil })
        }
        .onChange(of: selection != nil) { _, open in PresentationGate.shared.set(gateKey, open) }
        .onDisappear { PresentationGate.shared.set(gateKey, false) }
    }

    private func playPending() {
        guard let p = pendingPlay else { return }
        pendingPlay = nil
        onPlay(p.0, p.1)
    }

    private func reload() async {
        if let m = await CoreEngine.shared.brainMap(noteID: noteID) {
            // Unchanged data keeps the same value, so the map does not redraw or refold for nothing.
            if m != map { map = m }
            readFailed = false
        } else {
            readFailed = map == nil
        }
        loaded = true
    }
}

// MARK: - the 3D map

/// Measured label sizes (unscaled), by node id.
private struct LabelSizes: PreferenceKey {
    static let defaultValue: [String: CGSize] = [:]
    static func reduce(value: inout [String: CGSize], nextValue: () -> [String: CGSize]) {
        value.merge(nextValue()) { $1 }
    }
}

/// Proposes at most `max` points of width and takes only what the content needs, so a short label stays short
/// and a long one wraps at `max`.
private struct CappedWidth: Layout {
    let max: CGFloat
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        subviews.first?.sizeThatFits(ProposedViewSize(width: Swift.min(proposal.width ?? max, max), height: nil)) ?? .zero
    }
    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        subviews.first?.place(at: CGPoint(x: bounds.midX, y: bounds.midY), anchor: .center,
                              proposal: ProposedViewSize(width: bounds.width, height: bounds.height))
    }
}

struct BrainMapCanvas: View {
    let map: BrainMap
    let centerLabel: String
    /// Inside a scrolling page (the note screen): drags only turn it sideways, so up and down still scroll the page.
    var inScroll = false
    let onSelect: (MapSelection) -> Void

    /// The intro plays once per app run, not every time the map comes back into view.
    @MainActor private static var introPlayed = false

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorScheme) private var scheme
    @State private var pipeline = ProcessingPipeline.shared

    @State private var expanded: Set<String> = []
    /// The camera at rest (or where a drag has it).
    @State private var camera = MapCamera()
    /// A settle or intro in flight: drawn frame by frame until it lands on `to`.
    @State private var anim: CameraAnimation?
    @State private var dragStart: MapCamera?
    @State private var zoomStart: Double?
    /// The person chose the zoom (pinch or + / −): refits keep it and only move the map, except that opening a
    /// branch may zoom out from it. Cleared by a tap on the centre.
    @State private var userZoomed = false
    /// A refit was asked for during a drag or pinch; it runs when the gesture ends.
    @State private var refitPending = false
    /// The note map: whether this gesture is the page's (vertical) or the map's (sideways), decided once.
    @State private var dragAxis: DragAxis = .undecided
    private enum DragAxis { case undecided, sideways, vertical }
    @State private var sizes: [String: CGSize] = [:]
    @State private var stageSize: CGSize = .zero

    private struct CameraAnimation: Equatable {
        let id = UUID()
        let from: MapCamera
        let to: MapCamera
        let start: Date
        let duration: Double
    }

    /// Notes being written (any model work): the map keeps still.
    private var still: Bool { pipeline.isWorking }
    /// Spinning and animation: not under Reduce Motion, not while notes are written.
    private var moving: Bool { !reduceMotion && !still }

    var body: some View {
        GeometryReader { geo in
            SwiftUI.TimelineView(.animation(minimumInterval: nil, paused: anim == nil)) { tl in
                let cam = shown(camera(at: tl.date))
                let nodes = BrainMapLayout.layout(map: map, size: geo.size, expanded: expanded, centerLabel: centerLabel,
                                                  branchLabel: MapStyle.title, moreLabel: MapStyle.more)
                let placed = BrainMapLayout.projected(nodes, size: geo.size, camera: cam, sizes: sizes)
                ZStack(alignment: .topLeading) {
                    ground(placed, cam: cam, size: geo.size)
                    ForEach(placed) { p in label(p) }
                }
                .accessibilityElement(children: .contain)
                .frame(width: geo.size.width, height: geo.size.height)
            }
            .contentShape(Rectangle())
            .gesture(drag.simultaneously(with: pinch), including: inScroll ? .none : .all)
            .simultaneousGesture(drag.simultaneously(with: pinch), including: inScroll ? .all : .none)
            .onAppear { stageSize = geo.size; start() }
            .onChange(of: geo.size) { _, s in stageSize = s; refit(animated: false, zoomOut: true) }
        }
        .clipped()
        .overlay(alignment: .bottomTrailing) { zoomButtons }
        .overlay(alignment: .bottom) {
            Text(moving ? "Drag to turn. Tap a branch to open it." : "Tap a branch to open it.")
                .font(.footnote).foregroundStyle(Theme.secondary)
                // One line, so it stays inside the band the labels keep clear of (BrainMapLayout.hintBand).
                .lineLimit(1).minimumScaleFactor(0.6)
                .dynamicTypeSize(...DynamicTypeSize.xxLarge)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 10).padding(.vertical, 4)
                .background(Capsule().fill(Theme.background.opacity(0.85)))
                .padding(.horizontal, 64).padding(.bottom, 8)
                .allowsHitTesting(false)
                .accessibilityHidden(true)
        }
        .onPreferenceChange(LabelSizes.self) { new in
            let rounded = new.mapValues { CGSize(width: ($0.width * 2).rounded() / 2, height: ($0.height * 2).rounded() / 2) }
            if rounded != sizes {
                sizes = rounded
                // New labels were measured (a branch just opened): fit again, also mid-animation (it then lands
                // on the corrected camera).
                refit(animated: anim != nil, zoomOut: true)
            }
        }
        // Notes started: whatever is moving lands where it was going, and nothing moves until they are done.
        .onChange(of: still) { _, s in if s { land() } }
        .onChange(of: reduceMotion) { _, _ in land(); camera.pan = .zero; refit(animated: false, zoomOut: true) }
        .onChange(of: map) { _, m in
            expanded = expanded.filter { k in m.branches.contains { $0.kind == k } }
            refit(animated: false, zoomOut: true)
        }
        .sensoryFeedback(.selection, trigger: expanded)
    }

    // MARK: camera

    private func camera(at date: Date) -> MapCamera {
        guard let a = anim else { return camera }
        let t = date.timeIntervalSince(a.start) / a.duration
        return t >= 1 ? a.to : BrainMapLayout.blend(a.from, a.to, t)
    }

    /// Reduce Motion lays the map flat; everything else is the camera as it is.
    private func shown(_ c: MapCamera) -> MapCamera {
        guard reduceMotion else { return c }
        return MapCamera(yaw: 0, tilt: 0, zoom: c.zoom, grow: 1, pan: c.pan)
    }

    private func animate(to target: MapCamera, duration: Double) {
        guard moving else { camera = target; anim = nil; return }
        let a = CameraAnimation(from: camera(at: .now), to: target, start: .now, duration: duration)
        anim = a
        camera = target
        Task {
            try? await Task.sleep(for: .seconds(duration))
            if anim?.id == a.id { anim = nil }
        }
    }

    /// Stops any animation where it was going.
    private func land() { if let a = anim { camera = a.to; anim = nil } }

    /// The first appearance in this app run: the branches grow from the centre once.
    private func start() {
        let target = fitted(camera, zoomOut: true)
        if moving && !Self.introPlayed {
            Self.introPlayed = true
            camera = MapCamera(yaw: target.yaw, tilt: target.tilt, zoom: target.zoom, grow: 0, pan: target.pan)
            animate(to: target, duration: 0.7)
        } else {
            camera = target
        }
    }

    private var nodes: [PlacedNode] {
        BrainMapLayout.layout(map: map, size: stageSize, expanded: expanded, centerLabel: centerLabel,
                              branchLabel: MapStyle.title, moreLabel: MapStyle.more)
    }

    /// `c` moved (and zoomed, by BrainMapLayout.refit's rules) so every label is inside the stage, clear of the hint
    /// and the zoom buttons. zoomOut: the change may need more room (a branch opened, labels measured, a new size).
    private func fitted(_ c: MapCamera, zoomOut: Bool) -> MapCamera {
        guard stageSize.width > 0 else { return c }
        let f = BrainMapLayout.refit(nodes, size: stageSize, camera: shown(c), sizes: sizes,
                                     userZoomed: userZoomed, mayZoomOut: zoomOut)
        var out = c
        out.zoom = f.zoom
        out.pan = f.pan
        out.grow = c.grow
        return out
    }

    private func refit(animated: Bool, zoomOut: Bool) {
        guard dragStart == nil, zoomStart == nil else { refitPending = true; return }
        let current = anim?.to ?? camera
        let target = fitted(current, zoomOut: zoomOut)
        guard target != current else { return }
        if animated { animate(to: target, duration: 0.35) } else { land(); camera = target }
    }

    // MARK: gestures

    private var drag: some Gesture {
        DragGesture(minimumDistance: 6)
            .onChanged { v in
                // In a scrolling page a mostly-vertical swipe is the page's: decided once per gesture.
                if inScroll {
                    if dragAxis == .undecided {
                        dragAxis = abs(v.translation.height) > abs(v.translation.width) ? .vertical : .sideways
                    }
                    if dragAxis == .vertical { return }
                }
                if dragStart == nil { land(); dragStart = camera }
                guard let s = dragStart else { return }
                if moving {
                    camera.yaw = s.yaw + Double(v.translation.width) * 0.45
                    // In a scrolling page, up and down belong to the page.
                    if !inScroll { camera.tilt = BrainMapLayout.clampTilt(s.tilt - Double(v.translation.height) * 0.2) }
                } else {
                    // Flat (Reduce Motion) or kept still: the drag moves the map instead of turning it.
                    camera.pan = CGSize(width: s.pan.width + v.translation.width,
                                        height: s.pan.height + (inScroll ? 0 : v.translation.height))
                }
            }
            .onEnded { _ in
                dragAxis = .undecided
                guard dragStart != nil else { return }
                dragStart = nil
                let pending = refitPending
                refitPending = false
                guard moving else {
                    if pending { refit(animated: false, zoomOut: true) }
                    return
                }
                var target = camera
                target.yaw = BrainMapLayout.settledYaw(camera.yaw)
                target.tilt = BrainMapLayout.restTilt
                animate(to: fitted(target, zoomOut: pending), duration: 0.42)
            }
    }

    private var pinch: some Gesture {
        MagnifyGesture()
            .onChanged { v in
                if zoomStart == nil { land(); zoomStart = camera.zoom }
                camera.zoom = BrainMapLayout.clampZoom((zoomStart ?? 1) * Double(v.magnification))
            }
            .onEnded { _ in
                zoomStart = nil
                userZoomed = true
                // Measured labels that arrived during the pinch may still zoom out from the chosen zoom.
                let pending = refitPending
                refitPending = false
                refit(animated: true, zoomOut: pending)
            }
    }

    private func zoom(by step: Double) {
        land()
        userZoomed = true
        var target = camera
        target.zoom = BrainMapLayout.clampZoom(camera.zoom + step)
        target = fitted(target, zoomOut: false)
        if moving { animate(to: target, duration: 0.25) } else { camera = target }
    }

    private var zoomButtons: some View {
        VStack(spacing: 8) {
            zoomButton("plus", "Zoom in", enabled: camera.zoom < BrainMapLayout.zoomRange.upperBound) { zoom(by: 0.15) }
            zoomButton("minus", "Zoom out", enabled: camera.zoom > BrainMapLayout.zoomRange.lowerBound) { zoom(by: -0.15) }
        }
        .padding(.trailing, 12).padding(.bottom, 36)
    }

    private func zoomButton(_ icon: String, _ title: LocalizedStringKey, enabled: Bool, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon).font(.body.weight(.semibold))
                .foregroundStyle(enabled ? Theme.text : Theme.secondary)
                .frame(width: 44, height: 44)
                .background(Circle().fill(Theme.card))
                .overlay(Circle().stroke(Theme.pillBorder, lineWidth: 1))
                .shadow(color: .black.opacity(scheme == .dark ? 0.5 : 0.08), radius: 8, y: 3)
        }
        .buttonStyle(.plain)
        .allowsHitTesting(enabled)
        .accessibilityLabel(Text(title))
    }

    /// Opens or folds a branch. Opening turns the map so the branch faces you and its items fan out across the
    /// screen (not while the map keeps still, or flat).
    private func toggle(_ branch: String) {
        let opening = !expanded.contains(branch)
        let change = {
            if opening { expanded.insert(branch) } else { expanded.remove(branch) }
        }
        if moving { withAnimation(.easeOut(duration: 0.3)) { change() } } else { change() }
        land()
        var target = camera
        if moving, opening, let i = map.branches.firstIndex(where: { $0.kind == branch }) {
            target.yaw = BrainMapLayout.facingYaw(branch: i, of: map.branches.count, from: camera.yaw)
            target.tilt = BrainMapLayout.restTilt
        }
        // Everything in view again: moved, and zoomed out from 1 if it must.
        animate(to: fitted(target, zoomOut: true), duration: opening ? 0.55 : 0.35)
    }

    // MARK: drawing

    /// The floor, its rings, the lines, the shadows, the stems and the points: everything under the labels.
    private func ground(_ placed: [ProjectedNode], cam: MapCamera, size: CGSize) -> some View {
        let (r1, r2) = BrainMapLayout.radii(size)
        let byID = Dictionary(uniqueKeysWithValues: placed.map { ($0.id, $0) })
        let dark = scheme == .dark
        let grow = cam.grow
        return Canvas { ctx, _ in
            func ring(_ r: Double) -> Path {
                var p = Path()
                for i in 0...72 {
                    let a = Double(i) / 72 * 2 * .pi
                    let q = BrainMapLayout.project(x: cos(a) * r, y: sin(a) * r, size: size, camera: MapCamera(
                        yaw: cam.yaw, tilt: cam.tilt, zoom: cam.zoom, grow: 1, pan: cam.pan)).point
                    if i == 0 { p.move(to: q) } else { p.addLine(to: q) }
                }
                p.closeSubpath()
                return p
            }
            // The floor: a soft disc that fades out at its edge.
            let disc = ring(r2 * 1.55)
            let c = BrainMapLayout.project(x: 0, y: 0, size: size, camera: cam).point
            ctx.fill(disc, with: .radialGradient(
                Gradient(colors: [Theme.text.opacity(dark ? 0.10 : 0.06), Theme.text.opacity(0.0)]),
                center: c, startRadius: 0, endRadius: r2 * 1.55 * cam.zoom))
            let dash = StrokeStyle(lineWidth: 1, dash: [3, 5])
            ctx.stroke(ring(r1), with: .color(Theme.hairline.opacity(0.5 * grow)), style: dash)
            ctx.stroke(ring(r2), with: .color(Theme.hairline.opacity(0.5 * grow)), style: dash)

            // Lines from the centre to the branches and from each branch to its items, nearest drawn last.
            for p in placed {
                guard let parent = p.node.parent, let from = byID[parent] else { continue }
                var path = Path()
                path.move(to: from.foot)
                path.addLine(to: p.foot)
                let width = (p.node.kind == .branch ? 2.4 : 1.5) * min(1.2, p.k)
                ctx.stroke(path, with: .color(MapStyle.color(p.node.branch).opacity(0.7 * p.opacity)),
                           style: StrokeStyle(lineWidth: width, lineCap: .round))
            }
            // Dotted arcs between shown memories that share a person, a topic or a meaning.
            for link in map.links {
                guard let a = placed.first(where: { $0.node.item?.ref == link.a && $0.node.kind == .item }),
                      let b = placed.first(where: { $0.node.item?.ref == link.b && $0.node.kind == .item }) else { continue }
                var path = Path()
                let control = CGPoint(x: (a.foot.x + b.foot.x) / 2, y: min(a.foot.y, b.foot.y) - 40)
                path.move(to: a.foot)
                path.addQuadCurve(to: b.foot, control: control)
                if let words = link.relationWords {
                    // An accepted relation: a solid line with its words at the top of the arc.
                    ctx.stroke(path, with: .color(Theme.accent.opacity(0.7)), style: StrokeStyle(lineWidth: 1.6, lineCap: .round))
                    let mid = CGPoint(x: (a.foot.x + 2 * control.x + b.foot.x) / 4, y: (a.foot.y + 2 * control.y + b.foot.y) / 4)
                    ctx.draw(Text(verbatim: words).font(.caption2.weight(.semibold)).foregroundStyle(Theme.accent), at: mid)
                    continue
                }
                ctx.stroke(path, with: .color(Theme.secondary.opacity(0.55)), style: StrokeStyle(lineWidth: 1.3, lineCap: .round, dash: [2, 5]))
            }
            // Shadows, stems and points, back to front.
            let tilt = cam.tilt * .pi / 180
            for p in placed {
                let col = MapStyle.color(p.node.branch)
                let rx = 9 * p.k, ry = 9 * p.k * cos(tilt) + 1
                ctx.fill(Path(ellipseIn: CGRect(x: p.foot.x - rx, y: p.foot.y - ry + 2, width: rx * 2, height: ry * 2)),
                         with: .color(.black.opacity(dark ? 0.55 : 0.10)))
                var stem = Path()
                let top = BrainMapLayout.stemTop(p)
                stem.move(to: top)
                stem.addLine(to: p.foot)
                ctx.stroke(stem, with: .linearGradient(Gradient(colors: [col.opacity(0.9), col.opacity(0.15)]),
                                                        startPoint: top, endPoint: p.foot),
                           lineWidth: 2)
                let r = 4 * p.k
                ctx.fill(Path(ellipseIn: CGRect(x: p.foot.x - r - 4, y: p.foot.y - r - 4, width: (r + 4) * 2, height: (r + 4) * 2)),
                         with: .color(col.opacity(0.2 * p.opacity)))
                ctx.fill(Path(ellipseIn: CGRect(x: p.foot.x - r, y: p.foot.y - r, width: r * 2, height: r * 2)), with: .color(col))
            }
        }
        .accessibilityHidden(true)
    }

    private func label(_ p: ProjectedNode) -> some View {
        let n = p.node
        // Farther labels are a touch softer; the flat map is all equally sharp.
        let blur = reduceMotion || dragStart != nil || zoomStart != nil ? 0 : min(0.4, max(0, -p.depth / 200))
        return Button { tap(n) } label: {
            MapLabel(node: n, centerLabel: centerLabel, expanded: n.branch.map(expanded.contains) ?? false)
                .fixedSize()
                .background(GeometryReader { g in Color.clear.preference(key: LabelSizes.self, value: [n.id: g.size]) })
                .scaleEffect(p.labelScale, anchor: .bottom)
                .blur(radius: blur)
                .frame(width: p.frame.width, height: p.frame.height, alignment: .bottom)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .opacity(p.opacity)
        .position(x: p.frame.midX, y: p.frame.midY)
        .zIndex(p.depth)
        .transition(.opacity)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: accessibilityText(n)))
        .accessibilityAddTraits(n.kind == .center ? [] : .isButton)
        .accessibilityValue(Text(verbatim: accessibilityState(n)))
        // VoiceOver reads the centre, then each branch followed by its items, whatever their depth.
        .accessibilitySortPriority(Double(10_000 - n.order))
    }

    private func accessibilityText(_ n: PlacedNode) -> String {
        switch n.kind {
        case .center: return centerLabel
        case .branch: return "\(MapStyle.title(n.branch ?? "")), \(n.count)"
        case .more: return MapStyle.more(n.count)
        case .item:
            let it = n.item
            return [it?.label ?? "", MapStyle.title(n.branch ?? "")].joined(separator: ", ")
        }
    }

    /// Open or folded for a branch; done, new, waiting to be checked and "not sure I heard this right" for an item
    /// (people and topics are items too: one whose memories all wait says so).
    private func accessibilityState(_ n: PlacedNode) -> String {
        switch n.kind {
        case .branch: return expanded.contains(n.branch ?? "") ? String(localized: "Open") : String(localized: "Folded")
        case .item:
            guard let it = n.item else { return "" }
            var words: [String] = []
            if it.done { words.append(String(localized: "Done")) }
            if it.isNew { words.append(String(localized: "New")) }
            if it.waiting { words.append(String(localized: "Waiting for you to check")) }
            if it.lowConfidence { words.append(String(localized: "Not sure I heard this right")) }
            return words.joined(separator: ", ")
        default: return ""
        }
    }

    private func tap(_ n: PlacedNode) {
        switch n.kind {
        case .center:
            // Back to the start: no zoom, no pan, the first angle.
            userZoomed = false
            let target = fitted(moving ? MapCamera() : MapCamera(yaw: camera.yaw, tilt: camera.tilt), zoomOut: true)
            if moving { animate(to: target, duration: 0.42) } else { camera = target }
        case .branch: if let b = n.branch { toggle(b) }
        case .more: if let b = n.branch { onSelect(.more(b)) }
        case .item: if let it = n.item, let b = n.branch, let s = MapSelection.of(it, branch: b) { onSelect(s) }
        }
    }
}

/// One label on the map, unscaled. Text is Theme.text on a card (or the main button's colours for the centre), so
/// it keeps the contrast rule; the branch colour is in the edges, icons and points.
private struct MapLabel: View {
    let node: PlacedNode
    let centerLabel: String
    let expanded: Bool
    @Environment(\.colorScheme) private var scheme

    private var color: Color { MapStyle.color(node.branch) }
    private var shadow: Color { .black.opacity(scheme == .dark ? 0.55 : 0.12) }

    var body: some View {
        Group {
            switch node.kind {
            case .center: center
            case .branch: branch
            case .item: item
            case .more: more
            }
        }
        .dynamicTypeSize(...DynamicTypeSize.accessibility1)
    }

    private var center: some View {
        CappedWidth(max: 180) {
            Text(verbatim: centerLabel).font(.headline).lineLimit(2).multilineTextAlignment(.center)
        }
        .foregroundStyle(Theme.primaryLabel)
        .padding(.horizontal, 16).padding(.vertical, 9)
        .background(Capsule().fill(Theme.primaryFill))
        .shadow(color: shadow, radius: 10, y: 4)
    }

    private var branch: some View {
        HStack(spacing: 6) {
            Image(systemName: MapStyle.icon(node.branch ?? ""))
                .font(.caption.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 24, height: 24)
                .background(Circle().fill(color))
            Text(verbatim: MapStyle.title(node.branch ?? "")).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.text)
            Text(verbatim: "\(node.count)").font(.subheadline.weight(.medium)).foregroundStyle(Theme.secondary)
            Image(systemName: "chevron.down").font(.caption2.weight(.bold)).foregroundStyle(Theme.secondary)
                .rotationEffect(.degrees(expanded ? 180 : 0))
        }
        .lineLimit(1)
        .padding(.leading, 5).padding(.trailing, 11).padding(.vertical, 5)
        .background(Capsule().fill(Theme.card))
        .overlay(Capsule().stroke(color, lineWidth: expanded ? 2 : 1.5))
        .shadow(color: shadow, radius: 8, y: 3)
    }

    private var item: some View {
        let it = node.item
        // Not checked yet: lighter text, a fainter stripe and card, a dashed outline, and a "?".
        let waiting = it?.waiting == true
        return VStack(alignment: .leading, spacing: 1) {
                CappedWidth(max: 112) {
                    (Text(verbatim: it?.label ?? "") + Text(verbatim: it?.lowConfidence == true ? " ?" : "").foregroundStyle(Theme.secondary))
                        .font(.footnote.weight(.medium))
                        .foregroundStyle(it?.done == true || waiting ? Theme.secondary : Theme.text)
                        .strikethrough(it?.done == true, color: Theme.secondary)
                        .lineLimit(3)
                        .multilineTextAlignment(.leading)
                }
                let n = it?.count ?? 0
                if n > 0 || waiting {
                    HStack(spacing: 6) {
                        if n > 0 {
                            Label { Text(verbatim: "\(n)") } icon: { Image(systemName: "square.stack.3d.up.fill") }
                                .labelStyle(.titleAndIcon)
                        }
                        // Its own mark, so a long label's "…" or a question's own "?" never hides it.
                        if waiting { Image(systemName: "questionmark.circle") }
                    }
                    .font(.caption2.weight(.semibold)).foregroundStyle(Theme.secondary)
                }
        }
        .padding(.leading, 14).padding(.trailing, 10).padding(.vertical, 7)
        .overlay(alignment: .leading) {
            Capsule().fill(color.opacity(waiting ? 0.45 : 1)).frame(width: 3).padding(.vertical, 8).padding(.leading, 6)
        }
        .background(RoundedRectangle(cornerRadius: 12).fill(Theme.card.opacity(waiting ? 0.6 : 1)))
        .overlay {
            if waiting {
                RoundedRectangle(cornerRadius: 12).stroke(color.opacity(0.7), style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
            } else {
                RoundedRectangle(cornerRadius: 12).stroke(color, lineWidth: it?.isNew == true ? 1.5 : 0)
            }
        }
        .shadow(color: shadow.opacity(waiting ? 0.5 : 1), radius: 8, y: 3)
    }

    private var more: some View {
        Text(verbatim: MapStyle.more(node.count))
            .font(.footnote.weight(.semibold)).foregroundStyle(Theme.link)
            .padding(.horizontal, 11).padding(.vertical, 6)
            .background(Capsule().fill(Theme.card))
            .overlay(Capsule().stroke(color, style: StrokeStyle(lineWidth: 1.5, dash: [3, 3])))
            .shadow(color: shadow, radius: 6, y: 2)
    }
}

// MARK: - fewer than 8 points: a plain list

struct BrainMapList: View {
    let map: BrainMap
    let onSelect: (MapSelection) -> Void

    var body: some View {
        List {
            Section {
                Text("Only a few things so far, so here they are as a list.")
                    .font(.subheadline).foregroundStyle(Theme.secondary)
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
            }
            ForEach(map.branches, id: \.kind) { b in
                Section {
                    ForEach(b.items, id: \.ref) { it in
                        Button { if let s = MapSelection.of(it, branch: b.kind) { onSelect(s) } } label: {
                            MapListRow(label: it.label, branch: b.kind, lowConfidence: it.lowConfidence, done: it.done, count: it.count,
                                       waiting: it.waiting)
                        }
                        .buttonStyle(.plain)
                    }
                    if b.more > 0 {
                        Button { onSelect(.more(b.kind)) } label: {
                            Text(verbatim: MapStyle.more(b.more)).font(.body.weight(.medium)).foregroundStyle(Theme.link)
                                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                } header: {
                    Text(verbatim: MapStyle.title(b.kind)).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.secondary)
                }
                .listRowBackground(Theme.card)
            }
        }
        .themedForm()
    }
}

/// A row with the branch's coloured point: used by the list, "more" and a person's or topic's memories.
struct MapListRow: View {
    let label: String
    let branch: String
    var lowConfidence = false
    var done = false
    var count = 0
    var detail: String? = nil
    /// Not checked yet: an open dashed point, lighter text and the queue's words.
    var waiting = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Group {
                if waiting {
                    Circle().stroke(MapStyle.color(branch), style: StrokeStyle(lineWidth: 1.5, dash: [2, 2]))
                } else {
                    Circle().fill(MapStyle.color(branch))
                }
            }
            .frame(width: 9, height: 9)
            .alignmentGuide(.firstTextBaseline) { $0[.bottom] - 1 }
            .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                (Text(verbatim: label) + Text(verbatim: lowConfidence ? " ?" : "").foregroundStyle(Theme.secondary))
                    .font(.body)
                    .foregroundStyle(done || waiting ? Theme.secondary : Theme.text)
                    .strikethrough(done, color: Theme.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let detail, !detail.isEmpty {
                    Text(verbatim: detail).font(.subheadline).foregroundStyle(Theme.secondary).lineLimit(2)
                }
                if waiting { MapWaitingLine() }
            }
            Spacer(minLength: 8)
            if count > 0 { Text(verbatim: "\(count)").font(.subheadline.weight(.medium)).foregroundStyle(Theme.secondary) }
            Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(Theme.secondary)
                .accessibilityHidden(true)
        }
        .frame(minHeight: 44)
        .contentShape(Rectangle())
        // One element: the label, the count and (not checked yet) "Waiting for you to check".
        .accessibilityElement(children: .combine)
    }
}

/// The review queue's words under a point that is not checked yet.
struct MapWaitingLine: View {
    var body: some View {
        Label("Waiting for you to check", systemImage: "hourglass")
            .font(.subheadline.weight(.medium)).foregroundStyle(Theme.caution)
    }
}

// MARK: - what a tap opens

/// The bottom sheet: a memory's card, a person's or topic's memories, or a branch's full list.
private struct MapSelectionSheet: View {
    let selection: MapSelection
    let noteID: Int64?
    let onPlay: (Int64, Int) -> Void
    /// Closes the whole sheet (from any page in it).
    let onClose: () -> Void
    @Environment(AppSettings.self) private var settings
    @Environment(Router.self) private var router

    var body: some View {
        NavigationStack {
            Group {
                switch selection {
                case .memory(let id): MapMemoryPanel(memoryID: id, onPlay: onPlay, onClose: onClose)
                case .topic(let id, let name, let person): TopicMemoriesList(topicID: id, name: name, person: person, noteID: noteID, onPlay: onPlay)
                case .more(let b): MoreList(branch: b, noteID: noteID, onPlay: onPlay)
                }
            }
            .navigationDestination(for: MapSelection.Route.self) { r in
                switch r {
                case .memory(let id): MapMemoryPanel(memoryID: id, onPlay: onPlay, onClose: onClose)
                case .topic(let id, let name, let person): TopicMemoriesList(topicID: id, name: name, person: person, noteID: noteID, onPlay: onPlay)
                }
            }
        }
        .presentationDetents(selection.isMemory ? [.medium, .large] : [.large])
        .presentationDragIndicator(.visible)
        .environment(settings)
        .environment(router)
    }
}

extension MapSelection {
    /// What a row inside the sheet pushes.
    enum Route: Hashable {
        case memory(Int64)
        case topic(id: Int64, name: String, person: Bool)
    }
    var isMemory: Bool { if case .memory = self { return true } else { return false } }
}

/// A reminder or to-do being offered for a memory: Yes / Edit / No.
private struct MapProposal: Identifiable, Equatable {
    let id = UUID()
    let kind: SuggestionKind
    let memory: BrainMemory

    /// The editor's card: the memory's words and its due date; no time is ever made up.
    var suggestion: Suggestion {
        var p = SuggestionPayload()
        p.text = memory.text
        if kind == .reminder {
            p.times = []
            p.repeat = "once"
            p.startDate = memory.dueDate
        } else {
            p.dueDate = memory.dueDate
            p.who = memory.who
        }
        let s = memory.sources.first
        return Suggestion(local: kind, payload: p, quote: s?.quote, noteID: s?.noteID, startSec: s?.startSec)
    }
}

/// A memory's card with the map's actions, as a sheet's page.
private struct MapMemoryPanel: View {
    let memoryID: Int64
    let onPlay: (Int64, Int) -> Void
    let onClose: () -> Void

    @Environment(\.dismiss) private var dismiss
    @Environment(AppSettings.self) private var settings
    @Environment(Router.self) private var router
    @State private var proposal: MapProposal?
    @State private var editing: MapProposal?
    @State private var saving = false
    @State private var failed = false
    @State private var toast: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                MemoryCardView(memoryID: memoryID, onPlay: onPlay, onRemoved: { dismiss() }) { action, m in
                    failed = false
                    switch action {
                    case .ask: askAbout(m)
                    case .remind: proposal = MapProposal(kind: .reminder, memory: m)
                    case .task: proposal = MapProposal(kind: .task, memory: m)
                    }
                }
                if let proposal { proposalCard(proposal) }
                if let toast {
                    Label { Text(verbatim: toast) } icon: { Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.positiveIcon) }
                        .font(.callout.weight(.medium)).foregroundStyle(Theme.text)
                        .padding(12).frame(maxWidth: .infinity, alignment: .leading)
                        .background(Theme.chip, in: RoundedRectangle(cornerRadius: 12))
                }
            }
            .padding(16)
        }
        .themedPage()
        .navigationBarTitleDisplayMode(.inline)
        .task(id: memoryID) { await CoreEngine.shared.touchMemory(memoryID) }
        .sheet(item: $editing) { p in
            SuggestionEditForm(suggestion: p.suggestion, noteTitle: p.memory.sources.first?.title) { _, _, draft in
                editing = nil
                Task { await save(p, draft) }
            }
            .environment(settings)
            .environment(router)
        }
    }

    private func proposalCard(_ p: MapProposal) -> some View {
        let reminder = p.kind == .reminder
        return VStack(alignment: .leading, spacing: 10) {
            Label(reminder ? "Remind me" : "Make a task", systemImage: reminder ? "bell.fill" : "checklist")
                .font(.subheadline.weight(.semibold)).foregroundStyle(Theme.secondary)
            Text(verbatim: p.memory.text).font(.body.weight(.medium)).fixedSize(horizontal: false, vertical: true)
            if let due = MemoryFormat.due(p.memory) {
                Text(verbatim: due).font(.callout).foregroundStyle(Theme.secondary)
            }
            if reminder {
                // A time is never made up: Yes opens the editor to choose one.
                Text("Choose a time, then save.").font(.callout).foregroundStyle(Theme.secondary)
            }
            if failed {
                Label("I couldn't save this. Please try again.", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout.weight(.medium)).foregroundStyle(Theme.warning)
            }
            MemoryActionButtons(busy: saving, buttons: [
                .init(title: "Yes", icon: "checkmark", tone: .positive, prominent: true) {
                    if reminder { editing = p } else { Task { await save(p, SuggestionDraft(p.suggestion.payload)) } }
                },
                .init(title: "Edit", icon: "pencil") { editing = p },
                .init(title: "No", icon: "xmark", tone: .negative) { proposal = nil },
            ])
        }
        .padding(16)
        .background(Theme.card, in: RoundedRectangle(cornerRadius: 16))
    }

    private func askAbout(_ m: BrainMemory) {
        let draft = String(localized: "Tell me more about: \(m.text)")
        onClose()
        // Once the sheet has gone (a note shown as a sheet closes too, so nothing is presented over another).
        Task {
            try? await Task.sleep(for: .milliseconds(450))
            router.ask(draft)
        }
    }

    private func save(_ p: MapProposal, _ d: SuggestionDraft) async {
        func opt(_ s: String) -> String? {
            let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
            return t.isEmpty ? nil : t
        }
        guard !saving else { return }
        saving = true
        failed = false
        let words = d.text.trimmingCharacters(in: .whitespacesAndNewlines)
        let source = p.memory.sources.first
        do {
            if p.kind == .reminder {
                _ = try await CoreEngine.shared.createReminder(
                    NewReminder(text: words, times: d.times, repeat: opt(d.repeatRule), startDate: p.memory.dueDate,
                                untilDate: d.untilDate, noteID: source?.noteID, startSec: source?.startSec,
                                medicineID: nil, sourceText: nil))
            } else {
                _ = try await CoreEngine.shared.createTask(
                    NewTask(text: words, who: opt(d.who), dueDate: d.dueDate, noteID: source?.noteID, startSec: source?.startSec))
            }
        } catch {
            NSLog("[sve] save from the map failed: %@", String(describing: error))
            saving = false
            failed = true
            return
        }
        saving = false
        proposal = nil
        await ReminderScheduler.shared.resync()
        NotificationCenter.default.post(name: .assistantChanged, object: nil)
        let message = p.kind == .reminder ? String(localized: "Reminder saved.") : String(localized: "To-do saved.")
        toast = message
        UIAccessibility.post(notification: .announcement, argument: message)
        if p.kind == .reminder { ReminderScheduler.shared.requestAuthorizationIfUndetermined() }
    }
}

/// A person's or topic's memories, strongest first, each with where it was said. "Who is this?" for a person.
private struct TopicMemoriesList: View {
    let topicID: Int64
    let name: String
    let person: Bool
    let noteID: Int64?
    let onPlay: (Int64, Int) -> Void

    @State private var memories: [BrainMemory]?
    @State private var failed = false

    var body: some View {
        List {
            if person {
                Section {
                    Text("Who is this?").font(.headline).foregroundStyle(Theme.text)
                        .listRowBackground(Color.clear)
                }
            }
            if let memories {
                if memories.isEmpty {
                    Text("Nothing kept about this yet.").foregroundStyle(Theme.secondary)
                        .listRowBackground(Theme.card)
                }
                ForEach(memories) { m in
                    NavigationLink(value: MapSelection.Route.memory(m.id)) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(MemoryFormat.typeLabel(m.type)).font(.caption.weight(.semibold)).foregroundStyle(Theme.secondary)
                            Text(verbatim: m.text).font(.body).foregroundStyle(m.waitingForCheck ? Theme.secondary : Theme.text)
                                .strikethrough(m.done, color: Theme.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                            if m.waitingForCheck { MapWaitingLine() }
                            ForEach(m.sources.prefix(2)) { s in sourceLine(s) }
                        }
                        .padding(.vertical, 4)
                        .frame(minHeight: 44, alignment: .leading)
                    }
                    .listRowBackground(Theme.card)
                }
            } else if failed {
                Label("I couldn't open this.", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(Theme.warning).listRowBackground(Theme.card)
            } else {
                ProgressView().frame(maxWidth: .infinity, minHeight: 88).listRowBackground(Color.clear)
            }
        }
        .themedForm()
        .navigationTitle(Text(verbatim: name))
        .navigationBarTitleDisplayMode(.inline)
        .task { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
    }

    /// The words in quotes, and the recording and moment they came from (▶ plays them).
    private func sourceLine(_ s: MemorySource) -> some View {
        let title = s.title.isEmpty ? String(localized: "Recording") : s.title
        return VStack(alignment: .leading, spacing: 2) {
            if !s.quote.isEmpty {
                Text(verbatim: "“\(s.quote)”").font(.subheadline.italic()).foregroundStyle(Theme.secondary).lineLimit(3)
            }
            Button { onPlay(s.noteID, s.startSec) } label: {
                Text(verbatim: "▶ \(title) @\(AssistantFormat.mmss(s.startSec))")
                    .font(.subheadline.weight(.medium)).foregroundStyle(Theme.accent)
                    .frame(minHeight: 44, alignment: .leading)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .accessibilityLabel(Text("Open the recording: \(title) at \(AssistantFormat.mmss(s.startSec))"))
        }
    }

    private func reload() async {
        if let m = await CoreEngine.shared.topicMemories(topicID, noteID: noteID) { memories = m; failed = false } else { failed = memories == nil }
    }
}

/// Everything a branch holds, not only what fits on the map.
private struct MoreList: View {
    let branch: String
    let noteID: Int64?
    let onPlay: (Int64, Int) -> Void

    private struct Row: Identifiable, Hashable {
        let route: MapSelection.Route
        let label: String
        var lowConfidence = false
        var done = false
        var waiting = false
        var id: String { "\(route)" }
    }

    @State private var rows: [Row]?

    var body: some View {
        List {
            if let rows {
                if rows.isEmpty { Text("Nothing kept about this yet.").foregroundStyle(Theme.secondary).listRowBackground(Theme.card) }
                ForEach(rows) { r in
                    NavigationLink(value: r.route) {
                        MapListRow(label: r.label, branch: branch, lowConfidence: r.lowConfidence, done: r.done, waiting: r.waiting)
                    }
                    .listRowBackground(Theme.card)
                }
            } else {
                ProgressView().frame(maxWidth: .infinity, minHeight: 88).listRowBackground(Color.clear)
            }
        }
        .themedForm()
        .navigationTitle(Text(verbatim: MapStyle.title(branch)))
        .navigationBarTitleDisplayMode(.inline)
        .task { await reload() }
        .onReceive(NotificationCenter.default.publisher(for: .assistantChanged)) { _ in Task { await reload() } }
    }

    private func reload() async {
        if let type = MapStyle.memoryType(branch) {
            // The map's own rule (core memory_on_map): checked, or waiting in the queue and neither health-related
            // nor a conflict; never a medicine's. The core leaves out wrong and superseded rows. Strongest first,
            // checked before waiting.
            let ms = await CoreEngine.shared.memories(MemoryFilter(noteID: noteID, types: [type]))
                .filter { m in
                    guard !m.isMedicine else { return false }
                    if m.waitingForCheck { return !m.health && m.conflictWith == nil }
                    return !m.queued && (m.review == "kept" || m.review == "corrected")
                }
            rows = ms.sorted {
                if $0.importance != $1.importance { return $0.importance > $1.importance }
                if $0.waitingForCheck != $1.waitingForCheck { return !$0.waitingForCheck }
                return $0.id > $1.id
            }
            .map { Row(route: .memory($0.id), label: $0.text, lowConfidence: $0.lowConfidence, done: $0.done, waiting: $0.waitingForCheck) }
        } else {
            let person = branch == "people"
            var topics = await CoreEngine.shared.topics(kind: person ? "person" : "topic")
            if let noteID {
                // One recording's: only those it said something about.
                var kept: [Topic] = []
                for t in topics.prefix(200) {
                    if let ms = await CoreEngine.shared.topicMemories(t.id, noteID: noteID), !ms.isEmpty { kept.append(t) }
                }
                topics = kept
            }
            rows = topics.map { Row(route: .topic(id: $0.id, name: $0.name, person: person), label: $0.name) }
        }
    }
}
