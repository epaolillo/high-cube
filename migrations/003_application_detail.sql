-- Application detail: commit message, no-cache deploys, phased logs, replica status timing.
-- SQLite ALTER TABLE ADD COLUMN only allows constant defaults (no strftime()).

ALTER TABLE deployments ADD COLUMN git_message TEXT NOT NULL DEFAULT '';
ALTER TABLE deployments ADD COLUMN no_cache INTEGER NOT NULL DEFAULT 0;
ALTER TABLE deployments ADD COLUMN build_log TEXT NOT NULL DEFAULT '';
ALTER TABLE deployments ADD COLUMN deploy_log TEXT NOT NULL DEFAULT '';

ALTER TABLE replicas ADD COLUMN status_changed_at TEXT NOT NULL DEFAULT '';

UPDATE replicas
SET status_changed_at = COALESCE(
  (SELECT created_at FROM deployments WHERE deployments.id = replicas.deployment_id),
  strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
)
WHERE status_changed_at = '';
