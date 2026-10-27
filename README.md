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
curl -i -H "Authorization: Bearer $ORCHA_SERVER_API_TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' \
     http://localhost:8070/workflow
# HTTP/1.1 202 Accepted
# Location: /workflow/runs/3f2a...
```

Runs are asynchronous — see [Workflow runs](#workflow-runs).

To use a custom `orcha.yaml`, copy the example next to the compose file and
uncomment the bind mount in `docker-compose.yml`:

```bash
cp src/orcha/orcha.yaml.example orcha.yaml
```

---

## Workflow runs

`POST /workflow` validates the workflow, queues a run and answers
**`202 Accepted`** with the run and a `Location` header. Poll the run until its
`status` is final:

| Endpoint | Purpose |
|---|---|
| `POST /workflow` | Submit a run → `202` (`200` with `Prefer: wait`) |
| `GET /workflow/runs/{id}` | Status, timestamps and, once finished, per-step `result` |
| `POST /workflow/runs/{id}/cancel` | Cancel: a queued run never starts (`200`); a running one stops cooperatively (`202`) |

A run moves through `queued → running → success | failed | cancelled`; a final
status never changes. Runs a previous server left queued or running are marked
`interrupted` when the next one starts. To wait for the result in one call,
send `Prefer: wait=<seconds>`: the server answers `200` with the finished run if
it completes in time (capped by `server.max_wait_ms`), otherwise `202`. The wait
is best effort: while another request is already waiting (one fewer than
`server.worker_threads`, none with a single worker) it answers `202` at once.

Cancelling a running run rolls back its completed steps if the workflow sets
`rollback`. A nested run (started by a `run_job` step) cannot be cancelled on
its own (`409`); cancel the run that started it.

```bash
curl -H "Authorization: Bearer $ORCHA_SERVER_API_TOKEN" -H 'Prefer: wait=30' \
     -H 'Content-Type: application/json' \
     -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' \
     http://localhost:8070/workflow
```

Every run — API, the dashboard's "Run now" and the cron scheduler — goes
through one bounded queue: `workflow.max_concurrent_runs` (default 4) run at
once and `workflow.max_queued_runs` (default 64) may wait. When the queue is
full, submissions get **`429 Too Many Requests`** with `Retry-After`; a
scheduled run that finds it full is recorded as `skipped`. A nested `run_job`
step runs inside its parent's slot, not through the queue. The admin API has
the same operations for all runs (`POST /api/jobs/{id}/run`,
`GET /api/runs/{id}`, `POST /api/runs/{id}/cancel`), and `GET /api/runs`
reports the queue load and `persistent` (false when the job database could not
be opened and runs are only kept in memory).

Run history keeps the last `jobs.max_runs` finished runs (default 10000,
`0` = unlimited); older ones are pruned automatically.

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
`Workflow timed out after N ms`), and no further steps start. A job started by
a `run_job` step is bound by its caller's deadline too. Shutting the server
down (SIGTERM / Ctrl-C) cancels queued and in-flight runs instead of waiting
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
fails (a step fails or times out) or is cancelled, the steps that completed are rolled back in
reverse completion order. Each command's `rollback(params)` receives the
parameters the step actually ran with (placeholders resolved). `reverse` stops
at the first rollback that fails; `best_effort` continues. Commands whose
metadata does not set `supports_rollback` are reported as `not_supported`. If
the run is stopped by a server *shutdown*, rollback is skipped. Each step result in the
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

The spec covers every public route (`/`, `/workflow`, `/workflow/runs/*`, `/commands`, `/sample`)
plus the admin surface under `/api/*` (jobs, run history, plugin lifecycle).
Admin endpoints are gated by HTTP Basic auth — credentials come from
`admin.username` / `admin.password` in `orcha.yaml` (or
`ORCHA_ADMIN_USERNAME` / `ORCHA_ADMIN_PASSWORD`).
