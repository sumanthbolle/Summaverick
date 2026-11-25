// Where everything on the map goes. Pure (Foundation + CoreGraphics), so a swiftc check can run it outside Xcode.
//
// The map lies on a floor: "You" (or the recording) in the middle, the branches evenly around it, each branch's
// items fanning outward from it. layout() gives the floor positions; projected() turns them into screen points
// for a camera (spin, tilt, zoom), as the approved mockup does: the floor turns by the spin, tilts away from you,
// and nearer points come out larger. Labels stand on stems above their point; a label that would overlap one in
// front of it rises on a taller stem, so no two labels' tap frames ever overlap. Every tap frame is >= 44 x 44 pt.
import CoreGraphics
import Foundation

enum MapNodeKind: Equatable { case center, branch, item, more }

/// One node on the floor.
struct PlacedNode: Equatable, Identifiable {
    /// "c", "b.<branch>", "i.<branch>.<ref>", "more.<branch>"
    let id: String
    let kind: MapNodeKind
    /// The branch kind ("people" …); nil for the centre.
    let branch: String?
    /// Items only.
    let item: MapItem?
    /// Branch: everything it holds. More: how many are not shown.
    let count: Int
    /// The floor position, in points at zoom 1 (the centre is 0, 0).
    let x: Double
    let y: Double
    /// The node it hangs from (the line drawn to it); nil for the centre.
    let parent: String?
    /// The label's size before depth scaling: an estimate; the view passes the measured size instead.
    let labelSize: CGSize
    /// Reading order (VoiceOver): the centre, then each branch followed by its items and its "more".
    var order: Int = 0
}

/// How the floor is seen.
struct MapCamera: Equatable {
    /// Spin around the centre, in degrees.
    var yaw: Double = BrainMapLayout.startYaw
    /// How far the floor tips away from you, in degrees (0 = flat, seen from straight above).
    var tilt: Double = BrainMapLayout.restTilt
    /// 0.5 ... 1.8.
    var zoom: Double = 1
    /// The intro: 0 = everything at the centre, 1 = in place.
    var grow: Double = 1
    /// Moves the whole map (flat map only, and while the map is kept still).
    var pan: CGSize = .zero

    /// Reduce Motion: flat and still.
    static let flat = MapCamera(yaw: 0, tilt: 0)
}

/// A node as drawn: where its point is on screen, and where its label goes.
struct ProjectedNode: Equatable, Identifiable {
    let node: PlacedNode
    /// The point on the floor.
    let foot: CGPoint
    /// Perspective size (1 at the centre's depth; larger nearer).
    let k: Double
    /// The label's drawn scale (k, kept between 0.74 and 1.12 so far labels stay readable).
    let labelScale: Double
    /// + toward you.
    let depth: Double
    /// From the foot up to the label's bottom edge, in points on screen.
    let stem: Double
    /// The drawn label (scaled), centred in `frame`.
    let label: CGRect
    /// What takes the tap: the drawn label, grown to at least 44 x 44.
    let frame: CGRect
    /// Farther labels are fainter.
    let opacity: Double
    var id: String { node.id }
}

enum BrainMapLayout {
    static let minTap: Double = 44
    /// The camera at rest: tilted 44 degrees, as approved.
    static let restTilt: Double = 44
    static let startYaw: Double = -18
    /// Drag limits for the tilt.
    static let tiltRange: ClosedRange<Double> = 20...66
    static let zoomRange: ClosedRange<Double> = 0.5...1.8
    /// On release the spin settles to the nearest step.
    static let yawStep: Double = 15
    /// Gaps kept between neighbouring tap frames.
    static let gapX: Double = 2
    static let gapY: Double = 1
    /// The stem's height at rest, before any lift (points at labelScale 1).
    static func baseStem(_ kind: MapNodeKind) -> Double {
        switch kind {
        case .center: return 40
        case .branch: return 22
        case .item, .more: return 12
        }
    }

    // MARK: floor positions

