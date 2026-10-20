-- Servers an application may run on, in priority order (0 = first choice).

CREATE TABLE IF NOT EXISTS application_target_servers (
  application_id INTEGER NOT NULL,
  server_id INTEGER NOT NULL,
  priority INTEGER NOT NULL,
  PRIMARY KEY (application_id, server_id),
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_application_target_servers_priority
  ON application_target_servers(application_id, priority);
