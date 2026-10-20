-- Servers used to be soft-deleted (status = 'removed') and stayed in the list.
-- Drop those rows. replicas.server_id has no ON DELETE action, so delete children first.
-- metrics, user_servers and application_target_servers cascade from servers.

DELETE FROM replicas
WHERE server_id IN (SELECT id FROM servers WHERE status = 'removed');

DELETE FROM servers WHERE status = 'removed';
