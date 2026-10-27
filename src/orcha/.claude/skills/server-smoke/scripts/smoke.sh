#!/usr/bin/env bash
#
# smoke.sh - End-to-end checks against the real orcha binary.
#
# usage: smoke.sh <scratch-dir> [port=18070]
#
# Starts the debug binary from a fresh run dir inside <scratch-dir> for each
# scenario, checks HTTP behaviour with curl, and stops it with SIGTERM.
# Scenarios: health, /workflow auth matrix (loopback / 0.0.0.0 without token /
# with token), async runs (202 + Location, polling, Prefer: wait, cancel, 429
# when the queue is full), step timeout, binary step output, admin API auth,
# nested run cancel (409) vs parent cancel, SIGTERM during a long run
# (cancelled, no leftover children), clean exit when the port is taken (without
# touching the live server's runs), and a crashed server's runs marked
# interrupted on restart.
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
start() { W=$(mktemp -d "$SCRATCH/orcha-run.XXXX"); ln -s "$CMDS" "$W/commands"; start_in "$W" "$@"; }
# start_in <dir> [ENV=VAL ...] — (re)start in an existing run dir (same job DB).
start_in() {
  W=$1; shift
  (cd "$W" && exec env ORCHA_SERVER_PORT=$PORT ORCHA_JOBS_SCHEDULER_ENABLED=false \
       ORCHA_LOGGING_FILE="$W/orcha.log" "$@" "$BIN" </dev/null >"$W/out.log" 2>&1) &
  PID=$!
  for _ in $(seq 1 50); do curl -s -o /dev/null --max-time 1 "$URL/" && return 0; sleep 0.2; done
  echo "FAIL server did not start (see $W/out.log)"; fails=$((fails+1)); return 1
}
stop() { kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; }
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$@"; }
wf() { code -H 'Content-Type: application/json' "$@" -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' "$URL/workflow"; }
# submit <json> [curl args...] -> prints the run id (empty on failure)
submit() { local body=$1; shift
  curl -s --max-time 20 -H 'Content-Type: application/json' "$@" -d "$body" "$URL/workflow" |
    python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("id",""))
except Exception: print("")'; }
status_of() { curl -s --max-time 5 "$URL/workflow/runs/$1" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("status",""))'; }

mkdir -p "$SCRATCH"
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then echo "port $PORT busy"; exit 2; fi

echo "== loopback, no token"
start && {
  check "health" 200 "$(code "$URL/")"
  check "/workflow open on loopback (202)" 202 "$(wf)"
  check "Prefer: wait -> 200" 200 "$(wf -H 'Prefer: wait=10')"
  check "bad JSON -> 400" 400 "$(code -H 'Content-Type: application/json' -d '{oops' "$URL/workflow")"
  hdr=$(curl -s -D - -o /dev/null -H 'Content-Type: application/json' \
        -d '{"steps":[{"command":"echo","params":{}}]}' "$URL/workflow" | tr -d '\r')
  check "202 has Location" yes "$(grep -qi '^Location: /workflow/runs/' <<<"$hdr" && echo yes || echo no)"
  id=$(submit '{"steps":[{"command":"echo","params":{"message":"x"}}]}')
  for _ in $(seq 1 50); do [ "$(status_of "$id")" = success ] && break; sleep 0.1; done
  check "polled run reaches success" success "$(status_of "$id")"
  out=$(curl -s --max-time 20 -H 'Content-Type: application/json' -H 'Prefer: wait=20' \
        -d '{"steps":[{"command":"shell_exec","timeout_ms":300,"params":{"cmd":"sleep 30"}}]}' "$URL/workflow")
  check "step timeout enforced" yes "$(grep -q 'Step timed out after 300 ms' <<<"$out" && echo yes || echo no)"
  out=$(curl -s --max-time 20 -w '\n%{http_code}' -H 'Content-Type: application/json' -H 'Prefer: wait=20' \
        -d '{"steps":[{"command":"shell_exec","params":{"cmd":"printf \"ok \\\\377 end\""}}]}' "$URL/workflow")
  check "binary output -> 200" 200 "$(tail -n1 <<<"$out")"
  check "binary output run succeeds" yes "$(grep -q '"status":"success"' <<<"$out" && echo yes || echo no)"
  stop; }

echo "== run queue full (1 runner, no waiting room)"
start ORCHA_WORKFLOW_MAX_CONCURRENT_RUNS=1 ORCHA_WORKFLOW_MAX_QUEUED_RUNS=0 && {
  busy=$(submit '{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}')
  for _ in $(seq 1 50); do [ "$(status_of "$busy")" = running ] && break; sleep 0.1; done
  hdr=$(curl -s -D - -o /dev/null -H 'Content-Type: application/json' \
        -d '{"steps":[{"command":"echo","params":{}}]}' "$URL/workflow" | tr -d '\r')
  check "queue full -> 429" yes "$(grep -q '^HTTP/1.1 429' <<<"$hdr" && echo yes || echo no)"
  check "429 has Retry-After" yes "$(grep -qi '^Retry-After: [0-9]' <<<"$hdr" && echo yes || echo no)"
  check "cancel running -> 202" 202 "$(code -X POST "$URL/workflow/runs/$busy/cancel")"
  for _ in $(seq 1 50); do [ "$(status_of "$busy")" = cancelled ] && break; sleep 0.1; done
  check "cancelled run recorded" cancelled "$(status_of "$busy")"
  stop; }

echo "== 0.0.0.0, no token (fail closed)"
start ORCHA_SERVER_HOST=0.0.0.0 && { check "/workflow disabled" 503 "$(wf)"; stop; }

echo "== 0.0.0.0 with token, admin enabled"
start ORCHA_SERVER_HOST=0.0.0.0 ORCHA_SERVER_API_TOKEN=smoke-token ORCHA_ADMIN_PASSWORD=smoke-pw && {
  check "no token -> 401" 401 "$(wf)"
  check "wrong token -> 401" 401 "$(wf -H 'Authorization: Bearer nope')"
  check "token -> 202" 202 "$(wf -H 'Authorization: Bearer smoke-token')"
  check "admin Basic accepted" 202 "$(wf -u admin:smoke-pw)"
  check "run status needs token" 401 "$(code "$URL/workflow/runs/x")"
  check "/api without auth -> 401" 401 "$(code "$URL/api/jobs")"
  check "/api with auth" 200 "$(code -u admin:smoke-pw "$URL/api/circuit-breakers")"
  # Nested run: cancel it directly -> 409; cancelling the parent stops both.
  api() { curl -s --max-time 10 -u admin:smoke-pw -H 'Content-Type: application/json' "$@"; }
  jid() { python3 -c 'import json,sys;print(json.load(sys.stdin).get("id",""))'; }
  api -d '{"name":"smoke-child","definition":{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}}' "$URL/api/jobs" >/dev/null
  parent=$(api -d '{"name":"smoke-parent","definition":{"steps":[{"command":"run_job","params":{"job":"smoke-child"}}]}}' "$URL/api/jobs" | jid)
  prun=$(api -X POST "$URL/api/jobs/$parent/run" | jid)
  child_run=""
  for _ in $(seq 1 50); do
    child_run=$(api "$URL/api/runs?limit=10" | python3 -c 'import json,sys
print(next((r["id"] for r in json.load(sys.stdin)["runs"] if r["trigger"]=="nested" and r["status"]=="running"),""))')
    [ -n "$child_run" ] && break; sleep 0.1; done
  check "nested run cancel -> 409" 409 "$(code -u admin:smoke-pw -X POST "$URL/api/runs/$child_run/cancel")"
  check "parent cancel -> 202" 202 "$(code -u admin:smoke-pw -X POST "$URL/api/runs/$prun/cancel")"
  pst=""; for _ in $(seq 1 50); do pst=$(api "$URL/api/runs/$prun" | python3 -c 'import json,sys;print(json.load(sys.stdin)["status"])'); [ "$pst" = cancelled ] && break; sleep 0.1; done
  check "parent run cancelled" cancelled "$pst"
  check "runs list reports persistent" yes "$(api "$URL/api/runs" | grep -q '"persistent":true' && echo yes || echo no)"
  stop; }

echo "== SIGTERM during a long run (client waiting with Prefer: wait)"
start && {
  curl -s --max-time 40 -H 'Content-Type: application/json' -H 'Prefer: wait=40' \
    -d '{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}' "$URL/workflow" > "$W/long.json" &
  CURL=$!; sleep 1
  t0=$(date +%s); stop; dt=$(( $(date +%s) - t0 ))
  wait "$CURL"
  check "shutdown under 5s" yes "$([ $dt -lt 5 ] && echo yes || echo no)"
  check "in-flight run cancelled" yes "$(grep -q '"status":"cancelled"' "$W/long.json" && echo yes || echo no)"
  check "no leftover child" no "$(pgrep -f 'sleep 30' >/dev/null && echo yes || echo no)"
}

echo "== port already taken (second instance shares the job DB)"
start && {
  live=$(submit '{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}')
  for _ in $(seq 1 50); do [ "$(status_of "$live")" = running ] && break; sleep 0.1; done
  (cd "$W" && exec env ORCHA_SERVER_PORT=$PORT ORCHA_JOBS_SCHEDULER_ENABLED=false "$BIN" </dev/null >"$W/out2.log" 2>&1)
  check "second instance exits 1" 1 "$?"
  check "live run left running" running "$(status_of "$live")"
  stop; }

echo "== crashed server: runs marked interrupted on restart"
start && {
  crashed=$(submit '{"steps":[{"command":"shell_exec","params":{"cmd":"sleep 30"}}]}')
  for _ in $(seq 1 50); do [ "$(status_of "$crashed")" = running ] && break; sleep 0.1; done
  kill -KILL "$PID"; wait "$PID" 2>/dev/null
  pkill -f 'sleep 30' 2>/dev/null
  start_in "$W" && {
    check "orphaned run interrupted" interrupted "$(status_of "$crashed")"
    stop; }
}

[ $fails -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit $fails