    /// Ring radii for a screen size: the branches' ring, and the first ring of items.
    static func radii(_ size: CGSize) -> (branch: Double, item: Double) {
        let base = max(240, min(Double(size.width), Double(size.height) * 1.1))
        return (base * 0.25, base * 0.40)
    }

    /// The nodes to draw for `expanded` branches (items and "more" only for those). Same input, same output.
    static func layout(map: BrainMap, size: CGSize, expanded: Set<String>, centerLabel: String = "You",
                       branchLabel: (String) -> String = { $0 }, moreLabel: (Int) -> String = { "\($0) more" }) -> [PlacedNode] {
        let (r1, r2) = radii(size)
        var nodes = [PlacedNode(id: "c", kind: .center, branch: nil, item: nil, count: 0, x: 0, y: 0, parent: nil,
                                labelSize: estimate(centerLabel, .center))]
        let n = map.branches.count
        guard n > 0 else { return nodes }
        let sector = 2 * Double.pi / Double(n)
        for (i, b) in map.branches.enumerated() {
            let a = Double(i) * sector - Double.pi / 2
            let bid = "b.\(b.kind)"
            let title = branchLabel(b.kind)
            nodes.append(PlacedNode(id: bid, kind: .branch, branch: b.kind, item: nil, count: b.total,
                                    x: cos(a) * r1, y: sin(a) * r1, parent: "c",
                                    labelSize: estimate("\(title)  \(b.total)", .branch)))
            guard expanded.contains(b.kind) else { continue }
            let shown = b.items
            // Items fan over at most most of the branch's own sector, so neighbouring fans never cross.
            let spread = min(sector * 0.8, 0.45 * Double(max(0, shown.count - 1)))
            for (j, it) in shown.enumerated() {
                let t = shown.count == 1 ? 0 : Double(j) / Double(shown.count - 1) - 0.5
                let aa = a + t * spread
                // Alternate rings, so labels side by side start at different depths.
                let r = r2 + Double(j % 2) * r2 * 0.17
                nodes.append(PlacedNode(id: "i.\(b.kind).\(it.ref)", kind: .item, branch: b.kind, item: it, count: it.count,
                                        x: cos(aa) * r, y: sin(aa) * r, parent: bid,
                                        labelSize: estimate(it.label, .item)))
            }
            if b.more > 0 {
                let r = r2 * 1.36
                nodes.append(PlacedNode(id: "more.\(b.kind)", kind: .more, branch: b.kind, item: nil, count: b.more,
                                        x: cos(a) * r, y: sin(a) * r, parent: bid,
                                        labelSize: estimate(moreLabel(b.more), .more)))
            }
        }
        for i in nodes.indices { nodes[i].order = i }
        return nodes
    }

    /// A rough label size, used until the view has measured the real one (and by the swiftc check).
    static func estimate(_ text: String, _ kind: MapNodeKind) -> CGSize {
        // Telugu letters run wider than Latin ones at the same size.
        let wide = text.unicodeScalars.contains { (0x0C00...0x0C7F).contains($0.value) }
        let per: Double = wide ? 8.6 : 6.8
        let chars = Double(text.count)
        switch kind {
        case .center: return CGSize(width: chars * per * 1.15 + 32, height: 36)
        case .branch: return CGSize(width: chars * per * 1.05 + 26 + 22, height: 32)
        case .more: return CGSize(width: chars * per + 22, height: 30)
        case .item:
            let maxText = 110.0 - 20
            let textW = chars * per
            let lines = max(1, min(3, ceil(textW / maxText)))
            return CGSize(width: min(110, textW + 20), height: 12 + 15 * lines)
        }
    }

    // MARK: projection

    /// A floor point on screen: its position, perspective size and depth (+ toward you).
    static func project(x: Double, y: Double, size: CGSize, camera: MapCamera) -> (point: CGPoint, k: Double, depth: Double) {
        let s = camera.yaw * .pi / 180, tl = camera.tilt * .pi / 180
        let gx = x * camera.grow, gy = y * camera.grow
        let xr = gx * cos(s) - gy * sin(s), yr = gx * sin(s) + gy * cos(s)
        let depth = yr * sin(tl)
        let focal = radii(size).item * 4.8
        let k = focal / (focal - depth) * camera.zoom
        // Tilted, the centre sits a little low so the far side has room; flat, it sits in the middle.
        let cy = Double(size.height) * (0.5 + 0.14 * sin(tl))
        let px = Double(size.width) / 2 + xr * k + Double(camera.pan.width)
        let py = cy + yr * cos(tl) * k + Double(camera.pan.height)
        return (CGPoint(x: px, y: py), k, depth)
    }

