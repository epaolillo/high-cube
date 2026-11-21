-- Top processes by CPU and memory reported by each agent every 60 seconds.
-- Kept for 48 hours, independent of the metrics TTL.
-- Quick Commands use them to find the culprit of a past CPU or RAM spike.
CREATE TABLE IF NOT EXISTS process_snapshots (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  server_id INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  collected_at TEXT NOT NULL,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_process_snapshots_server_collected
  ON process_snapshots(server_id, collected_at);
CREATE INDEX IF NOT EXISTS idx_process_snapshots_collected
  ON process_snapshots(collected_at);
