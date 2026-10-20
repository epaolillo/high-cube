# Integration checklist

End-to-end validation for the High Cube MVP on two Linux servers.

## Host A (admin)

1. Install the build toolchain plus NGINX, certbot, and Docker (see [Build](BUILD.md) for the package lists):
   ```bash
   sudo apt-get update && sudo apt-get install -y --no-install-recommends \
     build-essential cmake pkg-config git libboost-all-dev protobuf-compiler \
     libprotobuf-dev libsqlite3-dev libssl-dev libspdlog-dev nlohmann-json3-dev \
     libcrypt-dev nginx certbot python3-certbot-nginx
   ```
   Then install Docker Engine from Docker's official repository.
2. Copy `hc-admin.conf.example` to `hc-admin.conf` and set `public_url`, `certbot_email`, `session_secret`.
3. Build frontend: `cd src/admin/frontend && npm install && npm run build`
4. Build binaries: `cmake -S . -B build && cmake --build build -j`
5. `sudo ./build/hc-admin --config ./hc-admin.conf` and create the first admin user.
6. DNS: `A example.com` and `A *.example.com` to host A public IP.
7. In Settings → General, add base domain `example.com`.

## Host B (agent)

1. Install Docker Engine from Docker's official repository and enable the `docker` service.
2. In the admin UI, create a server with host B IP. Copy the one-time `agentToken`.
3. Write `hc-agent.conf` with `admin_url`, `agent_token`, `server_name`, `server_ip`.
4. Copy `hc-agent` binary to host B and run `sudo ./hc-agent --config ./hc-agent.conf`.

## Deploy a web app

1. Create a Web application with a public GitHub repo, Dockerfile, port, and hostname `app.example.com`.
2. Click Deploy. Confirm replica becomes `running`/`healthy`.
3. Confirm `https://app.example.com` serves traffic after TLS becomes `active`.
4. Scale to 2, stop one replica, restart it, open logs/metrics from the detail page.

## Private database

1. Create a Private application from a Postgres Dockerfile with a volume mount and `desiredReplicas=1`.
2. Deploy it. Note internal endpoint `hc-<id>:5432`.
3. Create a Web application that depends on that private app and sets `DATABASE_URL` to the internal endpoint.
4. Confirm both land on the same server and the web app can connect.

## Failure drills

1. `docker stop` a web replica: agent/admin should mark status and recover according to restart policy.
2. Stop `hc-agent` for 180s: server goes offline; non-volume replicas can be placed elsewhere; volume-backed services stay unavailable on the original server.
