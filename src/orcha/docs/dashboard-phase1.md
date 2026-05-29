# Orcha Admin Dashboard — Phase 1 Implementation Plan

Phase 1 of the admin dashboard. Exposes the existing `PluginManager` over an
authenticated HTTP/JSON API and ships a single embedded HTML page to drive it.

**No new third-party dependencies.** OpenSSL, cpprestsdk, and `PluginManager`
are already in the build.

## Scope

In scope:
- `BasicAuth` middleware gating `/api/*` and `/admin`.
- `PluginAdminRoute` — list / inspect / reload / enable / disable plugins, toggle the watcher.
- `DashboardRoute` — serves `/admin` (embedded HTML/JS).
- DI wiring so `CommandAgent` receives `PluginManager`, `IPluginDiscovery`, the
  plugin directory, and admin credentials.
- Unit tests + a manual curl smoke script.

Out of scope (later phases):
- Jobs, run history, scheduling, SQLite (Phase 2/3).
- Per-plugin parameter editing, persistent "disabled" denylist, TLS.

## Key design constraint

`PluginManager::unload_plugin()` erases the plugin from `loaded_plugins_`,
including its `library_path`. Once a plugin is "disabled" (unloaded), the manager
no longer knows where it lives. Phase 1 therefore computes the set of *available*
plugins from disk via `IPluginDiscovery::scan_plugins(directory)` and diffs it
against the *loaded* set from `PluginManager::get_all_plugins()`:

- **loaded**    = `get_all_plugins()`
- **available** = `scan_plugins(directory)`
- **enable(name)** = look up the library path in the available set, then `load_plugin(path)`.

Disable only unloads from memory; it does not delete the file. The directory
watcher (`watch_thread_func`) currently only *logs* changes, so a disabled plugin
will not silently reload. A persistent disabled-denylist that survives restart is
deferred to Phase 2.

## Files

```
agent/
  AuthMiddleware.hpp              NEW  Basic-auth guard + base64 + constant-time compare
  IRouteHandler.hpp               EDIT add middleware support to Router; add path helpers
  dashboard_embedded.hpp          NEW  embedded /admin HTML/JS
  routes/
    PluginAdminRoute.hpp          NEW  /api/plugins...
    DashboardRoute.hpp            NEW  /admin
  CommandAgent.hpp / .cpp         EDIT new ctor params + register routes + install middleware
config/
  YamlConfiguration.hpp           EDIT add AdminConfig struct
main.cpp                          EDIT pass deps into CommandAgent
tests/
  PluginAdminTests.hpp            NEW  unit tests
  NewFeaturesTests.hpp            EDIT call new tests from run_new_features_tests()
CMakeLists.txt                    EDIT add new headers to agent target (no new libs)
scripts/admin_smoke.sh            NEW  manual curl smoke test
```

## Config (`admin` section)

```yaml
admin:
  enabled: true
  auth_required: true
  username: admin
  password: change-me      # overridable via ORCHA_ADMIN_PASSWORD
  realm: "Orcha Admin"
```

Fail-closed rule: if `auth_required` is true but `password` is empty, log a
warning and do **not** register admin routes — never expose an unauthenticated
config-mutating API.

## Admin API

| Method | Path | Maps to |
|---|---|---|
| `GET`  | `/api/plugins` | loaded + available diff, each tagged `status` |
| `GET`  | `/api/plugins/{name}` | `get_plugin_metadata(name)` (404 if absent) |
| `POST` | `/api/plugins/{name}/reload` | `reload_plugin(name)` |
| `POST` | `/api/plugins/{name}/disable` | `unload_plugin(name)` |
| `POST` | `/api/plugins/{name}/enable` | resolve path from scan, `load_plugin(path)` |
| `GET`  | `/api/plugins/_watch` | `is_watching()` |
| `PUT`  | `/api/plugins/_watch` | body `{ "enabled": bool }` |
| `GET`  | `/admin` | dashboard UI |

`GET /api/plugins` response:

```json
{
  "watching": false,
  "directory": "./commands",
  "plugins": [
    { "name": "http_request", "version": "2.0.0", "status": "loaded",
      "description": "...", "author": "...", "tags": [],
      "dependencies": [], "library_path": "./commands/.../x.dylib",
      "commands": ["http_request"] }
  ]
}
```

## Middleware + Router

`Router` gains an optional middleware chain. A middleware returns `true` if it
handled (rejected) the request, short-circuiting dispatch. The auth middleware
guards only paths under `/api/`; everything else (including the `/admin` shell)
passes through, so existing public endpoints are unaffected.

Auth notes:
- base64 decode hand-rolled (avoid `EVP_DecodeBlock` padding quirks).
- constant-time credential compare to avoid timing leaks.
- The 401 reply deliberately omits `WWW-Authenticate: Basic` so the browser does
  NOT pop its native login dialog. The dashboard renders a custom login view and
  attaches `Authorization: Basic <base64(user:pass)>` to its `fetch()` calls.

## Login view & theming (custom UI)

- `/admin` is served unauthenticated — it carries no data, just the SPA shell.
  On load the SPA shows a custom login card; on success it stores the
  `Authorization` token in `sessionStorage` and attaches it to every API call.
  A 401 from any call clears the token and returns to the login view. A "Sign
  out" button clears it on demand. Credentials are validated by calling
  `GET /api/plugins` with the entered token (no dedicated login endpoint).
- **Themes: Light / Dark / System.** A switcher (top-right, visible on the login
  view too) writes the choice to `localStorage` (`orcha_theme`); an inline
  `<head>` script applies it before first paint to avoid a flash. CSS variables
  define the light (default) and dark palettes; `data-theme="system"` follows
  `prefers-color-scheme` via a media query, while `light`/`dark` force a palette.

## Build order

1. `AdminConfig` in `YamlConfiguration.hpp` + default `admin:` block.
2. `Router::use` + path helpers in `IRouteHandler.hpp`.
3. `AuthMiddleware.hpp` + unit tests.
4. `PluginAdminRoute.hpp` + unit tests (mock registry/discovery).
5. `DashboardRoute.hpp` + `dashboard_embedded.hpp`.
6. `CommandAgent` ctor/route wiring; `main.cpp` deps + banner.
7. CMake header/link updates; build debug + release.
8. Manual smoke script; docs.

Each step compiles and tests green before the next; the dashboard is demoable
after step 6.

## Bugs found & fixed during implementation

Exposing `PluginManager` over HTTP surfaced two latent bugs that crashed the
server on the first real `disable`/`reload`. Both are fixed:

1. **Re-entrant lock in `PluginManager::unload_plugin`.** It held `mutex_` for
   the whole function and then called `notify_*`, which re-acquire the same
   non-recursive `std::mutex` (UB). Restructured to release the lock before the
   registry call and notifications (matching `load_plugin`).

2. **Use-after-`dlclose` in `CommandRegistry::unregister_command`.** It called
   `dlclose(handle)` before the last `shared_ptr` to the command was released,
   so the command's destructor ran on unmapped code. Reordered to drop all
   command references first, then `dlclose`.

Also: `PluginManager` previously unregistered by *plugin* name, but the registry
is keyed by *command* name (often different, e.g. plugin `EchoCommand` →
command `echo`), so unload never succeeded. `PluginManager` now records the
command name(s) each plugin registers (via a before/after diff of
`registry->list_commands()`) and unregisters those on unload — making
disable/enable/reload actually functional.

## Security caveats (document in README)

- Basic auth over plain HTTP sends credentials base64-encoded (not encrypted) —
  front with TLS / restrict to trusted networks for remote use.
- Fail closed when `auth_required && password.empty()`.
- `library_path` is exposed in API responses (leaks server paths) — acceptable
  for an admin tool but noted.
- The SPA keeps the Basic token in `sessionStorage` (cleared on tab close / sign
  out); same reversible-base64 exposure as Basic auth itself. TLS is the real
  protection for credentials in transit.
