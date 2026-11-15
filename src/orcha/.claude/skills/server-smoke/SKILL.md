---
name: server-smoke
description: Run the real orcha binary end to end and check user-visible HTTP behaviour (auth on /workflow, bind address, timeouts, shutdown/cancellation, admin API). Use after changing the agent, routes, auth, config, the engine's runtime behaviour or main.cpp, or when asked to confirm a change works on the actual server rather than in unit tests.
---

# End-to-end smoke tests on the real server

Unit tests do not cover process-level behaviour: binding, auth middleware order,
signal handling, shutdown, env-var config. This skill runs the actual binary.

## Standard run

```bash
cmake --build cmake-build-debug -j 8
bash .claude/skills/server-smoke/scripts/smoke.sh <scratchpad>/smoke 18070
```

Covers: health; `/workflow` open on loopback, 503 on `0.0.0.0` without a token,
401/200 with a token, admin Basic accepted; 400 on bad JSON; step `timeout_ms`
enforced; `/api/*` auth; SIGTERM during a 30 s workflow (exits in < 5 s, the run
is reported `Cancelled`, no leftover child processes); a second instance on a busy
port exits 1. Extend the script with a scenario when you add user-visible
behaviour, and keep scenarios benign (no load or attack-style payloads).

Also run `scripts/admin_smoke.sh` (admin dashboard API) when routes or auth change:

```bash
# start a server with ORCHA_ADMIN_PASSWORD=change-me on a free port, then:
ORCHA_URL=http://127.0.0.1:<port> ADMIN_USER=admin ADMIN_PASS=change-me bash scripts/admin_smoke.sh echo
```

Plugin uploads (signing keys, scopes, bundled overrides, downgrades, crash recovery),
when the installer, the upload route or plugin loading change. Needs OpenSSL 3:

```bash
OPENSSL=/opt/homebrew/bin/openssl bash .claude/skills/server-smoke/scripts/upload_smoke.sh <scratchpad>/upsmoke 18090
```

## Writing your own scenario — pitfalls we hit

- Start the server with `(cd "$W" && exec env ... "$BIN") &` so `$!` is the server.
  Without `exec`, `kill $PID` hits the subshell and the server keeps running; on
  macOS a second server can then bind `*:PORT` next to `127.0.0.1:PORT`, and your
  requests silently go to the old one. Check `lsof -nP -iTCP:<port> -sTCP:LISTEN`.
- Use absolute paths inside `(cd "$W" && ...)` — `$PWD` there is the run dir.
- Run from a fresh scratch dir: `main` reads `./orcha.yaml` and writes
  `./orcha-jobs.db` and logs relative to the current directory.
- Config via env: `ORCHA_SERVER_PORT`, `ORCHA_SERVER_HOST`, `ORCHA_SERVER_API_TOKEN`,
  `ORCHA_ADMIN_PASSWORD`, `ORCHA_JOBS_SCHEDULER_ENABLED=false`,
  `ORCHA_WORKFLOW_CIRCUIT_BREAKER_FAILURE_THRESHOLD=2`, `ORCHA_LOGGING_LEVEL=warn`.
- In-memory state (circuit breakers) resets on restart; jobs persist in the run dir's DB.
- Always stop what you started, by its PID (keep it in a pid file), and report the
  PASS/FAIL output verbatim. Do not `pkill -f cmake-build-debug/orcha`: other sessions
  and reviewer agents may have their own servers running.
- Bash defaults like `${2:-{\}}` do not do what they look like (that one yields `{\}`):
  set defaults in a separate statement.
