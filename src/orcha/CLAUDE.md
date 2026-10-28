# Orcha

C++20 workflow orchestrator: command plugins loaded with `dlopen`, workflows of
steps (JSON/YAML), an HTTP agent (Boost.Beast) with an embedded admin dashboard,
saved jobs with cron scheduling (SQLite). The repo root holds `Dockerfile`,
`docker-compose.yml` and `README.md`; all code lives here in `src/orcha`.

## Build and test

- Configure/build: `cmake --build cmake-build-debug -j 8` (Ninja, vcpkg toolchain
  already configured; deps: nlohmann_json, Boost (beast/asio/url), yaml-cpp,
  OpenSSL, sqlite3).
- Tests: `./cmake-build-debug/test_new_features` and
  `./cmake-build-debug/test_dependency_resolver`. Both must pass. There is no CTest;
  tests are header suites in `tests/*.hpp` registered in `run_new_features_tests()`
  (`tests/NewFeaturesTests.hpp`) using `ORCHA_ASSERT*` from `tests/Assertions.hpp`
  (always on, unlike `assert`).
- Concurrency/memory changes: also run the suites under ThreadSanitizer and
  ASan+UBSan — see the `sanitizers` skill.
- clangd shows many false errors (missing nlohmann/boost headers). Trust the build.
- zsh does not word-split `$VAR`; put multi-flag compiler/curl invocations in a
  bash script or use arrays.

## Architecture (where to look)