    /// What the map keeps clear at the bottom of the stage: the hint line under the map, and the zoom buttons at
    /// the bottom right (BrainMapView draws them there with these numbers).
    static let hintBand: Double = 36
    static let zoomColumn = CGSize(width: 44 + 12 + 6, height: 44 * 2 + 8 + 36 + 6)
    static func reservedAreas(_ size: CGSize) -> [CGRect] {
        let w = Double(size.width), h = Double(size.height)
        return [CGRect(x: 0, y: h - hintBand, width: w, height: hintBand),
                CGRect(x: w - Double(zoomColumn.width), y: h - Double(zoomColumn.height),
                       width: Double(zoomColumn.width), height: Double(zoomColumn.height))]
    }
    /// The space labels must stay inside: the stage, less a margin, above the hint line.
    static func labelSpace(_ size: CGSize, margin: Double = 6) -> CGRect {
        CGRect(x: 0, y: margin, width: Double(size.width), height: max(0, Double(size.height) - hintBand - margin))
    }

    /// Every node on screen. The centre and the branches are placed first (nearest first), so they keep short stems
    /// next to their points; then the items and "more", nearest first. A label that would overlap one already placed,
    /// or the hint line or the zoom buttons, rises until it does not. Returned back to front (the order to draw them).
    /// `sizes`: measured label sizes by node id (unscaled); missing ones use the node's estimate.
    static func projected(_ nodes: [PlacedNode], size: CGSize, camera: MapCamera,
                          sizes: [String: CGSize] = [:], reserved: [CGRect]? = nil) -> [ProjectedNode] {
        let w = Double(size.width)
        func structural(_ n: PlacedNode) -> Bool { n.kind == .center || n.kind == .branch }
        let order = nodes.map { n -> (PlacedNode, (point: CGPoint, k: Double, depth: Double)) in
            (n, project(x: n.x, y: n.y, size: size, camera: camera))
        }
        // Centre and branches first, then nearest first; ties by id, so the result never depends on the input order.
        .sorted { a, b in
            if structural(a.0) != structural(b.0) { return structural(a.0) }
            return a.1.depth != b.1.depth ? a.1.depth > b.1.depth : a.0.id < b.0.id
        }

        var placed: [CGRect] = reserved ?? reservedAreas(size)
        let space = labelSpace(size)
        var out: [ProjectedNode] = []
        for (n, q) in order {
            let sc = max(0.74, min(1.12, q.k))
            let raw = sizes[n.id] ?? n.labelSize
            let lw = Double(raw.width) * sc, lh = Double(raw.height) * sc
            let fw = max(minTap, lw), fh = max(minTap, lh)
            // Kept inside the screen sideways when it can be (the stem still rises from the point).
            var cx = Double(q.point.x)
            if w > fw + 12 { cx = min(max(cx, fw / 2 + 6), w - fw / 2 - 6) }
            let base = baseStem(n.kind) * sc
            let py = Double(q.point.y)
            // The nearest free spot inside the label space: on a taller stem, a little to one side, or hanging
            // under the point (in that order of preference, by how far the label moves).
            var frame: CGRect?
            var lift = 0.0
            for spot in spots {
                let x = min(max(cx + spot.dx * fw, fw / 2), w - fw / 2)
                let f = spot.below
                    ? CGRect(x: x - fw / 2, y: py + base + spot.dy, width: fw, height: fh)
                    : CGRect(x: x - fw / 2, y: py - base - spot.dy - fh, width: fw, height: fh)
                if space.contains(f) && !placed.contains(where: { clash($0, f) }) { frame = f; lift = spot.dy; break }
            }
            if frame == nil {
                // No room inside: straight up on a taller stem until free (the caller zooms out or moves the map).
                lift = 0
                while true {
                    let f = CGRect(x: cx - fw / 2, y: py - base - lift - fh, width: fw, height: fh)
                    if !placed.contains(where: { clash($0, f) }) { frame = f; break }
                    lift += 6
                }
            }
            let fr = frame ?? .zero
            placed.append(fr)
            let hangs = Double(fr.minY) > py
            let label = CGRect(x: Double(fr.midX) - lw / 2, y: hangs ? Double(fr.minY) : Double(fr.maxY) - lh, width: lw, height: lh)
            let opacity = max(0.78, min(1, 1 + q.depth / 400))
            out.append(ProjectedNode(node: n, foot: q.point, k: q.k, labelScale: sc, depth: q.depth,
                                     stem: base + lift, label: label, frame: fr, opacity: opacity))
        }
        return out.sorted { a, b in a.depth != b.depth ? a.depth < b.depth : a.id > b.id }
    }

