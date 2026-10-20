-- Horizontal scale policy fields and rolling-redeploy linkage.

ALTER TABLE applications ADD COLUMN scale_mode TEXT NOT NULL DEFAULT 'manual';
ALTER TABLE applications ADD COLUMN min_replicas INTEGER NOT NULL DEFAULT 1;
ALTER TABLE applications ADD COLUMN max_replicas INTEGER NOT NULL DEFAULT 1;
ALTER TABLE applications ADD COLUMN scale_target_cpu_percent REAL NOT NULL DEFAULT 70;
ALTER TABLE applications ADD COLUMN scale_target_memory_percent REAL NOT NULL DEFAULT 80;
ALTER TABLE applications ADD COLUMN scale_target_load REAL NOT NULL DEFAULT 1.0;
ALTER TABLE applications ADD COLUMN scale_cooldown_seconds INTEGER NOT NULL DEFAULT 120;
ALTER TABLE applications ADD COLUMN scale_window_seconds INTEGER NOT NULL DEFAULT 180;
ALTER TABLE applications ADD COLUMN scale_last_action_at TEXT NOT NULL DEFAULT '';

UPDATE applications
SET min_replicas = desired_replicas,
    max_replicas = desired_replicas
WHERE scale_mode = 'manual';

ALTER TABLE replicas ADD COLUMN replaces_replica_id INTEGER;
