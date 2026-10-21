-- Which managed database this application is. Empty means the Management tab stays hidden.
-- Allowed values are enforced by the API (mysql-8, postgres-16, pgvector-17, ...).
ALTER TABLE applications ADD COLUMN database_flavor TEXT NOT NULL DEFAULT '';
