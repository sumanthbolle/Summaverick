/*
 * particle-s.js — the folded metal S from the Summaverick mark, drawn as a
 * ribbon of light points in the hero frame.
 *
 * The S is one strip folded twice, as in the logo render: a top plate, a fold
 * on the left, a diagonal, a fold on the right, and a bottom plate. The strip
 * is sampled on a regular grid along its length and across its width, each
 * point is turned in 3D and lit like brushed titanium (a broad diffuse term,
 * a tight highlight, and a glint that travels along the strip), and drawn as
 * a dot. When the frame first enters view the points gather out of a loose
 * cloud into the S, once. After that the S turns slowly and leans toward the
 * pointer.
 *
 * Decorative: the canvas is aria-hidden. Under reduced motion the S is drawn
 * once, assembled, at its resting angle.
 */

const ALONG = 260;
const ACROSS = 30;
const DUST = 160;
const HALF_WIDTH = 0.34;
const GATHER_MS = 1900;

// Centreline of the strip in the plane of the mark (y up), from the end of
// the top plate to the end of the bottom plate. Extra points on the straight
// runs keep the spline from bowing them.
const SPINE = [
  [0.62, 0.92], [0.34, 0.8], [0.06, 0.68], [-0.22, 0.56],
  [-0.42, 0.46], [-0.5, 0.33], [-0.42, 0.22], [-0.24, 0.13],
  [0.0, 0.02], [0.24, -0.09],
  [0.42, -0.19], [0.5, -0.32], [0.42, -0.44], [0.22, -0.53],
  [-0.06, -0.65], [-0.34, -0.77], [-0.62, -0.89],
];

