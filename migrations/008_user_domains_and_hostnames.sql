-- Per-user base domain assignment and multiple hostnames per application.

CREATE TABLE IF NOT EXISTS user_base_domains (
  user_id INTEGER NOT NULL,
  base_domain_id INTEGER NOT NULL,
  PRIMARY KEY (user_id, base_domain_id),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
  FOREIGN KEY (base_domain_id) REFERENCES base_domains(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS application_hostnames (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  application_id INTEGER NOT NULL,
  hostname TEXT NOT NULL UNIQUE COLLATE NOCASE,
  base_domain_id INTEGER NOT NULL,
  tls_status TEXT NOT NULL DEFAULT 'pending',
  position INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (application_id) REFERENCES applications(id) ON DELETE CASCADE,
  FOREIGN KEY (base_domain_id) REFERENCES base_domains(id)
);

CREATE INDEX IF NOT EXISTS idx_application_hostnames_app
  ON application_hostnames(application_id, position);

-- Copy existing application hostnames into the new table (longest matching base domain).
INSERT INTO application_hostnames (application_id, hostname, base_domain_id, tls_status, position)
SELECT
  a.id,
  lower(a.hostname),
  (
    SELECT bd.id
    FROM base_domains bd
    WHERE lower(a.hostname) LIKE '%.' || lower(bd.hostname)
    ORDER BY length(bd.hostname) DESC
    LIMIT 1
  ),
  a.tls_status,
  0
FROM applications a
WHERE a.hostname IS NOT NULL
  AND trim(a.hostname) != ''
  AND (
    SELECT bd.id
    FROM base_domains bd
    WHERE lower(a.hostname) LIKE '%.' || lower(bd.hostname)
    ORDER BY length(bd.hostname) DESC
    LIMIT 1
  ) IS NOT NULL;

-- Assign those base domains to the application owners so edits keep working.
INSERT OR IGNORE INTO user_base_domains (user_id, base_domain_id)
SELECT DISTINCT a.created_by, ah.base_domain_id
FROM applications a
INNER JOIN application_hostnames ah ON ah.application_id = a.id
WHERE a.created_by IS NOT NULL AND a.created_by > 0;
