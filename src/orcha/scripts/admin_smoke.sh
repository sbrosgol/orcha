#!/usr/bin/env bash
#
# admin_smoke.sh - Manual smoke test for the Phase 1 admin dashboard.
#
# Starts nothing; assumes Orcha is already running with the admin dashboard
# enabled. Configure credentials to match your orcha.yaml / ORCHA_ADMIN_*.
#
# Usage:
#   ORCHA_URL=http://localhost:8070 ADMIN_USER=admin ADMIN_PASS=change-me \
#     scripts/admin_smoke.sh [plugin_name]
#
set -uo pipefail

URL="${ORCHA_URL:-http://localhost:8070}"
USER="${ADMIN_USER:-admin}"
PASS="${ADMIN_PASS:-change-me}"
PLUGIN="${1:-}"

pass=0
fail=0

check() { # description expected_code actual_code
  if [[ "$2" == "$3" ]]; then
    echo "  [PASS] $1 ($3)"; pass=$((pass+1))
  else
    echo "  [FAIL] $1 (expected $2, got $3)"; fail=$((fail+1))
  fi
}

code() { # method path [curl args...]
  local method="$1"; local path="$2"; shift 2
  curl -s -o /dev/null -w '%{http_code}' -X "$method" "$@" "$URL$path"
}

echo "== Orcha admin smoke test against $URL =="

echo "-- Admin API unauthenticated (expect 401) --"
check "GET /api/plugins  no creds"  401 "$(code GET /api/plugins)"

echo "-- Public endpoints / login shell unaffected (expect 200) --"
check "GET /            no creds"   200 "$(code GET /)"
check "GET /commands    no creds"   200 "$(code GET /commands)"
# /admin serves the login shell unauthenticated (no data); the SPA logs in to /api/*.
check "GET /admin        no creds"  200 "$(code GET /admin)"

echo "-- Authenticated (expect 200) --"
check "GET /api/plugins  with creds" 200 "$(code GET /api/plugins -u "$USER:$PASS")"
check "GET /api/plugins/_watch"      200 "$(code GET /api/plugins/_watch -u "$USER:$PASS")"

echo "-- Bad credentials (expect 401) --"
check "GET /api/plugins  wrong pass" 401 "$(code GET /api/plugins -u "$USER:wrong")"

echo "-- Plugin listing --"
curl -s -u "$USER:$PASS" "$URL/api/plugins" | sed 's/,/,\n/g' | head -30

if [[ -n "$PLUGIN" ]]; then
  echo "-- Reload '$PLUGIN' (expect 200 if loaded) --"
  check "POST /api/plugins/$PLUGIN/reload" 200 \
    "$(code POST "/api/plugins/$PLUGIN/reload" -u "$USER:$PASS")"
fi

echo "-- Jobs API --"
JBODY='{"name":"smoke-job","definition":{"steps":[{"command":"echo","params":{"message":"hi"}}]}}'
check "GET  /api/jobs no creds (401)" 401 "$(code GET /api/jobs)"
check "POST /api/jobs (create, 201)" 201 \
  "$(code POST /api/jobs -u "$USER:$PASS" -H 'Content-Type: application/json' -d "$JBODY")"
JID=$(curl -s -u "$USER:$PASS" "$URL/api/jobs" \
  | sed 's/{/\n{/g' | grep 'smoke-job' | grep -oE '"id":"[a-f0-9]+"' | head -1 | cut -d'"' -f4)
if [[ -n "$JID" ]]; then
  check "POST /api/jobs/{id}/run (200)" 200 "$(code POST "/api/jobs/$JID/run" -u "$USER:$PASS")"
  check "GET  /api/jobs/{id}/runs (200)" 200 "$(code GET "/api/jobs/$JID/runs" -u "$USER:$PASS")"
  check "DELETE /api/jobs/{id} (200)" 200 "$(code DELETE "/api/jobs/$JID" -u "$USER:$PASS")"
else
  echo "  [WARN] could not capture created job id; skipping run/delete checks"
fi
check "GET  /api/runs (200)" 200 "$(code GET /api/runs -u "$USER:$PASS")"

echo
echo "== $pass passed, $fail failed =="
[[ "$fail" -eq 0 ]]