function catmull(p0, p1, p2, p3, t) {
  const t2 = t * t;
  const t3 = t2 * t;
  return [0, 1].map(
    (k) =>
      0.5 *
      (2 * p1[k] +
        (-p0[k] + p2[k]) * t +
        (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
        (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3)
  );
}

// Samples the spine evenly by arc length so the dots keep one spacing
// through the tight folds and along the plates.
function sampleSpine(count) {
  const dense = [];
  const pts = [SPINE[0], ...SPINE, SPINE[SPINE.length - 1]];
  for (let i = 1; i < pts.length - 2; i++) {
    for (let s = 0; s < 40; s++) dense.push(catmull(pts[i - 1], pts[i], pts[i + 1], pts[i + 2], s / 40));
  }
  dense.push(SPINE[SPINE.length - 1]);
  const lengths = [0];
  for (let i = 1; i < dense.length; i++) {
    lengths.push(lengths[i - 1] + Math.hypot(dense[i][0] - dense[i - 1][0], dense[i][1] - dense[i - 1][1]));
  }
  const total = lengths[lengths.length - 1];
  const out = [];
  let j = 0;
  for (let i = 0; i < count; i++) {
    const target = (i / (count - 1)) * total;
    while (j < lengths.length - 2 && lengths[j + 1] < target) j++;
    const f = (target - lengths[j]) / (lengths[j + 1] - lengths[j] || 1);
    const a = dense[j];
    const b = dense[j + 1];
    const x = a[0] + (b[0] - a[0]) * f;
    const y = a[1] + (b[1] - a[1]) * f;
    const tx = b[0] - a[0];
    const ty = b[1] - a[1];
    const tl = Math.hypot(tx, ty) || 1;
    out.push({ x, y, tx: tx / tl, ty: ty / tl, u: i / (count - 1) });
  }
  return out;
}

function buildPoints() {
  const spine = sampleSpine(ALONG);
  const points = [];
  for (const s of spine) {
    for (let j = 0; j < ACROSS; j++) {
      const v = (j / (ACROSS - 1)) * 2 - 1;
      // A scattered start position for the gather, on a loose shell.
      const a = Math.random() * Math.PI * 2;
      const b = Math.acos(Math.random() * 2 - 1);
      const r = 1.4 + Math.random() * 1.2;
      points.push({
        x: s.x,
        y: s.y,
        z: v * HALF_WIDTH,
        nx: s.ty,
        ny: -s.tx,
        u: s.u,
        v,
        sx: r * Math.sin(b) * Math.cos(a),
        sy: r * Math.sin(b) * Math.sin(a),
        sz: r * Math.cos(b),
        delay: Math.random() * 0.35 + s.u * 0.25,
      });
    }
  }
  return points;
}

function buildDust() {
  return Array.from({ length: DUST }, () => ({
    x: (Math.random() * 2 - 1) * 1.5,
    y: (Math.random() * 2 - 1) * 1.1,
    z: (Math.random() * 2 - 1) * 0.8,
    speed: 0.02 + Math.random() * 0.05,
    phase: Math.random() * Math.PI * 2,
    size: Math.random() < 0.15 ? 1.4 : 0.9,
  }));
}

const easeOut = (t) => 1 - Math.pow(1 - t, 3);

function setup(canvas) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  const host = canvas.closest("[data-particle-host]") || canvas.parentElement;
  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  const points = buildPoints();
  const dust = buildDust();
  // Light from the upper left, in front: the side the logo render is lit from.
  const L = (() => {
    const l = [-0.45, 0.62, 0.64];
    const n = Math.hypot(...l);
    return l.map((c) => c / n);
  })();

  let w = 0;
  let h = 0;
  let dpr = 1;
  let visible = false;
  let raf = 0;
  let gatherStart = 0;
  let gathered = reduced.matches;
  let px = 0;
  let py = 0;
  let tx = 0;
  let ty = 0;
  const start = performance.now();

  const resize = () => {
    const box = canvas.getBoundingClientRect();
    dpr = Math.min(2, window.devicePixelRatio || 1);
    w = box.width;
    h = box.height;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  };

  const draw = (now) => {
    const t = (now - start) / 1000;
    const still = reduced.matches;
    let g = 1;
    if (!gathered) {
      if (!gatherStart) gatherStart = now;
      g = Math.min(1, (now - gatherStart) / GATHER_MS);
      if (g >= 1) gathered = true;
    }

    px += (tx - px) * 0.06;
    py += (ty - py) * 0.06;
    const yaw = (still ? -0.32 : -0.32 + Math.sin(t * 0.32) * 0.26) + px * 0.45;
    const pitch = 0.62 + (still ? 0 : Math.sin(t * 0.21) * 0.05) + py * 0.22;
    const cy = Math.cos(yaw);
    const sy = Math.sin(yaw);
    const cp = Math.cos(pitch);
    const sp = Math.sin(pitch);
    // Sized and centred in the space between the frame's status bar (top
    // 56px) and its composer (bottom 96px).
    const room = h - 56 - 96;
    const scale = Math.min(w * 0.26, room * 0.46);
    const ox = w / 2;
    const oy = 56 + room / 2;
    const glint = still ? -1 : ((t * 0.16) % 1.6) - 0.3;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    for (const d of dust) {
      const dx = d.x + Math.sin(t * d.speed * 6 + d.phase) * 0.06;
      const wy = ((d.y + 1.1 + (still ? 0 : t * d.speed)) % 2.2) - 1.1;
      const persp = 3 / (3 + d.z);
      ctx.fillStyle = `rgba(214, 219, 230, ${0.12 * g})`;
      ctx.fillRect(ox + dx * scale * persp * 1.2, oy - wy * scale * persp, d.size, d.size);
    }

    for (const p of points) {
      let x = p.x;
      let y = p.y;
      let z = p.z;
      if (g < 1) {
        const k = easeOut(Math.min(1, Math.max(0, (g - p.delay * 0.5) / (1 - p.delay * 0.5))));
        x = p.sx + (p.x - p.sx) * k;
        y = p.sy + (p.y - p.sy) * k;
        z = p.sz + (p.z - p.sz) * k;
      }
      // Yaw about y, then pitch about x: the strip's width tips toward the
      // viewer so the plates read as faces, as in the render.
      const x1 = x * cy + z * sy;
      const z1 = -x * sy + z * cy;
      const y2 = y * cp - z1 * sp;
      const z2 = y * sp + z1 * cp;
      const nx1 = p.nx * cy;
      const nz1 = -p.nx * sy;
      const ny2 = p.ny * cp - nz1 * sp;
      const nz2 = p.ny * sp + nz1 * cp;

      const diffuse = Math.abs(nx1 * L[0] + ny2 * L[1] + nz2 * L[2]);
      // Highlight toward a viewer straight ahead: the half vector of L and z.
      const hx = L[0];
      const hy = L[1];
      const hz = L[2] + 1;
      const hl = Math.hypot(hx, hy, hz);
      const spec = Math.pow(Math.abs(nx1 * hx + ny2 * hy + nz2 * hz) / hl, 28);
      const brushed = 0.06 * Math.sin(p.v * 19 + p.u * 3);
      const sweep = Math.exp(-((p.u - glint) ** 2) / 0.004) * 0.7;
      let b = 0.16 + diffuse * 0.5 + spec * 0.85 + brushed + sweep;
      // Points toward the far edge of the strip sit a little darker.
      b *= 0.78 + 0.22 * (1 - (p.v + 1) / 2);
      b = Math.min(1.2, Math.max(0.05, b)) * (0.25 + 0.75 * g);

      const persp = 3.2 / (3.2 - z2);
      const sxp = ox + x1 * scale * persp;
      const syp = oy - y2 * scale * persp;
      const size = 1.05 + persp * 0.55 + (b > 0.95 ? 0.35 : 0);
      const c = Math.min(1, b);
      const r = Math.round(118 + 137 * c);
      const gg = Math.round(124 + 128 * c);
      const bl = Math.round(138 + 116 * c);
      ctx.fillStyle = `rgba(${r}, ${gg}, ${bl}, ${Math.min(1, 0.35 + b * 0.65)})`;
      ctx.fillRect(sxp - size / 2, syp - size / 2, size, size);
    }
  };

  const tick = (now) => {
    raf = 0;
    if (!visible || document.hidden) return;
    draw(now);
    if (!reduced.matches) raf = requestAnimationFrame(tick);
  };
  const run = () => {
    if (visible && !document.hidden && !raf) raf = requestAnimationFrame(tick);
  };

  resize();
  new ResizeObserver(() => {
    resize();
    draw(performance.now());
  }).observe(canvas);

  new IntersectionObserver(
    (entries) => {
      visible = entries.some((e) => e.isIntersecting);
      run();
    },
    { threshold: 0.15 }
  ).observe(canvas);
  document.addEventListener("visibilitychange", run);
  reduced.addEventListener?.("change", () => {
    if (reduced.matches) gathered = true;
    run();
  });

  host.addEventListener("pointermove", (e) => {
    const r = host.getBoundingClientRect();
    tx = ((e.clientX - r.left) / r.width - 0.5) * 2;
    ty = ((e.clientY - r.top) / r.height - 0.5) * 2;
  });
  host.addEventListener("pointerleave", () => {
    tx = 0;
    ty = 0;
  });
}

export function initParticleS() {
  if (!("ResizeObserver" in window) || !("IntersectionObserver" in window)) return;
  document.querySelectorAll("canvas[data-particle-s]").forEach(setup);
}
