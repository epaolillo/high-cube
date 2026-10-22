<p align="center">
  <img src="./logo.png" alt="High Cube" width="220" />
</p>

<h1 align="center">High Cube</h1>

<p align="center">
  Deploy from GitHub. Choose how many replicas. High Cube handles the rest.
</p>

---

High Cube is a self-hosted platform for running containerized applications across your own Linux servers. You point it at a GitHub repository with a Dockerfile, give the application a hostname, and set how many replicas you want. High Cube builds the image, spreads the replicas across healthy servers, routes traffic through NGINX with automatic HTTPS, and replaces anything that goes down.

The only scaling decision left to you is horizontal: the replica count.

## What it does

- **Deploys from GitHub.** Each application is a repository, a branch, and a Dockerfile path. Public repositories work with a URL. Private repositories use a per-user GitHub App registered from the panel. A deployment clones, builds, and rolls replicas onto the new image.
- **Admin and User roles.** Admins manage servers, users, and settings. Users manage their own applications and GitHub integrations, and deploy only onto servers the admin assigned to them.
- **Manages several servers.** Two or more Linux hosts form one pool. Replicas are placed automatically on the healthy server with the most free memory.
- **Routes by hostname with HTTPS.** Every application gets its own hostname. High Cube writes the NGINX configuration, obtains a Let's Encrypt certificate, verifies it, and switches the site to HTTPS.
- **Shows what is running.** Live container logs, host and container metrics, and a terminal into any container or host, all from the browser.
- **Recovers on its own.** A container that exits is restarted. A server that stops responding has its replicas moved to the rest of the pool.

## How it works

High Cube is two native C++ programs.

### `hc-admin`

The control plane. It runs once, on the host that receives public traffic.

- Holds the platform state in an on-disk SQLite database.
- Exposes the control API under `/api`.
- Generates the NGINX configuration for the admin panel and for every application hostname.
- Requests and renews TLS certificates through certbot.
- Decides where each replica runs and reacts when a container or a server fails.
- Installs itself as a systemd service the first time it runs, and asks on the terminal for the first administrator account.

### `hc-agent`

The worker. It runs on every server that hosts containers.

- Connects out to `hc-admin` over WebSocket and receives orders encoded with Protobuf.
- Builds, starts, stops, and restarts containers through the local Docker engine.
- Reports container status, container metrics, and host metrics every second.
- Streams logs and relays interactive terminals for containers and for the host.

```
                    Internet
                       │
                ┌──────▼──────┐
                │    NGINX    │  HTTPS, routing by hostname
                └──┬───────┬──┘
          /api     │       │   miapp.example.com
       admin panel │       │   abc.example.com
                ┌──▼───┐   │
                │hc-   │   │
                │admin │   │
                └──┬───┘   │
     WebSocket +   │       │
     Protobuf      │       │
        ┌──────────┼───────┼──────────┐
        │          │       │          │
   ┌────▼─────┐ ┌──▼───────▼┐  ┌──────▼────┐
   │ hc-agent │ │ hc-agent  │  │ hc-agent  │
   │ Docker   │ │ Docker    │  │ Docker    │
   └──────────┘ └───────────┘  └───────────┘
     server 1     server 2       server N
```

## Domains and HTTPS

DNS stays in your hands, and you only configure it once per domain.

1. In **Settings → General**, add a base domain such as `example.com`.
2. In your DNS provider, create two records pointing to the public IP of the server running `hc-admin`:
   - `A  example.com    → <server IP>`
   - `A  *.example.com  → <server IP>`
3. When you create an application, give it a hostname under that domain, for example `miapp.example.com`.

High Cube then publishes the site, requests a certificate for that exact hostname, validates it with Let's Encrypt, and switches NGINX to HTTPS. The certificate status for each application is shown as pending, active, or failed. A failed request can be retried from the application.

## Admin panel

A web dashboard served by the same NGINX that serves your applications.

- **Servers:** connected agents, their health, metrics, and a host terminal.
- **Applications:** repository, hostname, replica count, deployments, replicas, logs, metrics, and container terminals.
- **Users:** Admin or User accounts. Admins assign which servers each User may use.
- **GitHub:** each user registers their own GitHub App for private repositories.
- **Settings:** base domains, plus metrics retention (seven days by default). Admin only.

