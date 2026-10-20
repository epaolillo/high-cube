# Build

## System packages (Ubuntu 24.04)

C++ build toolchain:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config git \
  libboost-all-dev protobuf-compiler libprotobuf-dev \
  libsqlite3-dev libssl-dev libspdlog-dev nlohmann-json3-dev libcrypt-dev
```

Admin host runtime (control plane):

```bash
sudo apt-get install -y --no-install-recommends nginx certbot python3-certbot-nginx
```

Agent host runtime: install Docker Engine and the CLI from Docker's official
repository (`docker-ce`, `docker-ce-cli`, `containerd.io`, `docker-buildx-plugin`,
`docker-compose-plugin`), then enable the `docker` service.

SQLiteCpp and jwt-cpp are fetched by CMake via FetchContent.

## Binaries

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

Outputs:

- `build/hc-admin`
- `build/hc-agent`

## Frontend

```bash
cd src/admin/frontend
npm install
npm run build
```

NGINX serves `src/admin/frontend/dist` for the admin hostname. During development, `npm run dev` proxies `/api` to `http://127.0.0.1:3000`.

## First run

```bash
cp hc-admin.conf.example hc-admin.conf
sudo ./build/hc-admin --config ./hc-admin.conf
```

Then start agents with `hc-agent.conf` on each worker host.
