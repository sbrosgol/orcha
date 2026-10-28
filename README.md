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

## YAML Definitions

Workflows and jobs are stored and executed as JSON. You can write them in
YAML in these places:

- **CLI:** `orcha workflow.yaml` runs a YAML workflow.
- **Workflow API:** `POST /workflow` accepts a YAML body sent with
  `Content-Type: application/yaml` (or `application/x-yaml`, `text/yaml`):

  ```bash
  curl -X POST http://localhost:8080/workflow \
       -H "Authorization: Bearer $ORCHA_SERVER_API_TOKEN" \
       -H 'Content-Type: application/yaml' --data-binary @workflow.yaml
  ```
- **Admin job editor:** the *Workflow definition* field has **Visual builder**,
  **JSON** and **YAML** tabs, which stay in sync. The YAML tab checks the text
  as you type and points at the line and column of an error. **Import file**
  (or dropping a file on the field) loads a `.yaml`, `.yml` or `.json` file
  holding either a definition (`steps: ...`) or a whole job (`name`,
  `description`, `schedule_cron`, `enabled`, `definition`). Nothing is saved
  until you save the job.
- **Jobs API:** `POST /api/jobs` and `PUT /api/jobs/{id}` accept YAML bodies
  sent with `Content-Type: application/yaml` (or `application/x-yaml`,
  `text/yaml`). Job fields must have the right type in any body (see
  below), so YAML `enabled: no` is rejected, because `no` is a string; write
  `enabled: false`:

  ```bash
  curl -u admin:change-me -X POST http://localhost:8080/api/jobs \
       -H 'Content-Type: application/yaml' --data-binary @nightly-job.yaml
  ```
- **Conversion:** `POST /api/definitions/convert` takes a definition as YAML or
  JSON and returns both forms (`{"definition": ..., "yaml": "..."}`). Key order
  is kept.

YAML follows the 1.2 core schema. Quoted values are always strings (`'8080'`
stays a string). Unquoted `true`/`false`, `null`/`~`, integers (including
`0x`/`0o`) and decimal numbers are typed. Words such as `yes`/`on`, and
`.inf`/`.nan`, stay strings. Anchors, aliases and merge keys
(`<<: *defaults`) work, and keys a mapping sets itself win over merged ones.
Duplicate keys are an error. Each input must hold exactly one document of at
most 512 KB (larger request bodies get `413`), with at most 64 levels of
nesting and 100,000 values after aliases expand. Comments and formatting are
not kept, because the definition is stored as JSON.

> **Compatibility:** before this change, the CLI also typed *quoted* scalars, so
> `port: "8080"` became the number 8080. Unquoted `True`/`FALSE` were strings
> (now booleans), `0x1F` was the float 31.0 (now the integer 31), and a
> duplicate key silently kept its last value (now an error). Quote a value
> exactly when it must stay a string.

### Job field types

`POST /api/jobs` and `PUT /api/jobs/{id}` reject a field with the wrong type
with `400`, for example `Invalid job: 'enabled' must be true or false (got
"false")`. `name` must be a string, `description` a string or null,
`enabled` a boolean, `schedule_cron` a string or null, and `definition` an
object. Unknown fields are ignored.

> **Behaviour change:** a mistyped field used to be skipped silently, so
> `"enabled": "false"` created an *enabled* job that the scheduler ran.

## Uploading Plugins

Admins can upload a command plugin from the dashboard (**Plugins → Upload
plugin**) instead of copying it into the plugin directory. An installed plugin
runs its code inside the Orcha server with the server's permissions, so
uploads are **off by default** and must be **signed**:

1. **Create a signing key** once, on a machine you trust (needs OpenSSL 3):

   ```bash
   src/orcha/scripts/orcha-plugin-sign.sh keygen orcha-plugins.pem
   # prints the public key, e.g. g6lxoFAKtt9aRy3gzH3iWnARD/txkgswH9mAk0CuR4w=
   ```