    /// Where a label may go, nearest first: dx in label widths, dy in points (up the stem, or down when below).
    private static let spots: [(dx: Double, dy: Double, below: Bool)] = {
        var all: [(dx: Double, dy: Double, below: Bool, cost: Double)] = []
        for dy in stride(from: 0.0, through: 600, by: 6) {
            for dx in [0.0, -0.5, 0.5, -1, 1, -1.5, 1.5] {
                all.append((dx, dy, false, dy + abs(dx) * 70))
                all.append((dx, dy, true, 36 + dy * 1.3 + abs(dx) * 70))
            }
        }
        // Stable: equal costs keep the order above (straight before sideways, left before right).
        return all.enumerated().sorted { $0.element.cost != $1.element.cost ? $0.element.cost < $1.element.cost : $0.offset < $1.offset }
            .map { ($0.element.dx, $0.element.dy, $0.element.below) }
    }()

    /// True when every label's tap frame is inside the label space.
    static func fits(_ p: [ProjectedNode], size: CGSize) -> Bool {
        let space = labelSpace(size)
        return p.allSatisfy { space.contains($0.frame) }
    }

    /// How far to move the map up or down so every label frame is inside the label space; nil when moving alone
    /// cannot do it (too tall, or out at both ends).
    static func shiftToFit(_ p: [ProjectedNode], size: CGSize) -> Double? {
        let space = labelSpace(size)
        guard let top = p.map({ Double($0.frame.minY) }).min(), let bottom = p.map({ Double($0.frame.maxY) }).max() else { return 0 }
        let down = Double(space.minY) - top, up = bottom - Double(space.maxY)
        if down > 0 && up > 0 { return nil }
        if down > 0 { return bottom + down <= Double(space.maxY) ? down : nil }
        if up > 0 { return top - up >= Double(space.minY) ? -up : nil }
        return 0
    }

    /// The camera that shows every label: moved up or down, and, when `zoomOut`, zoomed out in steps (from the
    /// camera's own zoom down to 0.5) until everything fits. Without `zoomOut` (the person chose the zoom) it only
    /// moves. Spin and tilt are kept. When nothing fits even at 0.5, the smallest zoom with the best move.
    static func fitted(_ nodes: [PlacedNode], size: CGSize, camera: MapCamera, sizes: [String: CGSize] = [:],
                       zoomOut: Bool) -> MapCamera {
        var c = camera
        c.grow = 1
        var zoom = clampZoom(camera.zoom)
        while true {
            c.zoom = zoom
            c.pan = CGSize(width: 0, height: 0)
            // Lifting depends a little on where the hint and zoom buttons fall, so a move is checked again.
            for _ in 0..<4 {
                let p = projected(nodes, size: size, camera: c, sizes: sizes)
                if fits(p, size: size) { return c }
                guard let d = shiftToFit(p, size: size), abs(d) > 0.5 else { break }
                c.pan.height += CGFloat(d)
            }
            if !zoomOut || zoom <= zoomRange.lowerBound + 1e-9 { break }
            zoom = max(zoomRange.lowerBound, zoom - 0.05)
        }
        // Nothing fits: the top stays in view (labels rise upward, so the top is what gets lost).
        let p = projected(nodes, size: size, camera: c, sizes: sizes)
        if let top = p.map({ Double($0.frame.minY) }).min(), top < Double(labelSpace(size).minY) {
            c.pan.height += CGFloat(Double(labelSpace(size).minY) - top)
        }
        return c
    }

