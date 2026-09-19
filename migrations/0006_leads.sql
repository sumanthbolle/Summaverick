-- Contact messages from the homepage form. The form used to confirm success in
-- the browser without sending anything anywhere; these rows are what the
-- confirmation now refers to.
CREATE TABLE IF NOT EXISTS leads (
  id               TEXT PRIMARY KEY,
  -- Client-generated per submission, so a retry after an uncertain response
  -- lands on the same row instead of creating a duplicate.
  idempotency_key  TEXT UNIQUE,
  name             TEXT,
  email            TEXT NOT NULL,
  organisation     TEXT,
  intent           TEXT,
  message          TEXT,
  -- 1 once an email notification has been accepted by the mail provider.
  notified         INTEGER NOT NULL DEFAULT 0,
  created_at       INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_leads_recent ON leads(created_at DESC);
