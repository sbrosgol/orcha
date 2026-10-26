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
| Jobs | `jobs/JobService.hpp`, `jobs/SqliteJobStore.*`, `jobs/JobScheduler.*`, `jobs/RunJobCommand.hpp` |
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
- **Auth**: `POST /workflow` needs `Authorization: Bearer <server.api_token>`
  (or admin Basic). With no token it is served only on a loopback bind, else 503.
  `/api/*` is admin Basic auth. `server.host` defaults to `127.0.0.1`
  (Docker sets `ORCHA_SERVER_HOST=0.0.0.0`).
- **Per-workflow settings**: `timeout_ms`, `"rollback": none|reverse|best_effort`,
  `"circuit_breaker": true`; per step `timeout_ms`, `parallel`, `name`. Invalid
  settings throw `std::invalid_argument` from `WorkflowDefinition::from_json`
  (routes answer 400). The dashboard editor must preserve fields it does not edit.
- **Threads**: background loops use `std::jthread` + `condition_variable_any`
  waits on the stop token (no sleep loops); nothing may let an exception escape a
  thread function. Shutdown order in `main`: cancel runs → scheduler → agent.

Config keys (env override `ORCHA_<SECTION>_<KEY>`, `_`→`.`): `server.{host,port,
worker_threads,api_token}`, `logging.{file,level,console_output}`,
`plugins.{directory,auto_reload,scan_interval_ms,denylist_path}`,
`admin.{enabled,auth_required,username,password,realm}`,
`jobs.{db_path,scheduler_enabled,scheduler_tick_seconds}`,
`workflow.{default_timeout_ms, circuit_breaker.{failure_threshold,success_threshold,reset_timeout_ms}}`.
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
  (`main` lags far behind). Commit only when asked; one focused commit per fix,
  each building and passing tests on its own — see the `ship` skill.
- Ask before decisions that change the public API or behaviour for existing
  clients (auth model, response shapes, defaults).
- Use the session scratchpad for build trees, run dirs and repro programs, never
  the repo.