    /// The camera after a change, by who chose the zoom.
    /// - Not chosen by the person (`userZoomed` false): the map's own zoom, from 1 down until everything fits.
    /// - Chosen by a pinch or the zoom buttons: that zoom is kept and the map only moves; a change that needs more
    ///   room (`mayZoomOut`: a branch opened, new labels measured, a new size) may zoom out from it, never back in.
    static func refit(_ nodes: [PlacedNode], size: CGSize, camera: MapCamera, sizes: [String: CGSize] = [:],
                      userZoomed: Bool, mayZoomOut: Bool) -> MapCamera {
        var start = camera
        start.pan = .zero
        if !userZoomed { start.zoom = 1 }
        return fitted(nodes, size: size, camera: start, sizes: sizes, zoomOut: !userZoomed || mayZoomOut)
    }

    static func clash(_ a: CGRect, _ b: CGRect) -> Bool {
        a.minX < b.maxX + gapX && a.maxX > b.minX - gapX && a.minY < b.maxY + gapY && a.maxY > b.minY - gapY
    }

    // MARK: gestures

    /// The spin on release: the nearest 15 degree step.
    static func settledYaw(_ yaw: Double) -> Double { (yaw / yawStep).rounded() * yawStep }

    /// The spin that turns branch `index` of `count` to the front (straight below the centre, nearest you), so its
    /// items fan out across the screen; the shortest way round from `current`. (Exact, not on a 15 degree step:
    /// five branches sit 72 degrees apart. The next drag settles to the steps again.)
    static func facingYaw(branch index: Int, of count: Int, from current: Double) -> Double {
        guard count > 0 else { return current }
        let a = Double(index) * 360 / Double(count) - 90
        var target = 90 - a
        while target - current > 180 { target -= 360 }
        while target - current < -180 { target += 360 }
        return target
    }

    /// Where a stem meets its label: straight above the point, or, when the label was kept inside the screen
    /// sideways, the nearest spot on the label's bottom edge.
    static func stemTop(_ p: ProjectedNode) -> CGPoint {
        let inset = min(10, Double(p.label.width) / 2)
        let x = min(max(Double(p.foot.x), Double(p.label.minX) + inset), Double(p.label.maxX) - inset)
        // A label hanging under its point (no room above) is joined at its top edge.
        if Double(p.label.minY) > Double(p.foot.y) { return CGPoint(x: x, y: Double(p.label.minY) + 2) }
        return CGPoint(x: x, y: Double(p.label.maxY) - 2)
    }

    static func clampZoom(_ z: Double) -> Double { min(max(z, zoomRange.lowerBound), zoomRange.upperBound) }
    static func clampTilt(_ t: Double) -> Double { min(max(t, tiltRange.lowerBound), tiltRange.upperBound) }

    /// Ease-out (cubic), as the mockup's settle and intro.
    static func ease(_ t: Double) -> Double { let k = min(1, max(0, t)); return 1 - pow(1 - k, 3) }

    /// A camera part way from `a` to `b` (t in 0...1, eased).
    static func blend(_ a: MapCamera, _ b: MapCamera, _ t: Double) -> MapCamera {
        let e = ease(t)
        func mix(_ x: Double, _ y: Double) -> Double { x + (y - x) * e }
        return MapCamera(yaw: mix(a.yaw, b.yaw), tilt: mix(a.tilt, b.tilt), zoom: mix(a.zoom, b.zoom),
                         grow: mix(a.grow, b.grow),
                         pan: CGSize(width: mix(Double(a.pan.width), Double(b.pan.width)),
                                     height: mix(Double(a.pan.height), Double(b.pan.height))))
    }
}
