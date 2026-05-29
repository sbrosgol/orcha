# Orcha Admin Dashboard — Phase 2 (Jobs + run history)

Phase 2 adds saved jobs, run history, and an interactive workflow flow chart to
the admin dashboard. Builds on Phase 1 (`docs/dashboard-phase1.md`).

## Scope delivered

- **SQLite persistence.** New `sqlite3` vcpkg dependency, used via the C API
  with a small RAII statement wrapper (no SQLiteCpp port — fewer deps).
- **`IJobStore` / `SqliteJobStore`** — job definitions + run history.
- **`JobService`** — runs a saved job (resolve `WorkflowDefinition` → execute via
  the engine factory → persist a `RunRecord`) and records ad-hoc runs.
- **`JobRoute`** — REST API (auth-gated, same Basic-auth middleware as Phase 1).
- **Ad-hoc recording** — every `POST /workflow` is persisted as a run
  (`job_id=null`, `trigger=api`).
- **Jobs UI** — Plugins/Jobs tabs, job list, create/edit modal, run-now,
  run history, run detail.
- **Interactive Canvas flow chart** — see below.

## Data model (SQLite)

```
jobs(id PK, name UNIQUE, description, definition JSON, schedule_cron, enabled,
     created_at, updated_at)
runs(id PK, job_id NULL, trigger, status, started_at, finished_at, result JSON, error)
  indexes: (job_id, started_at), (started_at)
```

`schedule_cron` is stored now but unused until Phase 3 (scheduler).

## API

```
GET    /api/jobs              list jobs
POST   /api/jobs              create  (body: {name, description?, definition:{steps:[...]}})
GET    /api/jobs/{id}         get
PUT    /api/jobs/{id}         update
DELETE /api/jobs/{id}         delete
POST   /api/jobs/{id}/run     run now -> RunRecord
GET    /api/jobs/{id}/runs    run history (?limit=, default 50)
GET    /api/runs              recent runs across all jobs + ad-hoc (?limit=)
GET    /api/runs/{id}         run detail
```

Validation: `name` required + unique (409 on conflict); `definition.steps` must
be an array. Jobs are gated by the Phase 1 auth middleware (`/api/*`).

## Interactive flow chart (HTML Canvas)

Rendered in the job detail panel. Edges are derived from the workflow's own
templating: a step whose params contain `{{stepN.output...}}` depends on step N
(the same references `PlaceholderResolver` resolves at runtime). Nodes are
auto-laid-out into dependency columns (longest-path layering).

Interactions:
- **Hover** highlights a node + its edges and shows a tooltip with the step's
  command and params.
- **Drag** repositions nodes.
- **Click** selects a node.
- **Run coloring** — after "Run now" (or clicking a row in run history), each
  node is tinted green/red from that run's per-step `success`.
- Re-colors correctly on theme change (reads CSS variables).

No charting library — plain `<canvas>` 2D, DPR-aware.

## Configuration

```yaml
jobs:
  db_path: "./orcha-jobs.db"   # also ORCHA_JOBS_DB_PATH
```

A failed store open does not crash startup — jobs are simply disabled and the
agent receives a null `JobService` (the Jobs API/routes are not registered).

## Wiring

- `bootstrap_services` registers `IJobStore` (SqliteJobStore) + `JobService`
  (wrapped in try/catch).
- `CommandAgent` takes an optional `JobService`: registers `JobRoute` (auth-gated)
  and passes the service to `WorkflowRoute` for ad-hoc run recording.
- New `jobs` static lib links `unofficial::sqlite3::sqlite3`; `agent` links `jobs`.

## Tests

- `tests/JobStoreTests.hpp` — `SqliteJobStore` CRUD, name-uniqueness, run
  insert/list/get (in-memory `:memory:` db), and `JobRoute` path matching.
- `scripts/admin_smoke.sh` extended with job create/run/history/delete + `/api/runs`.

Verified: Debug + Release build clean (no warnings), 25 unit tests pass, smoke
14/14 against a live server.

## Job-to-job linking (`run_job` command)

Jobs compose via a built-in `run_job` command (`jobs/RunJobCommand.hpp`):

```json
{ "steps": [
  { "command": "run_job", "params": { "job": "build-db" } },
  { "command": "echo", "params": { "message": "db said {{step1.output.last.echoed}}" } }
] }
```

- Resolves the referenced job by id, then by name; runs it synchronously via
  `JobService` and returns `{job, run_id, status, result, last}` as the step
  output. `result` is the sub-job's full step-result array; `last` is its final
  step's output (convenience for `{{stepN.output.last.<field>}}`).
- A failed sub-job throws, failing the calling step.
- **Recursion/cycle guard**: a thread-local call stack rejects self-reference and
  cycles, and caps nesting depth at 16.
- Registered in `bootstrap_services` with a `weak_ptr<JobService>` (the registry
  owns the command and `JobService` owns the registry via its engine factory — a
  strong ref would leak).

### Placeholder fix (affects all workflows)

The resolver regex `\{\{step(\d+)\.output(\.\w+)*\}\}` used a *repeated capturing
group*, so `std::regex` kept only its last iteration — nested references like
`{{step1.output.a.b}}` silently lost all but the final segment. Changed to
`((?:\.\w+)*)` so the whole dotted path is captured; `navigate_output` already
walked multi-level paths. `{{job}}` is still not a placeholder (only
`{{stepN.output…}}` is); the dashboard's New-job template was updated to a
resolving example instead of the misleading `{{job}}`.

### Named step references

In addition to positional `{{stepN.output.<path>}}`, steps can be referenced by
name: give a step a `name` and reference it as `{{steps.<name>.output.<path>}}`.
This survives reordering. Positional refs still work; the `steps.` prefix avoids
any clash with `stepN`. An unknown name resolves to empty (not an error).

```json
{ "steps": [
  { "name": "fetch",  "command": "http_request", "params": { "url": "https://api.github.com/zen" } },
  { "name": "report", "command": "echo", "params": { "message": "zen: {{steps.fetch.output.body}}" } }
] }
```

Implementation: `WorkflowStepResult` carries the step `name`; the resolver does a
second pass for the named form; the parallel dependency analyzer and the Canvas
flow chart both map names to steps for edges. Example:
`sample_workflows/http-named-steps.yaml`.

### Test-harness note

`tests/JobStoreTests.hpp` uses `ORCHA_ASSERT` (always-on), not `<cassert>`'s
`assert()`. Release defines `NDEBUG`, which makes `assert()` a no-op that does
not even evaluate its argument — so `assert(store.create_job(...))` would skip
the side effect entirely (and a later non-assert deref then crashed in Release).
The other legacy test files still use plain `assert()` and are effectively
vacuous under Release; converting them is a separate cleanup.

## Not in this phase (Phase 3)

Cron scheduling (`JobScheduler` thread evaluating `schedule_cron`), enable/disable
toggles driving the scheduler, and a persistent plugin disabled-denylist.
