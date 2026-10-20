CREATE TABLE IF NOT EXISTS schema_migrations (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  filename TEXT NOT NULL UNIQUE,
  applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS roles (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  is_builtin INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS role_permissions (
  role_id INTEGER NOT NULL,
  entity TEXT NOT NULL,
  action TEXT NOT NULL,
  allowed INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (role_id, entity, action),
  FOREIGN KEY (role_id) REFERENCES roles(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  password_hash TEXT NOT NULL,
  name TEXT NOT NULL DEFAULT '',
  locale TEXT NOT NULL DEFAULT 'en',
  avatar_path TEXT NOT NULL DEFAULT '',
  role_id INTEGER NOT NULL,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (role_id) REFERENCES roles(id)
);

CREATE TABLE IF NOT EXISTS servers (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  ip TEXT NOT NULL,
  agent_token_hash TEXT NOT NULL,
  last_seen_at TEXT,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS base_domains (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  hostname TEXT NOT NULL UNIQUE COLLATE NOCASE,
  created_by INTEGER,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (created_by) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS applications (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('web', 'private')),
  repo_url TEXT NOT NULL,
  branch TEXT NOT NULL DEFAULT 'main',
  base_directory TEXT NOT NULL DEFAULT '/',
  dockerfile_path TEXT NOT NULL DEFAULT 'Dockerfile',
  ports_exposes INTEGER NOT NULL,
  desired_replicas INTEGER NOT NULL DEFAULT 1,
  env_json TEXT NOT NULL DEFAULT '{}',
  hostname TEXT UNIQUE COLLATE NOCASE,
  tls_status TEXT NOT NULL DEFAULT 'pending',
  health_check_type TEXT NOT NULL DEFAULT 'none',
  health_check_path TEXT NOT NULL DEFAULT '/',
  health_check_command TEXT NOT NULL DEFAULT '',
  max_restart_count INTEGER NOT NULL DEFAULT 0,
  deploy_key_public TEXT NOT NULL DEFAULT '',
  deploy_key_private TEXT NOT NULL DEFAULT '',
  created_by INTEGER,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (created_by) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS application_dependencies (
  application_id INTEGER NOT NULL,
  dependency_application_id INTEGER NOT NULL,
  PRIMARY KEY (application_id, dependency_application_id),
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (dependency_application_id) REFERENCES applications(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS application_volumes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  application_id INTEGER NOT NULL,
  name TEXT NOT NULL,
  mount_path TEXT NOT NULL,
  UNIQUE (application_id, name),
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS deployments (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  application_id INTEGER NOT NULL,
  git_sha TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'pending',
  log_text TEXT NOT NULL DEFAULT '',
  created_by INTEGER,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (created_by) REFERENCES users(id)
);

CREATE TABLE IF NOT EXISTS replicas (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  deployment_id INTEGER NOT NULL,
  application_id INTEGER NOT NULL,
  server_id INTEGER NOT NULL,
  container_id TEXT NOT NULL DEFAULT '',
  host_port INTEGER,
  status TEXT NOT NULL DEFAULT 'pending',
  desired_state TEXT NOT NULL DEFAULT 'running',
  restart_count INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (deployment_id) REFERENCES deployments(id) ON DELETE CASCADE,
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (server_id) REFERENCES servers(id)
);

CREATE TABLE IF NOT EXISTS metrics (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  server_id INTEGER NOT NULL,
  replica_id INTEGER,
  payload_json TEXT NOT NULL,
  collected_at TEXT NOT NULL,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE,
  FOREIGN KEY (replica_id) REFERENCES replicas(id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_metrics_server_collected ON metrics(server_id, collected_at);
CREATE INDEX IF NOT EXISTS idx_metrics_replica_collected ON metrics(replica_id, collected_at);
CREATE INDEX IF NOT EXISTS idx_metrics_collected ON metrics(collected_at);

CREATE TABLE IF NOT EXISTS settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

INSERT OR IGNORE INTO settings (key, value) VALUES ('metrics_ttl_days', '7');
