CREATE TABLE upsc_sources (
  id               TEXT PRIMARY KEY,       -- 'src_...'
  url              TEXT NOT NULL,
  host             TEXT NOT NULL,
  title            TEXT,
  content_hash     TEXT NOT NULL,
  official_summary TEXT,
  source_verified  INTEGER NOT NULL DEFAULT 0,
  fetched_at       INTEGER NOT NULL,
  health           TEXT
);
CREATE INDEX idx_upsc_sources_fetched ON upsc_sources(fetched_at DESC);

CREATE TABLE upsc_notes (
  id            TEXT PRIMARY KEY,
  source_id     TEXT NOT NULL REFERENCES upsc_sources(id) ON DELETE CASCADE,
  content_hash  TEXT NOT NULL,             -- invalidates note when source changes
  status        TEXT NOT NULL,             -- draft | published | needs-review
  paper         TEXT,
  anchor        TEXT,
  syllabus_json TEXT,
  score         REAL,
  band          TEXT,                      -- core | strong | thin | log
  payload_json  TEXT NOT NULL,
  published_at  INTEGER,
  created_at    INTEGER NOT NULL
);
CREATE INDEX idx_upsc_notes_pub ON upsc_notes(status, published_at DESC);
CREATE INDEX idx_upsc_notes_anchor ON upsc_notes(anchor);

CREATE TABLE upsc_review (
  id            TEXT PRIMARY KEY,
  note_id       TEXT NOT NULL REFERENCES upsc_notes(id) ON DELETE CASCADE,
  reason        TEXT NOT NULL,
  flagged_json  TEXT,
  created_at    INTEGER NOT NULL,
  resolved_at   INTEGER,
  resolution    TEXT
);

CREATE TABLE publish_runs (
  id            TEXT PRIMARY KEY,
  kind          TEXT NOT NULL,             -- daily | weekly | prelims | revision
  started_at    INTEGER NOT NULL,
  finished_at   INTEGER,
  ok            INTEGER,
  stats_json    TEXT
);
