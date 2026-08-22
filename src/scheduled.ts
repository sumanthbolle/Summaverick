/**
 * Cron dispatch. Wrangler triggers (see wrangler.jsonc) fire these by cron
 * string. Each branch is implemented in its owning task:
 *   "0 17 * * *" — nightly D1 export -> R2 backups/ (T4)
 *   "0 18 * * *" — UPSC retention sweep: archive >180d notes to R2, delete (T10)
 */
import type { Env } from "./types";

export async function runScheduled(cron: string, env: Env): Promise<void> {
  switch (cron) {
    case "0 17 * * *":
      await import("./lib/backup").then((m) => m.runBackup(env));
      break;
    case "0 18 * * *":
      await import("./lib/retention").then((m) => m.runUpscRetention(env));
      break;
    default:
      console.warn("no scheduled handler for cron", cron);
  }
}
