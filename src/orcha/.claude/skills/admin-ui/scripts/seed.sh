#!/usr/bin/env bash
#
# seed.sh - Create demo jobs and a few runs so every dashboard view has data.
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

ids=$(c "$URL/api/jobs" | python3 -c 'import json,sys;print(" ".join(j["id"] for j in json.load(sys.stdin)["jobs"]))')
for id in $ids; do c -X POST "$URL/api/jobs/$id/run" >/dev/null || true; done
echo "seeded: $(c "$URL/api/jobs" | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["jobs"]))') jobs"
