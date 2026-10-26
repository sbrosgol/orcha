#!/usr/bin/env bash
#
# smoke.sh - End-to-end checks against the real orcha binary.
#
# usage: smoke.sh <scratch-dir> [port=18070]
#
# Starts the debug binary from a fresh run dir inside <scratch-dir> for each
# scenario, checks HTTP behaviour with curl, and stops it with SIGTERM.
# Scenarios: health, /workflow auth matrix (loopback / 0.0.0.0 without token /
# with token), step timeout, admin API auth, SIGTERM during a long workflow
# (cancelled, no leftover children), clean exit when the port is taken.
# Prints PASS/FAIL lines; exits non-zero if any check failed.
set -uo pipefail

SCRATCH="${1:?usage: smoke.sh <scratch-dir> [port]}"
PORT="${2:-18070}"
SRC="$(cd "$(dirname "$0")/../../../.." && pwd)"
BIN="$SRC/cmake-build-debug/orcha"
CMDS="$SRC/cmake-build-debug/commands"
[ -x "$BIN" ] || { echo "build first: cmake --build cmake-build-debug"; exit 2; }
URL="http://127.0.0.1:$PORT"
fails=0
check() { if [ "$2" = "$3" ]; then echo "PASS $1 ($3)"; else echo "FAIL $1 (expected $2, got $3)"; fails=$((fails+1)); fi; }

# start [ENV=VAL ...] — `exec` so $PID is the server itself, not a subshell.
start() {
  W=$(mktemp -d "$SCRATCH/orcha-run.XXXX"); ln -s "$CMDS" "$W/commands"
  (cd "$W" && exec env ORCHA_SERVER_PORT=$PORT ORCHA_JOBS_SCHEDULER_ENABLED=false \
       ORCHA_LOGGING_FILE="$W/orcha.log" "$@" "$BIN" </dev/null >"$W/out.log" 2>&1) &
  PID=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "$URL/" && return 0; sleep 0.2; done
  echo "FAIL server did not start (see $W/out.log)"; fails=$((fails+1)); return 1
}
stop() { kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$@"; }
wf() { code -H 'Content-Type: application/json' "$@" -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' "$URL/workflow"; }

mkdir -p "$SCRATCH"
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then echo "port $PORT busy"; exit 2; fi

echo "== loopback, no token"
start && {
  check "health" 200 "$(code "$URL/")"
  check "/workflow open on loopback" 200 "$(wf)"
  check "bad JSON -> 400" 400 "$(code -H 'Content-Type: application/json' -d '{oops' "$URL/workflow")"
  out=$(curl -s --max-time 20 -H 'Content-Type: application/json' \
        -d '{"steps":[{"command":"shell_exec","timeout_ms":300,"params":{"cmd":"sleep 30"}}]}' "$URL/workflow")
  check "step timeout enforced" yes "$(grep -q 'Step timed out after 300 ms' <<<"$out" && echo yes || echo no)"
  stop; }

echo "== 0.0.0.0, no token (fail closed)"
start ORCHA_SERVER_HOST=0.0.0.0 && { check "/workflow disabled" 503 "$(wf)"; stop; }

echo "== 0.0.0.0 with token, admin enabled"
start ORCHA_SERVER_HOST=0.0.0.0 ORCHA_SERVER_API_TOKEN=smoke-token ORCHA_ADMIN_PASSWORD=smoke-pw && {
  check "no token -> 401" 401 "$(wf)"
  check "wrong token -> 401" 401 "$(wf -H 'Authorization: Bearer nope')"
  check "token -> 200" 200 "$(wf -H 'Authorization: Bearer smoke-token')"
  check "admin Basic accepted" 200 "$(wf -u admin:smoke-pw)"
  check "/api without auth -> 401" 401 "$(code "$URL/api/jobs")"
  check "/api with auth" 200 "$(code -u admin:smoke-pw "$URL/api/circuit-breakers")"
  stop; }

echo "== SIGTERM during a long workflow"
start && {
  curl -s --max-time 40 -H 'Content-Type: application/json' \
    -d '{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}' "$URL/workflow" > "$W/long.json" &
  CURL=$!; sleep 1
  t0=$(date +%s); stop; dt=$(( $(date +%s) - t0 ))
  wait "$CURL"
  check "shutdown under 5s" yes "$([ $dt -lt 5 ] && echo yes || echo no)"
  check "in-flight run cancelled" yes "$(grep -q 'Cancelled' "$W/long.json" && echo yes || echo no)"
  check "no leftover child" no "$(pgrep -f 'sleep 30' >/dev/null && echo yes || echo no)"
}

echo "== port already taken"
start && {
  W2=$(mktemp -d "$SCRATCH/orcha-run.XXXX"); ln -s "$CMDS" "$W2/commands"
  (cd "$W2" && exec env ORCHA_SERVER_PORT=$PORT ORCHA_JOBS_SCHEDULER_ENABLED=false "$BIN" </dev/null >"$W2/out.log" 2>&1)
  check "second instance exits 1" 1 "$?"
  stop; }

[ $fails -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit $fails