| Area | Files |
|---|---|
| Command interface, plugin ABI | `core/ICommand.hpp`, `core/ExecutionControl.hpp` |
| Registry + plugin loading | `core/CommandRegistry.*`, `core/PluginManager.*`, `core/IPluginDiscovery.hpp` |
| Workflow engine | `workflow/WorkflowEngine.*`, `workflow/IWorkflowEngine.hpp`, `workflow/ExecutionContext.hpp` |
| Circuit breakers | `core/CircuitBreaker.hpp` (one shared `CircuitBreakerRegistry`) |
| HTTP server / routing / auth | `agent/HttpServer.*`, `agent/CommandAgent.*`, `agent/IRouteHandler.hpp`, `agent/AuthMiddleware.hpp`, `agent/routes/*` |
| Admin dashboard | `agent/dashboard_embedded.hpp` (single raw-string page) — use the `admin-ui` skill |
| Run queue (async runs) | `jobs/RunQueue.*` (admission, runners, cancel, wait), `agent/routes/RunResponses.hpp` (run resource, 202/429/409 replies) |
| YAML definitions | `utils/YamlJson.*` (target `yaml_json`; convert + limits), `agent/HttpBody.hpp` (YAML/JSON bodies), `agent/routes/DefinitionRoute.hpp` (`POST /api/definitions/convert`) |
| Jobs | `jobs/JobService.hpp` (`execute` persists a run's lifecycle), `jobs/SqliteJobStore.*`, `jobs/JobScheduler.*`, `jobs/RunJobCommand.hpp` |
| Config | `config/YamlConfiguration.*`, `config/ServerConfig.hpp`, `config/AdminConfig.hpp`, `orcha.yaml.example` |
| Composition root | `main.cpp` (`bootstrap_services`, `run_server_mode`) |
| Bundled plugins | `commands/*` (+ shared `commands/HttpClient.hpp`, `commands/Process.hpp`) |

## Contracts — keep these true

- **Command failure**: a step fails if `execute*` throws or returns an object with
  `"success": false` (`"error"` becomes the message). See `ICommand::execute`.
- **Cancellation/timeouts are cooperative**: the engine calls
  `execute_with_control(params, ExecutionControl)`. Anything that blocks (child
  processes, network) must poll `control.should_stop()` / use `remaining()`.
  Use `commands/Process.hpp` and `commands/HttpClient.hpp` rather than raw
  `popen`/`system` or synchronous Beast calls (Beast timeouts only apply to async ops).
- **Plugin ABI**: every plugin expands `ORCHA_PLUGIN_ABI_EXPORT()` once.
  Changing `ICommand`'s layout (adding/reordering virtuals) requires bumping
  `kPluginAbiVersion` (currently 2) and rebuilding all plugins; the registry
  refuses mismatched plugins.
- **Plugin lifetime**: a loaded command's `shared_ptr` deleter owns the library
  handle. Never `dlclose` manually or keep raw `ICommand*` beyond a call.
- **Per-run state** (job stack for `run_job`, cancellation token) lives in
  `ExecutionContext`, never in `thread_local` globals. Anything that spawns a
  thread for a step must `capture()` and install a `Scope`.
- **Placeholders** (`{{stepN.output.x}}`, `{{steps.name.output.x}}`) resolve in a
  single pass; never re-scan substituted text.
- **Untrusted JSON** from HTTP goes through `parse_json_bounded()` (depth 64).
  YAML bodies (YAML `Content-Type` on `/api/jobs`, `/api/definitions/convert`)
  go through `parse_request_body()` → `Utils::parse_yaml()` (depth 64, 100k
  values after alias expansion, one document).
- **YAML ↔ JSON** lives only in `utils/YamlJson.cpp` (CLI, jobs API, convert
  endpoint; the dashboard calls the endpoint and has no YAML parser). Its header
  must not include yaml-cpp (the agent layer builds without it); `parse_yaml`/
  `json_to_yaml` are compiled for `Json` and `ordered_json` only. Plain
  scalars follow the YAML 1.2 core schema, quoted ones are strings, and
  `json_to_yaml()` must round-trip exactly (it verifies and falls back to
  quoting). Definitions are stored as JSON; YAML comments are not kept.
- **Auth**: `POST /workflow` needs `Authorization: Bearer <server.api_token>`
  (or admin Basic). With no token it is served only on a loopback bind, else 503.
  `/api/*` is admin Basic auth. `server.host` defaults to `127.0.0.1`
  (Docker sets `ORCHA_SERVER_HOST=0.0.0.0`).
- **Per-workflow settings**: `timeout_ms`, `"rollback": none|reverse|best_effort`,
  `"circuit_breaker": true`; per step `timeout_ms`, `parallel`, `name`. Invalid
  settings throw `std::invalid_argument` from `WorkflowDefinition::from_json`
  (routes answer 400), as do mistyped job fields from `JobDefinition::from_json`
  (never skip a present field of the wrong type). The dashboard editor must
  preserve fields it does not edit.
- **Runs are asynchronous**: every top-level run (`POST /workflow`, dashboard
  "Run now", scheduler) goes through the one bounded `RunQueue` (202 + `Location`;
  full → 429 + `Retry-After`; 503 only for "API disabled"/shutting down;
  `Prefer: wait=N` → 200 if done in time). Nested `run_job` runs inline in the
  parent's slot (never queue it: deadlock) and inherits its deadline and
  cancellation; it cannot be cancelled on its own (409). One response shape: the
  run resource. Lifecycle `queued → running → success|failed|cancelled`, plus
  `interrupted` / `skipped`; final statuses never change (`update_run` only
  touches live rows). A user cancel rolls back (per the workflow's `rollback`);
  only shutdown (`EngineOptions::cancel`) skips rollback. Store I/O never runs
  under `RunQueue::mutex_`.
- **Job store**: always exists (in-memory fallback, reported as
  `persistent: false`). Rows carry their process's `owner`; only
  `run_server_mode`, after the agent has bound its port, marks other owners'
  live runs `interrupted`. Schema changes go through `migrate_schema()` in a
  transaction with `PRAGMA user_version`. Serialize stored/sent JSON with the
  UTF-8 `replace` handler (step output may be binary).
- **Threads**: background loops use `std::jthread` + `condition_variable_any`
  waits on the stop token (no sleep loops); nothing may let an exception escape a
  thread function. Shutdown order in `main`: scheduler → cancel runs
  (`run_cancel`) → `RunQueue::stop()` → agent.

Config keys (env override `ORCHA_<SECTION>_<KEY>`, `_`→`.`): `server.{host,port,
worker_threads,api_token,max_wait_ms}`, `logging.{file,level,console_output}`,
`plugins.{directory,auto_reload,scan_interval_ms,denylist_path}`,
`admin.{enabled,auth_required,username,password,realm}`,
`jobs.{db_path,max_runs,scheduler_enabled,scheduler_tick_seconds}`,
`workflow.{default_timeout_ms, max_concurrent_runs, max_queued_runs,
circuit_breaker.{failure_threshold,success_threshold,reset_timeout_ms}}`.
Document new keys in `orcha.yaml.example` and the README; validate in
`config/ConfigValidator.hpp`.

## How we work

- Every fix ships with a regression test that fails without it (or a repro under
  a sanitizer, kept as a test where practical). Verify end to end on the real
  binary when behaviour is user-visible — see the `server-smoke` skill.
- Changes touching the dashboard: `admin-ui` skill, then the `admin-ui-reviewer`
  agent. Design/contract changes: `architecture-reviewer`. C++ API/idiom changes:
  `cpp-reviewer`. Threads, locks, lifetimes, cancellation: `concurrency-reviewer`.
- Git: branch from `dev` (`fix/…`, `feat/…`, `chore/…`), PRs target **`dev`**
  (`main` is updated from `dev`). Commit only when asked; one focused commit per fix,
  each building and passing tests on its own — see the `ship` skill.
- Ask before decisions that change the public API or behaviour for existing
  clients (auth model, response shapes, defaults).
- Use the session scratchpad for build trees, run dirs and repro programs, never
  the repo.
