-- Which server receives public traffic for a base domain.
-- Nullable: domains created before this column have no access point yet.
ALTER TABLE base_domains ADD COLUMN access_server_id INTEGER REFERENCES servers(id) ON DELETE SET NULL;
