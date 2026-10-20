-- Multiple host:container port mappings and a pending-redeploy flag.

ALTER TABLE applications ADD COLUMN port_mappings_json TEXT NOT NULL DEFAULT '[]';
ALTER TABLE applications ADD COLUMN pending_redeploy INTEGER NOT NULL DEFAULT 0;

-- Preserve the previous single listen port as one mapping with auto host port (0).
UPDATE applications
SET port_mappings_json = '[{"containerPort":' || ports_exposes || ',"hostPort":0}]'
WHERE port_mappings_json = '[]' AND ports_exposes > 0;