The dashboard columns load every row and scroll inside their card. Detail lists use server-side pagination, sorting, and filtering, with page sizes of 10, 100, or 1000 rows. The interface is available in English and Spanish.

## Requirements

- Linux with systemd on every host.
- Docker on every server that runs `hc-agent`.
- NGINX and certbot on the host that runs `hc-admin`.
- A domain you control, with the two `A` records described above.

## Installation

`install.sh` builds High Cube on your machine and deploys it to a destination host over SSH. The same script handles both a fresh install and an update: run from a terminal it probes the host and shows a menu with the matching action preselected.

### 1. Prepare the build machine

On the machine where you run `install.sh` (Ubuntu 24.04), install the build toolchain and the tools the script needs (SSH, rsync, Node.js, Python, OpenSSL):

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config git \
  libboost-all-dev protobuf-compiler libprotobuf-dev \
  libsqlite3-dev libssl-dev libspdlog-dev nlohmann-json3-dev libcrypt-dev \
  nodejs npm rsync openssh-client openssl python3
```

SQLiteCpp and jwt-cpp are fetched automatically by CMake.

### 2. Prepare the destination host

The destination needs:

- SSH access for a user you own (key-based, non-interactive).
- `sudo` on that user (to write the systemd units and the NGINX config). If `sudo` needs a password, run `install.sh` from a terminal so it can prompt.
- Docker Engine installed and running (the agent drives containers through it).

NGINX and certbot are installed automatically by the script if they are missing.

### 3. Build and deploy

```bash
# Configure the build directory once.
cmake -S . -B build

# Build everything locally and deploy it to the destination over SSH.
# HC_DEST_USER is the destination SSH user and has no default.
HC_DEST_USER=deploy ./install.sh <destination-ip>
```

`install.sh` compiles `hc-admin` and `hc-agent`, builds the frontend, ships the binaries and their runtime libraries, runs the database migrations, generates the `session_secret` and the `agent_token`, and configures NGINX and the systemd services. Run from a terminal it shows two menus: the action (install or update) and the role to install. On a fresh install it first runs a preflight check over SSH to confirm the host meets the requirements for the chosen role. Useful flags:

- `--role admin|agent|both` — what to install (default `both`). `admin` is the control panel only, `agent` is a worker only, `both` runs both on one host.
- `--mode install|update` — skip the action menu and force it.
- `--db true` — copy the local SQLite database to the destination.

An agent-only install joins an existing control panel, so it also needs the admin URL and the one-time token from the panel:

```bash
HC_DEST_USER=deploy \
HC_ADMIN_URL=ws://<admin-ip>:3000/api/agents/connect \
HC_AGENT_TOKEN=<token-from-panel> \
./install.sh <worker-ip> --role agent
```

### 4. First login

On a fresh install the script creates the first administrator and writes its one-time credentials on the destination:

```bash
cat /var/www/high-cube/.first-admin.txt
```

Open `http://<destination-ip>/`, sign in, and change the password. Then go to **Settings → General** to add your base domain and point the two `A` records (see [Domains and HTTPS](#domains-and-https)).

### 5. Add more servers (agents)

Each extra worker host joins the pool from the panel:

1. In **Servers**, create a server with the worker's IP and copy the one-time agent token.
2. Deploy the worker with the agent role (it builds `hc-agent`, writes `hc-agent.conf`, and installs the service):
   ```bash
   HC_DEST_USER=deploy \
   HC_ADMIN_URL=ws://<admin-ip>:3000/api/agents/connect \
   HC_AGENT_TOKEN=<token-from-panel> \
   ./install.sh <worker-ip> --role agent
   ```

Docker must already be installed on the worker; the preflight check verifies it.

## Scope

The first release covers:

- Deployment from GitHub using a Dockerfile.
- Two or more managed servers.
- Automatic distribution of replicas.
- NGINX routing by hostname with automatic HTTPS.
- Logs, metrics, and terminals for containers and hosts.
- Recovery when a container or an agent goes down.

Managed databases, preview environments, external image registries, and vertical autoscaling are outside this first release.
