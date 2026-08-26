-- Research runs + eval results for the live agent (T8) and the public
-- scoreboard (T9). All *_at columns are Unix epoch MILLISECONDS (INTEGER),
-- never ISO strings.

CREATE TABLE IF NOT EXISTS research_runs (
  id                TEXT PRIMARY KEY,
  device_id         TEXT NOT NULL,
  query             TEXT NOT NULL,
  intent            TEXT,
  layers_json       TEXT,
  citations         INTEGER,
  verified          INTEGER,
  injection_flagged INTEGER NOT NULL DEFAULT 0,
  latency_ms        INTEGER,
  tokens_in         INTEGER,
  tokens_out        INTEGER,
  trace_r2_key      TEXT,
  status            TEXT NOT NULL,          -- running | done | failed | blocked
  created_at        INTEGER NOT NULL,
  finished_at       INTEGER
);
CREATE INDEX IF NOT EXISTS idx_runs_recent ON research_runs(created_at DESC);

CREATE TABLE IF NOT EXISTS eval_results (
  id            TEXT PRIMARY KEY,
  run_at        INTEGER NOT NULL,
  commit_sha    TEXT NOT NULL,
  suite         TEXT NOT NULL,             -- regression | adversarial
  passed        INTEGER NOT NULL,
  total         INTEGER NOT NULL,
  detail_r2_key TEXT
);
CREATE INDEX IF NOT EXISTS idx_evals_recent ON eval_results(run_at DESC);
