-- Members of the single WireGuard hub-and-spoke network named "VPN".
-- The network itself is settings: vpn_cidr (present while the VPN exists), vpn_created_at,
-- and vpn_hub_node_id (the central node every member connects to).
-- Private keys never reach hc-admin: each node generates its own and reports the public key.
CREATE TABLE IF NOT EXISTS vpn_nodes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL,                      -- server | device
  server_id INTEGER UNIQUE,                -- kind = server
  user_id INTEGER,                         -- admin who enrolled the device
  name TEXT NOT NULL,
  address TEXT,                            -- IPv4 inside vpn_cidr; null while a device is enrolling
  public_key TEXT,
  listen_port INTEGER NOT NULL,
  endpoint TEXT NOT NULL DEFAULT '',       -- manual host[:port] override for peers outside the LAN
  lan_ip TEXT NOT NULL DEFAULT '',         -- reported by the node
  observed_ip TEXT NOT NULL DEFAULT '',    -- source address seen by hc-admin
  status TEXT NOT NULL DEFAULT 'pending',  -- enrolling | pending | active
  token_hash TEXT UNIQUE,                  -- enrolling: one-time enroll token. device: sync token
  token_expires_at TEXT,                   -- enrolling only
  last_seen_at TEXT,
  created_at TEXT NOT NULL,
  FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE,
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE SET NULL
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_vpn_nodes_address ON vpn_nodes(address) WHERE address IS NOT NULL;
