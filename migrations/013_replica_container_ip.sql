-- Store the container IP on the highcube Docker network so nginx can proxy
-- public hostnames to it without publishing any host port.
ALTER TABLE replicas ADD COLUMN container_ip TEXT NOT NULL DEFAULT '';
