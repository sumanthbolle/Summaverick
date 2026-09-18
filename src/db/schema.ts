/**
 * TypeScript row types mirroring migrations/*.sql exactly.
 *
 * Convention: every `*_at` / `*At` column is Unix epoch MILLISECONDS as INTEGER.
 * Booleans are stored as INTEGER 0/1 (suffixed `_int` in the raw row where it
 * helps readability). JSON columns end in `_json` and hold serialised strings.
 */

export interface UserRow {
  id: string;
  email: string | null;
  display_name: string | null;
  created_at: number;
  last_seen_at: number | null;
}

export interface CredentialRow {
  id: string; // WebAuthn credential id, base64url
  user_id: string;
  public_key: ArrayBuffer; // BLOB
  sign_count: number;
  transports: string | null; // JSON array string
  created_at: number;
}

export interface SessionRow {
  id: string;
  user_id: string | null;
  device_id: string;
  created_at: number;
  expires_at: number;
  ua_hash: string | null;
}

export interface MagicLinkRow {
  token_hash: string;
  email: string;
  expires_at: number;
  consumed_at: number | null;
}

export interface QuizCategoryRow {
  id: string;
  name: string;
  icon: string | null;
  description: string | null;
  featured: number;
  color: string | null;
  modes_json: string;
  sort_order: number;
}

export interface QuestionRow {
  id: string;
  category_id: string;
  kind: string;
  difficulty: string | null;
  is_multi: number;
  topic: string | null;
  stem: string;
  options_json: string; // JSON array of strings
  correct_json: string; // JSON array of 0-based indices
  explanation: string | null;
  source: string | null;
  source_set: string | null;
  retired_at: number | null;
}

export interface AttemptRow {
  id: string;
  user_id: string | null;
  device_id: string;
  category_id: string;
  mode: string;
  question_ids: string; // JSON: [{ id, order: number[] }] — fixes set + option perm
  started_at: number;
  finished_at: number | null;
  score: number | null;
}

export interface ResponseRow {
  id: string;
  attempt_id: string;
  question_id: string;
  chosen_json: string; // JSON array of chosen ORIGINAL indices
  correct: number; // 0/1
  ms_taken: number | null;
  answered_at: number;
}

export interface EventRow {
  id: string;
  user_id: string | null;
  device_id: string | null;
  name: string;
  props_json: string | null;
  ts: number;
}

export interface ContentRow {
  id: string;
  kind: string; // post | interview | tutorial
  slug: string;
  title: string;
  excerpt: string | null;
  category: string | null;
  learning_path: string | null;
  difficulty: string | null;
  personas_json: string | null; // parsed JSON array, never the python-literal string
  company: string | null;
  read_time: string | null;
  body_r2_key: string;
  status: string;
  published_at: number | null;
  updated_at: number;
}

export interface ProgressRow {
  user_id: string;
  content_id: string;
  state: string; // unread | reading | done
  pct: number;
  updated_at: number;
}

export interface UpscSourceRow {
  id: string;
  url: string;
  host: string;
  title: string | null;
  content_hash: string;
  official_summary: string | null;
  source_verified: number;
  fetched_at: number;
  health: string | null;
}

export interface UpscNoteRow {
  id: string;
  source_id: string;
  content_hash: string;
  status: string; // draft | published | needs-review
  paper: string | null;
  anchor: string | null;
  syllabus_json: string | null;
  score: number | null;
  band: string | null; // core | strong | thin | log
  payload_json: string;
  published_at: number | null;
  created_at: number;
}

export interface UpscReviewRow {
  id: string;
  note_id: string;
  reason: string;
  flagged_json: string | null;
  created_at: number;
  resolved_at: number | null;
  resolution: string | null;
}

export interface PublishRunRow {
  id: string;
  kind: string; // daily | weekly | prelims | revision
  started_at: number;
  finished_at: number | null;
  ok: number | null;
  stats_json: string | null;
}

export interface MetalsDailyRow {
  day: string; // YYYY-MM-DD (UTC)
  base: string; // e.g. XAU
  quote: string; // e.g. USD
  rate: number;
  fetched_at: number;
}

export interface LeadRow {
  id: string;
  device_id: string;
  name: string | null;
  email: string;
  organisation: string | null;
  intent: string; // servicenow_app | integration | ai_tool | unsure
  message: string;
  /** 1 once the notification email left the Worker. */
  notified: number;
  user_agent: string | null;
  created_at: number;
}
