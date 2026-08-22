/**
 * Admin routes (T10). The UPSC needs-review queue as a real authenticated UI.
 *
 * These live under /admin and /api/admin and are additionally meant to sit
 * behind Cloudflare Access in production (configure an Access application for
 * /admin* and /api/admin*). As defence in depth the Worker also requires the
 * Access-injected identity header; in local dev (ENVIRONMENT=development) the
 * check is bypassed so the queue is usable without Access.
 */
import type { Ctx, Env, RouteDef, RouteMaker } from "../types";
import {
  listOpenReviews,
  resolveReview,
} from "../db/queries";
import { forbidden, json, nowMs, ok, readJson } from "../lib/json";

function assertAdmin(req: Request, env: Env): Response | null {
  if (env.ENVIRONMENT === "development") return null;
  // Cloudflare Access injects the authenticated user's email on every request
  // it lets through. Absence means the request did not pass Access.
  const email = req.headers.get("cf-access-authenticated-user-email");
  if (!email) return forbidden("admin access required (Cloudflare Access)");
  return null;
}

export function adminRoutes(route: RouteMaker): RouteDef[] {
  return [
    // JSON: open review queue
    route("GET", "/api/admin/reviews", async (req, ctx: Ctx) => {
      const blocked = assertAdmin(req, ctx.env);
      if (blocked) return blocked;
      const reviews = await listOpenReviews(ctx.env.DB, 200);
      return json({
        ok: true,
        count: reviews.length,
        reviews: reviews.map((r) => ({
          id: r.id,
          noteId: r.note_id,
          reason: r.reason,
          flagged: safeParse(r.flagged_json),
          createdAt: r.created_at,
        })),
      });
    }),

    // Resolve one review
    route("POST", "/api/admin/reviews/:id/resolve", async (req, ctx: Ctx) => {
      const blocked = assertAdmin(req, ctx.env);
      if (blocked) return blocked;
      const body = await readJson<{ resolution?: string }>(req);
      const done = await resolveReview(
        ctx.env.DB,
        ctx.params.id!,
        nowMs(),
        body?.resolution ?? "resolved"
      );
      if (!done) return json({ ok: false, error: "not_found_or_resolved" }, { status: 404 });
      return ok({ resolved: ctx.params.id });
    }),

    // The review UI (server-rendered, gated). Fetches the JSON above.
    route("GET", "/admin/review", async (req, ctx: Ctx) => {
      const blocked = assertAdmin(req, ctx.env);
      if (blocked) return blocked;
      return new Response(REVIEW_HTML, {
        headers: { "content-type": "text/html; charset=utf-8" },
      });
    }),
  ];
}

function safeParse(s: string | null): unknown {
  if (!s) return null;
  try {
    return JSON.parse(s);
  } catch {
    return null;
  }
}

const REVIEW_HTML = `<!doctype html>
<html lang="en"><head><meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>UPSC Review Queue — summaverick admin</title>
<link rel="stylesheet" href="/assets/app.css" />
<style>
  .rev{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px;margin:12px 0}
  .rev h3{margin:0 0 6px;font-size:1rem}
  .reason{color:var(--bad);font-weight:600}
  .flag{color:var(--muted);font-size:.85rem;white-space:pre-wrap}
  button{background:var(--accent);color:#04121c;border:0;padding:8px 16px;border-radius:8px;font-weight:700;cursor:pointer;margin-top:10px}
  .empty{color:var(--muted)}
</style></head>
<body><main class="wrap">
  <h1>UPSC Review Queue</h1>
  <p class="tag">Notes flagged by the evidence gate (unsupported facts or content-hash mismatch).</p>
  <div id="list"><p class="empty">Loading…</p></div>
</main>
<script type="module">
const list = document.getElementById('list');
async function load(){
  const r = await fetch('/api/admin/reviews', {credentials:'same-origin'});
  const d = await r.json();
  if(!d.reviews || !d.reviews.length){ list.innerHTML = '<p class="empty">Queue is empty.</p>'; return; }
  list.innerHTML = '';
  for(const rev of d.reviews){
    const el = document.createElement('div'); el.className='rev';
    el.innerHTML = '<h3>'+rev.noteId+'</h3>'+
      '<div class="reason">'+rev.reason+'</div>'+
      '<div class="flag">'+JSON.stringify(rev.flagged,null,2)+'</div>'+
      '<button>Resolve</button>';
    el.querySelector('button').onclick = async ()=>{
      await fetch('/api/admin/reviews/'+rev.id+'/resolve',{method:'POST',headers:{'content-type':'application/json'},credentials:'same-origin',body:JSON.stringify({resolution:'resolved-by-admin'})});
      load();
    };
    list.appendChild(el);
  }
}
load();
</script>
</body></html>`;
