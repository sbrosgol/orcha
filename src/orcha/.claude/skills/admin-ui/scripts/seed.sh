#!/usr/bin/env bash
#
# seed.sh - Create demo jobs and a few runs so every dashboard view has data
# (including a rolled-back run and an open circuit breaker).
#
# usage: ORCHA_URL=http://127.0.0.1:8091 ADMIN_USER=admin ADMIN_PASS=change-me seed.sh
#
set -euo pipefail
URL="${ORCHA_URL:-http://127.0.0.1:8091}"
AUTH="${ADMIN_USER:-admin}:${ADMIN_PASS:-change-me}"
c() { curl -sf -u "$AUTH" "$@"; }

for name in nightly-report health-check db-backup; do
  c -X POST "$URL/api/jobs" -H 'Content-Type: application/json' -d @- >/dev/null <<EOF || echo "skip $name (exists?)"
{"name":"$name","description":"Demo $name job","enabled":true,"schedule_cron":"0 9 * * 1-5",
 "definition":{"steps":[
   {"name":"greet","command":"echo","params":{"message":"hi"}},
   {"name":"repeat","command":"echo","params":{"message":"{{steps.greet.output.echoed}}"}}]}}
EOF
done

# A failing job so failed badges / red flow nodes / error output render too.
c -X POST "$URL/api/jobs" -H 'Content-Type: application/json' >/dev/null -d \
  '{"name":"broken-step","description":"Fails on purpose","enabled":false,"definition":{"steps":[{"name":"boom","command":"no_such_command","params":{}}]}}' \
  || echo "skip broken-step (exists?)"

# A failing job with rollback enabled, so per-step rollback outcomes render.
c -X POST "$URL/api/jobs" -H 'Content-Type: application/json' >/dev/null -d \
  '{"name":"deploy-with-rollback","description":"Fails after a completed step; rollback enabled","enabled":false,
    "definition":{"rollback":"reverse","timeout_ms":60000,"steps":[
      {"name":"prepare","command":"echo","params":{"message":"prepared"}},
      {"name":"deploy","command":"shell_exec","timeout_ms":5000,"params":{"cmd":"echo deploying; exit 2"}}]}}' \
  || echo "skip deploy-with-rollback (exists?)"

# A job that opts into circuit breaking and keeps failing, so a breaker opens.
c -X POST "$URL/api/jobs" -H 'Content-Type: application/json' >/dev/null -d \
  '{"name":"flaky-api","description":"Downstream is down; circuit breaker enabled","enabled":false,
    "definition":{"circuit_breaker":true,"steps":[{"name":"call","command":"shell_exec","params":{"cmd":"exit 1"}}]}}' \
  || echo "skip flaky-api (exists?)"

ids=$(c "$URL/api/jobs" | python3 -c 'import json,sys;print(" ".join(j["id"] for j in json.load(sys.stdin)["jobs"]))')
for id in $ids; do c -X POST "$URL/api/jobs/$id/run" >/dev/null || true; done

# Run flaky-api past the default failure threshold (5) so its circuit opens and
# the last run is rejected without executing.
flaky=$(c "$URL/api/jobs" | python3 -c 'import json,sys;print(next((j["id"] for j in json.load(sys.stdin)["jobs"] if j["name"]=="flaky-api"),""))')
if [ -n "$flaky" ]; then for _ in 1 2 3 4 5; do c -X POST "$URL/api/jobs/$flaky/run" >/dev/null || true; done; fi
echo "seeded: $(c "$URL/api/jobs" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["jobs"]))') jobs"