2. **Trust it on the server** and turn uploads on (`orcha.yaml`, or
   `ORCHA_PLUGINS_UPLOAD_ENABLED=true` / `ORCHA_PLUGINS_UPLOAD_PUBLIC_KEYS=<key>,<key>`):

   ```yaml
   plugins:
     upload:
       enabled: true
       public_keys: ["g6lxoFAKtt9aRy3gzH3iWnARD/txkgswH9mAk0CuR4w="]
       max_bytes: 33554432               # 32 MB
       directory: "./uploaded-plugins"   # installed uploads; keep it persistent
   ```

   Uploads also require `admin.auth_required: true`. If anything is missing,
   the dashboard's upload dialog says what.

   **Scoping keys.** A plain key may sign any plugin name but may not
   override a bundled plugin. To limit a key to certain names, or to let it
   override bundled plugins, list it as a mapping:

   ```yaml
       public_keys:
         - "g6lx...R4w="                    # any name, no bundled overrides
         - key: "Zk1p...9Qs="
           plugins: ["acme-*", "billing"]   # only these names (* and ? globs)
           override_bundled: true           # may replace bundled plugins
   ```

   Both rules are checked at upload and again at install, against the
   configuration at that moment. A refused upload gets `403` naming the key and
   what it may sign. (`ORCHA_PLUGINS_UPLOAD_PUBLIC_KEYS` sets plain keys only.)

3. **Sign each build** of the plugin. The signature covers the library's
   **file name**, its bytes and its `manifest.json`, so renaming the library
   or swapping the manifest invalidates it. The manifest (or, without one, the
   file name) names the plugin, so the signature decides which plugin an
   upload becomes:

   ```bash
   src/orcha/scripts/orcha-plugin-sign.sh sign orcha-plugins.pem libmy_plugin.so manifest.json
   # writes libmy_plugin.so.sig
   ```

4. **Upload** the library (under the name it was signed with), its `.sig` and
   (optionally) `manifest.json`. The server checks the size, that the file is a
   shared library for this platform and CPU, the manifest, and the signature,
   all without loading the file, and keeps the upload in
   `<plugins.upload.directory>/.orcha-uploads`. Then **review** it (version,
   whether it is an upgrade, downgrade or new plugin, SHA-256, signing key)
   and **install** it.

Uploaded plugins live in `plugins.upload.directory`, apart from the bundled
plugins in `plugins.directory`, and both are loaded at start. Each install goes
to a new directory, `<plugins.upload.directory>/<name>-<id>`. Installing a
plugin whose name already exists replaces it:

- an **uploaded** plugin is replaced: only its library, manifest and
  provenance file are moved aside (other files in its directory stay);
- a **bundled** plugin is **overridden**: the upload loads instead of it, now
  and after restarts, and the bundled files are never modified (so
  `plugins.directory` can be read-only).

Workflows already running keep using the old version.

Installing an **older** version than the one installed (a downgrade) needs
explicit confirmation: the dashboard asks you to tick "Install … even though …
is installed", and the API answers `409` with `"requires": "allow_downgrade"`
until the install request sends `{"allow_downgrade": true}`. This is checked
against the version installed at install time, so an upload that was an
upgrade when staged but has since been overtaken is caught too. If
the new version fails to load (for example it was built against another
plugin ABI), the previous one is restored and reloaded. If the server dies
while installing (a plugin can crash it while loading), the previous version
is put back on the next server start, before plugins are loaded, and the
upload is marked as having crashed the server. Installing also clears a
previous **Disable**.

Every upload, refusal, install, failure and discard is logged at WARN level
with the admin user, the SHA-256 and the key id. Installed plugins keep a
`.orcha-provenance-<name>.json` file (who uploaded and installed it, when,
which key, SHA-256), shown in the plugin's details.

The API is `GET|POST /api/plugins/_uploads`, `POST /api/plugins/_uploads/{id}/install`
and `DELETE /api/plugins/_uploads/{id}` (see `/swagger`).

**What signing does not cover:** the signature is checked at upload. Anyone
who can write to the plugin directories directly (or run a workflow that does,
e.g. with `shell_exec`) can still add a plugin.

> **Docker:** the image sets `plugins.upload.directory` to `/app/data/plugins`,
> on the `orcha-data` volume, so uploaded plugins (and the staging area and
> install journal that make installs crash-safe) survive the container being
> recreated. Bundled plugins stay in the image (`/app/commands`) and update
> with it; an uploaded plugin keeps overriding a bundled one of the same name
> across image upgrades. Do not put `/app/commands` on a volume.

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
