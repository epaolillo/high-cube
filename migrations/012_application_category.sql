-- The "type" column now classifies an application by category, but the original
-- schema pinned it with CHECK (type IN ('web', 'private')). SQLite cannot drop a
-- CHECK with ALTER TABLE, and rebuilding the table here is unsafe because these
-- migrations run inside a transaction (where PRAGMA foreign_keys is a no-op, so a
-- DROP would cascade-delete child rows). Instead, edit the stored schema in place
-- to remove the constraint, reload it, and normalize legacy values to 'other'.
PRAGMA writable_schema = ON;

UPDATE sqlite_master
SET sql = replace(sql, ' CHECK (type IN (''web'', ''private''))', '')
WHERE type = 'table' AND name = 'applications';

-- RESET turns writable_schema off and forces the in-memory schema to reload so
-- the statements below compile against the constraint-free definition.
PRAGMA writable_schema = RESET;

UPDATE applications
SET type = 'other'
WHERE type NOT IN ('other', 'db', 'storage', 'ai_ml', 'networking', 'monitoring', 'security');
