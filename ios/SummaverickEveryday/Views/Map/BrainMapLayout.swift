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
        return (base * 0.19, base * 0.34)
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

    /// Every node on screen, front ones placed first so they keep their short stems; a label that would overlap
    /// one already placed rises until it does not. Returned back to front (the order to draw them).
    /// `sizes`: measured label sizes by node id (unscaled); missing ones use the node's estimate.
    static func projected(_ nodes: [PlacedNode], size: CGSize, camera: MapCamera,
                          sizes: [String: CGSize] = [:]) -> [ProjectedNode] {
        let w = Double(size.width)
        let order = nodes.map { n -> (PlacedNode, (point: CGPoint, k: Double, depth: Double)) in
            (n, project(x: n.x, y: n.y, size: size, camera: camera))
        }
        // Nearest first; ties by id, so the result never depends on the input order.
        .sorted { a, b in a.1.depth != b.1.depth ? a.1.depth > b.1.depth : a.0.id < b.0.id }

        var placed: [CGRect] = []
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
            var lift = 0.0
            var frame = CGRect.zero
            while true {
                // The tap frame's bottom sits where the label's bottom sits; a taller frame grows upward.
                let bottom = Double(q.point.y) - base - lift
                frame = CGRect(x: cx - fw / 2, y: bottom - fh, width: fw, height: fh)
                if !placed.contains(where: { clash($0, frame) }) { break }
                lift += 6
            }
            placed.append(frame)
            let label = CGRect(x: cx - lw / 2, y: Double(frame.maxY) - lh, width: lw, height: lh)
            let opacity = max(0.78, min(1, 1 + q.depth / 400))
            out.append(ProjectedNode(node: n, foot: q.point, k: q.k, labelScale: sc, depth: q.depth,
                                     stem: base + lift, label: label, frame: frame, opacity: opacity))
        }
        return out.reversed()
    }

    /// How far to move the map down so labels that rose above the top edge come into view, using only the
    /// empty room under the lowest label or point (never pushing anything off the bottom). 0 when all fit.
    static func fitShift(_ p: [ProjectedNode], size: CGSize, margin: Double = 8) -> Double {
        guard let top = p.map({ Double($0.frame.minY) }).min() else { return 0 }
        let bottom = p.map { max(Double($0.frame.maxY), Double($0.foot.y) + 8) }.max() ?? 0
        let over = max(0, margin - top)
        let room = max(0, Double(size.height) - margin - bottom)
        return min(over, room)
    }

    static func clash(_ a: CGRect, _ b: CGRect) -> Bool {
        a.minX < b.maxX + gapX && a.maxX > b.minX - gapX && a.minY < b.maxY + gapY && a.maxY > b.minY - gapY
    }

    // MARK: gestures

    /// The spin on release: the nearest 15 degree step.
    static func settledYaw(_ yaw: Double) -> Double { (yaw / yawStep).rounded() * yawStep }

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
