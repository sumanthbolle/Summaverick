CREATE TABLE content (
  id            TEXT PRIMARY KEY,
  kind          TEXT NOT NULL,             -- post | interview | tutorial
  slug          TEXT NOT NULL,
  title         TEXT NOT NULL,
  excerpt       TEXT,
  category      TEXT,
  learning_path TEXT,
  difficulty    TEXT,
  personas_json TEXT,                      -- parsed array, NOT the raw string
  company       TEXT,                      -- interviews only
  read_time     TEXT,
  body_r2_key   TEXT NOT NULL,             -- 'content/post/97.html'
  status        TEXT NOT NULL DEFAULT 'published',
  published_at  INTEGER,
  updated_at    INTEGER NOT NULL
);
CREATE UNIQUE INDEX idx_content_slug ON content(kind, slug);
CREATE INDEX idx_content_feed ON content(kind, status, published_at DESC);

CREATE VIRTUAL TABLE content_fts USING fts5(
  title, excerpt, body,
  content_id UNINDEXED,
  tokenize = 'porter unicode61'
);

CREATE TABLE progress (
  user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  content_id    TEXT NOT NULL REFERENCES content(id) ON DELETE CASCADE,
  state         TEXT NOT NULL,             -- unread | reading | done
  pct           REAL NOT NULL DEFAULT 0,
  updated_at    INTEGER NOT NULL,
  PRIMARY KEY (user_id, content_id)
);
