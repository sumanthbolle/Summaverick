-- Identity ------------------------------------------------------------------
CREATE TABLE users (
  id            TEXT PRIMARY KEY,
  email         TEXT UNIQUE,
  display_name  TEXT,
  created_at    INTEGER NOT NULL,
  last_seen_at  INTEGER
);

CREATE TABLE credentials (
  id            TEXT PRIMARY KEY,          -- WebAuthn credential ID (base64url)
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  public_key    BLOB NOT NULL,
  sign_count    INTEGER NOT NULL DEFAULT 0,
  transports    TEXT,
  created_at    INTEGER NOT NULL
);
CREATE INDEX idx_credentials_user ON credentials(user_id);

CREATE TABLE sessions (
  id            TEXT PRIMARY KEY,
  user_id       TEXT REFERENCES users(id) ON DELETE CASCADE,
  device_id     TEXT NOT NULL,             -- set for anonymous sessions too
  created_at    INTEGER NOT NULL,
  expires_at    INTEGER NOT NULL,
  ua_hash       TEXT
);
CREATE INDEX idx_sessions_user ON sessions(user_id);
CREATE INDEX idx_sessions_expiry ON sessions(expires_at);

CREATE TABLE magic_links (
  token_hash    TEXT PRIMARY KEY,
  email         TEXT NOT NULL,
  expires_at    INTEGER NOT NULL,
  consumed_at   INTEGER
);

-- Quiz ----------------------------------------------------------------------
CREATE TABLE quiz_categories (
  id            TEXT PRIMARY KEY,          -- 'csa', 'scripting', ...
  name          TEXT NOT NULL,
  icon          TEXT,
  description   TEXT,
  featured      INTEGER NOT NULL DEFAULT 0,
  color         TEXT,
  modes_json    TEXT NOT NULL,             -- verbatim from quiz-categories.json
  sort_order    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE questions (
  id            TEXT PRIMARY KEY,          -- 'arch-e1'
  category_id   TEXT NOT NULL REFERENCES quiz_categories(id),
  kind          TEXT NOT NULL DEFAULT 'mcq',
  difficulty    TEXT,                      -- easy | medium | hard
  is_multi      INTEGER NOT NULL DEFAULT 0,
  topic         TEXT,
  stem          TEXT NOT NULL,
  options_json  TEXT NOT NULL,             -- JSON array of strings
  correct_json  TEXT NOT NULL,             -- JSON array of 0-based indices
  explanation   TEXT,
  source        TEXT,
  source_set    TEXT,
  retired_at    INTEGER
);
CREATE INDEX idx_questions_cat ON questions(category_id, difficulty)
  WHERE retired_at IS NULL;

CREATE TABLE attempts (
  id            TEXT PRIMARY KEY,
  user_id       TEXT REFERENCES users(id) ON DELETE CASCADE,
  device_id     TEXT NOT NULL,
  category_id   TEXT NOT NULL REFERENCES quiz_categories(id),
  mode          TEXT NOT NULL,
  question_ids  TEXT NOT NULL,             -- JSON array, fixes the set at start
  started_at    INTEGER NOT NULL,
  finished_at   INTEGER,
  score         REAL
);
CREATE INDEX idx_attempts_user ON attempts(user_id, started_at DESC);
CREATE INDEX idx_attempts_device ON attempts(device_id, started_at DESC);

CREATE TABLE responses (
  id            TEXT PRIMARY KEY,
  attempt_id    TEXT NOT NULL REFERENCES attempts(id) ON DELETE CASCADE,
  question_id   TEXT NOT NULL REFERENCES questions(id),
  chosen_json   TEXT NOT NULL,
  correct       INTEGER NOT NULL,
  ms_taken      INTEGER,
  answered_at   INTEGER NOT NULL
);
CREATE UNIQUE INDEX idx_responses_unique ON responses(attempt_id, question_id);

-- Telemetry -----------------------------------------------------------------
CREATE TABLE events (
  id            TEXT PRIMARY KEY,
  user_id       TEXT,
  device_id     TEXT,
  name          TEXT NOT NULL,
  props_json    TEXT,
  ts            INTEGER NOT NULL
);
CREATE INDEX idx_events_ts ON events(ts);
