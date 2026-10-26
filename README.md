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

## Timeouts and Cancellation

Each step can set `timeout_ms`, and a workflow can set a top-level `timeout_ms`
for the whole run; steps without their own limit use
`workflow.default_timeout_ms` (0 = no limit):

```json
{
  "timeout_ms": 60000,
  "steps": [
    { "command": "shell_exec", "timeout_ms": 5000, "params": { "cmd": "make test" } },
    { "command": "http_request", "params": { "url": "https://example.com", "timeout_ms": 2000 } }
  ]
}
```

A step that exceeds its limit fails with `Step timed out after N ms` (or
`Workflow timed out after N ms`), and no further steps start. Shutting the
server down (SIGTERM / Ctrl-C) cancels in-flight workflows instead of waiting
for them.

Commands stop cooperatively: `shell_exec` terminates the command's whole
process group, and `http_request` and the downloaders abort the request. (The
`timeout_ms` *param* of `http_request` limits how long any single network
operation may stall.) A command that ignores cancellation still fails as timed
out, but only when it eventually returns.

**Plugin authors:** override `ICommand::execute_with_control(params, control)`
and poll `control.should_stop()` (or use `control.remaining()`) in anything
that blocks. Every plugin must also expand `ORCHA_PLUGIN_ABI_EXPORT()` once;
plugins built against an older `core/ICommand.hpp` are refused at load time
and must be rebuilt.

## Rollback and Circuit Breaking

Both are opt-in per workflow:

```json
{
  "rollback": "reverse",
  "circuit_breaker": true,
  "steps": [
    { "command": "create_pg_db", "params": { "dbname": "app" } },
    { "command": "http_request", "params": { "url": "https://deploy.example.com/hook" } }
  ]
}
```

**`rollback`** (`none` | `reverse` | `best_effort`, default `none`): when the run
fails (a step fails or times out), the steps that completed are rolled back in
reverse completion order. Each command's `rollback(params)` receives the
parameters the step actually ran with (placeholders resolved). `reverse` stops
at the first rollback that fails; `best_effort` continues. Commands whose
metadata does not set `supports_rollback` are reported as `not_supported`. If
the run is *cancelled* (shutdown), rollback is skipped. Each step result in the
response carries the outcome, e.g. `"rollback": {"status": "rolled_back"}`
(`rolled_back`, `failed`, `skipped`, `not_supported`).

**`circuit_breaker`** (`true` | `false`, default `false`): each step consults a
circuit breaker for its command, shared across all runs. After
`workflow.circuit_breaker.failure_threshold` consecutive failures the circuit
opens and steps using that command fail immediately (without running it) until
`reset_timeout_ms` passes; then a single probe is allowed, and
`success_threshold` successful probes close it again. Only command failures and
timeouts count — validation errors and cancellations do not. Inspect or reset
breakers with `GET /api/circuit-breakers` and
`POST /api/circuit-breakers/{command}/reset` (admin auth).

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
