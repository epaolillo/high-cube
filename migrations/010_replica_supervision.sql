-- Container supervision and crash traceability.
-- The orchestrator records when a replica was last reported alive so it can
-- detect silent falls and relaunch the container, keeping it alive.
ALTER TABLE replicas ADD COLUMN last_seen_at TEXT NOT NULL DEFAULT '';

-- Historical log of container falls and recoveries, so operators can audit
-- how often an application crashed and what the orchestrator did about it.
CREATE TABLE IF NOT EXISTS replica_incidents (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  application_id INTEGER NOT NULL,
  replica_id INTEGER,
  server_id INTEGER,
  kind TEXT NOT NULL,                 -- restart | unhealthy | failed | down | healed
  detail TEXT NOT NULL DEFAULT '',
  restart_count INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (replica_id) REFERENCES replicas(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_replica_incidents_app
  ON replica_incidents(application_id, created_at);
CREATE INDEX IF NOT EXISTS idx_replica_incidents_created
  ON replica_incidents(created_at);
