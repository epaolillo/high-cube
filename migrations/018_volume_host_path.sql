-- Host directory of each volume, relative to /var/lib/data on its server.
-- Existing rows stay empty: their data remains in the Docker volume they already use.
ALTER TABLE application_volumes ADD COLUMN host_path TEXT NOT NULL DEFAULT '';
-- Overlapping (nested) directories are refused by the API; this guards exact duplicates.
CREATE UNIQUE INDEX IF NOT EXISTS idx_application_volumes_host_path
  ON application_volumes (host_path) WHERE host_path <> '';
