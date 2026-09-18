-- Project enquiries from the homepage form. The form used to confirm "your
-- message is on its way" without sending it anywhere, so the promise that a
-- person reads every message had nothing behind it. All *_at columns are Unix
-- epoch MILLISECONDS (INTEGER), never ISO strings.

CREATE TABLE IF NOT EXISTS leads (
  id            TEXT PRIMARY KEY,
  device_id     TEXT NOT NULL,
  name          TEXT,
  email         TEXT NOT NULL,
  organisation  TEXT,
  intent        TEXT NOT NULL,           -- servicenow_app | integration | ai_tool | unsure
  message       TEXT NOT NULL,
  -- Whether the notification email left the Worker. 0 when no provider or
  -- recipient is configured, in which case the row is the only record.
  notified      INTEGER NOT NULL DEFAULT 0,
  user_agent    TEXT,
  created_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_leads_recent ON leads(created_at DESC);
