# Orcha

Orcha is a fast, extensible, and modern command orchestration engine written in C++20.  
It dynamically loads command plugins, runs ordered or parallel workflows defined in YAML, supports PowerShell automation, and exposes both CLI and HTTP API.

---

## Features

- **Plugin-based architecture:** Add or update commands without recompiling the core agent
- **YAML workflow runner:** Define multi-step workflows with parameters, piping, and parallelism
- **Dynamic command loading:** Hot-plug commands from the `commands/` directory
- **Fully asynchronous, multi-threaded**
- **Cross-platform:** Linux, Windows, macOS
- **Embedded PowerShell support:** Secure, portable scripting even without host PowerShell
- **REST API:** POST workflows or commands to `/workflow`
- **Modern C++20, clean architecture**
- **Built with CMake & vcpkg:** Easy dependency management

---

## Quick Start

### 1. Clone and Build

```bash
git clone https://github.com/sbrosgol/orcha-pub.git
cd orcha
# Install vcpkg dependencies
vcpkg install boost-thread boost-system cpprestsdk yaml-cpp
mkdir build && cd build
cmake ..
make
```

### 2. Run with Docker (recommended)

A multi-stage `Dockerfile` and `docker-compose.yml` are provided at the repo root.
The compose stack runs only Orcha — the `create_pg_db` plugin is designed to
connect to an *existing* Postgres instance (host, network-reachable, or a
container in another compose stack). Pass the host/credentials in the workflow
payload at runtime. From inside the container, the host machine is reachable
at `host.docker.internal`.

```bash
# from the repo root
docker compose build
docker compose up -d

# tail logs
docker compose logs -f orcha
```

The agent is now reachable on `http://localhost:8070/`. To override admin
credentials or the exposed port, set them in the environment or in a
`.env` file beside `docker-compose.yml`:

```bash
ORCHA_ADMIN_PASSWORD=super-secret
ORCHA_SERVER_API_TOKEN=$(openssl rand -hex 32)
ORCHA_PORT=8080
```

`POST /workflow` executes commands (including `shell_exec`), so it is
fail-closed: it requires `Authorization: Bearer $ORCHA_SERVER_API_TOKEN`
(admin Basic credentials are also accepted). Without a token it is only served
when the server is bound to a loopback address (the non-Docker default is
`server.host: 127.0.0.1`); otherwise it answers `503`.

```bash
curl -H "Authorization: Bearer $ORCHA_SERVER_API_TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' \
     http://localhost:8070/workflow
```

To use a custom `orcha.yaml`, copy the example next to the compose file and
uncomment the bind mount in `docker-compose.yml`:

```bash
cp src/orcha/orcha.yaml.example orcha.yaml
```

---

## API Documentation (Swagger / OpenAPI)

Once Orcha is running, the agent serves an interactive API explorer and a
machine-readable spec:

| URL | What it serves |
|---|---|
| `GET /swagger`        | Swagger UI (interactive) |
| `GET /swagger.json`   | OpenAPI 3.0.3 specification |
| `GET /sample`         | A minimal `POST /workflow` payload |
| `GET /commands`       | Loaded command plugins and their parameters |

The spec covers every public route (`/`, `/workflow`, `/commands`, `/sample`)
plus the admin surface under `/api/*` (jobs, run history, plugin lifecycle).
Admin endpoints are gated by HTTP Basic auth — credentials come from
`admin.username` / `admin.password` in `orcha.yaml` (or
`ORCHA_ADMIN_USERNAME` / `ORCHA_ADMIN_PASSWORD`).
