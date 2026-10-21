-- Audit trail and presence tracking for platform users.
-- Every meaningful action a user performs (creating an application, deploying,
-- managing servers, etc.) is recorded here so administrators can audit it.
CREATE TABLE IF NOT EXISTS user_activity (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  action TEXT NOT NULL,                 -- semantic key, e.g. "application.deploy"
  method TEXT NOT NULL DEFAULT '',      -- HTTP method of the request
  path TEXT NOT NULL DEFAULT '',        -- concrete request path
  target_type TEXT NOT NULL DEFAULT '', -- e.g. "application", "server", "user"
  target_id TEXT NOT NULL DEFAULT '',   -- id of the affected resource when known
  detail TEXT NOT NULL DEFAULT '',      -- optional JSON with extra context
  status INTEGER NOT NULL DEFAULT 0,    -- HTTP status of the response
  ip TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_user_activity_user
  ON user_activity(user_id, created_at);
CREATE INDEX IF NOT EXISTS idx_user_activity_created
  ON user_activity(created_at);

-- Coarse presence heartbeats. While a user keeps the dashboard open, a WebSocket
-- connection is held and a heartbeat row is written periodically. This gives the
-- activity timeline the history it needs to tell whether a user was active.
CREATE TABLE IF NOT EXISTS user_presence (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_user_presence_user
  ON user_presence(user_id, at);
CREATE INDEX IF NOT EXISTS idx_user_presence_at
  ON user_presence(at);
