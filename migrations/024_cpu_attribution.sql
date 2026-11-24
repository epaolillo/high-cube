-- Per-minute CPU attribution by container, reported by each agent.
-- Kept under the same retention window as metrics (metrics_ttl_days).
CREATE TABLE IF NOT EXISTS cpu_attribution_snapshots (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  server_id INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  collected_at TEXT NOT NULL,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_cpu_attribution_server_collected
  ON cpu_attribution_snapshots(server_id, collected_at);
CREATE INDEX IF NOT EXISTS idx_cpu_attribution_collected
  ON cpu_attribution_snapshots(collected_at);
