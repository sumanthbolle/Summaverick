-- Persisted daily metals series (T9). Lets /api/metals serve real charts from
-- D1 instead of refetching Frankfurter on every request. One row per
-- (day, base, quote); backfilled + upserted by the metals route/cron.
CREATE TABLE metals_daily (
  day         TEXT NOT NULL,               -- YYYY-MM-DD (UTC)
  base        TEXT NOT NULL,               -- XAU | XAG | ...
  quote       TEXT NOT NULL,               -- USD | INR | ...
  rate        REAL NOT NULL,
  fetched_at  INTEGER NOT NULL,
  PRIMARY KEY (day, base, quote)
);
CREATE INDEX idx_metals_daily_series ON metals_daily(base, quote, day);
