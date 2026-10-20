-- Fixed Admin/User roles, per-user server assignment, and GitHub App integrations.

ALTER TABLE roles ADD COLUMN code TEXT;

-- Seed / normalize the two builtin roles.
INSERT OR IGNORE INTO roles (name, is_builtin, code) VALUES ('Administrator', 1, 'admin');
INSERT OR IGNORE INTO roles (name, is_builtin, code) VALUES ('User', 1, 'user');

UPDATE roles SET code = 'admin', is_builtin = 1, name = 'Administrator'
WHERE name = 'Administrator' OR lower(name) = 'admin' OR code = 'admin';
UPDATE roles SET code = 'user', is_builtin = 1, name = 'User'
WHERE name = 'User' OR lower(name) = 'user' OR code = 'user';

-- Deduplicate: keep the lowest id per code, reassign users, delete extras without code.
UPDATE users
SET role_id = (SELECT MIN(id) FROM roles WHERE code = 'admin')
WHERE role_id IN (SELECT id FROM roles WHERE code = 'admin')
  AND role_id != (SELECT MIN(id) FROM roles WHERE code = 'admin');

UPDATE users
SET role_id = (SELECT MIN(id) FROM roles WHERE code = 'user')
WHERE role_id IN (SELECT id FROM roles WHERE code = 'user')
  AND role_id != (SELECT MIN(id) FROM roles WHERE code = 'user');

UPDATE users
SET role_id = (SELECT MIN(id) FROM roles WHERE code = 'user')
WHERE role_id NOT IN (SELECT id FROM roles WHERE code IN ('admin', 'user'));

DELETE FROM roles
WHERE id NOT IN (
  SELECT MIN(id) FROM roles WHERE code = 'admin'
  UNION
  SELECT MIN(id) FROM roles WHERE code = 'user'
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_roles_code ON roles(code);

DROP TABLE IF EXISTS role_permissions;

CREATE TABLE IF NOT EXISTS user_servers (
  user_id INTEGER NOT NULL,
  server_id INTEGER NOT NULL,
  PRIMARY KEY (user_id, server_id),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS github_apps (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL,
  name TEXT NOT NULL,
  app_id INTEGER,
  slug TEXT NOT NULL DEFAULT '',
  client_id TEXT NOT NULL DEFAULT '',
  client_secret TEXT NOT NULL DEFAULT '',
  webhook_secret TEXT NOT NULL DEFAULT '',
  private_key_pem TEXT NOT NULL DEFAULT '',
  html_url TEXT NOT NULL DEFAULT '',
  owner_login TEXT NOT NULL DEFAULT '',
  installation_id INTEGER,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_github_apps_user ON github_apps(user_id);

ALTER TABLE applications ADD COLUMN github_app_id INTEGER REFERENCES github_apps(id);
ALTER TABLE applications ADD COLUMN repo_full_name TEXT NOT NULL DEFAULT '';
