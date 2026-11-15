#!/usr/bin/env bash
#
# upload_smoke.sh - Plugin uploads end to end on the real binary (API only).
#
#   upload_smoke.sh <workdir> [port=18090]
#
# Two keys: PLAIN (any name, no bundled overrides) and SCOPED (echo* only,
# override_bundled). Checks: uploads off -> 403; a plain key overriding the
# bundled echo -> 403; the scoped key -> staged, installed, loaded from the
# upload directory, bundled files untouched, a workflow runs on it; a renamed
# library and a forged signature -> 422; an out-of-scope name -> 403; a
# downgrade -> 409 until confirmed; a crashed install is rolled back at the
# next start and the upload marked; the override survives the restart.
#
# Needs: cmake-build-debug built, python3, OpenSSL 3 (OPENSSL=... on macOS).
# Uses only its own process (PID file); never pkill.
set -uo pipefail

WORK="${1:-}"; PORT="${2:-18090}"
[ -n "$WORK" ] || { sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
ORCHA="$(cd "$HERE/../../../.." && pwd)"
BIN="$ORCHA/cmake-build-debug"
SIGN="$ORCHA/scripts/orcha-plugin-sign.sh"
OPENSSL="${OPENSSL:-openssl}"; export OPENSSL
rm -rf "$WORK"; mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"
U="http://127.0.0.1:$PORT"; A=(-s -u admin:change-me)
FAILS=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
expect() { [ "$2" = "$3" ] && pass "$1" || fail "$1 (got: $2, want: $3)"; }

stop() { [ -f "$WORK/orcha.pid" ] && { kill "$(cat "$WORK/orcha.pid")" 2>/dev/null; rm -f "$WORK/orcha.pid"; sleep 0.7; }; return 0; }
start() {
  (cd "$WORK" && exec "$BIN/orcha" >> stdout.log 2>&1) & echo $! > "$WORK/orcha.pid"
  for _ in $(seq 1 60); do curl -s -o /dev/null "$U/" && return 0; sleep 0.25; done
  echo "server did not start; see $WORK/stdout.log"; exit 1
}
trap stop EXIT
config() {   # enabled
  cat > "$WORK/orcha.yaml" <<EOF
server: { host: "127.0.0.1", port: $PORT }
plugins:
  directory: "$WORK/commands"
  denylist_path: "./denylist.txt"
  upload:
    enabled: $1
    directory: "./uploaded-plugins"
    public_keys:
      - "$PLAIN"
      - key: "$SCOPED"
        plugins: ["echo*"]
        override_bundled: true
admin: { enabled: true, auth_required: true, username: admin, password: change-me }
jobs: { db_path: "./jobs.db" }
EOF
}
# fixture <dir> <key.pem> <name> <version> [filename]: a signed copy of the echo plugin.
fixture() {
  mkdir -p "$WORK/fx/$1"
  cp "$BIN/commands/EchoCommand/$LIB" "$WORK/fx/$1/${5:-$LIB}"
  python3 -c "import json,sys; json.dump({'name':sys.argv[1],'version':sys.argv[2],'description':'smoke'}, open(sys.argv[3],'w'))" \
    "$3" "$4" "$WORK/fx/$1/manifest.json"
  "$SIGN" sign "$2" "$WORK/fx/$1/${5:-$LIB}" "$WORK/fx/$1/manifest.json" > /dev/null
}
# upload <dir> [filename-to-send] -> "<http status> <body>"
upload() {
  python3 - "$WORK/fx/$1" "$LIB" "${2:-$LIB}" "$PORT" <<'PY'
import base64, json, os, sys, urllib.error, urllib.request
d, lib, name, port = sys.argv[1:5]
src = os.path.join(d, lib) if os.path.exists(os.path.join(d, lib)) else os.path.join(d, name)
b = lambda p: base64.b64encode(open(p, 'rb').read()).decode()
body = {"library": {"filename": name, "data": b(src)}, "signature": b(src + ".sig"), "manifest": b(os.path.join(d, "manifest.json"))}
req = urllib.request.Request(f"http://127.0.0.1:{port}/api/plugins/_uploads", json.dumps(body).encode(),
    {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(b"admin:change-me").decode()})
try:
    r = urllib.request.urlopen(req); print(r.status, r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
PY
}
id_of() { cut -d' ' -f2- | python3 -c 'import json,sys; print(json.load(sys.stdin).get("id",""))'; }
install() {   # id [json body, default {}]
  local body="${2:-}"; [ -n "$body" ] || body='{}'
  curl "${A[@]}" -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' -d "$body" -X POST "$U/api/plugins/_uploads/$1/install"
}
field() { python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

# Keys, bundled plugins, fixtures.
"$SIGN" keygen "$WORK/plain.pem" > /dev/null || { echo "needs OpenSSL 3 (set OPENSSL)"; exit 1; }
"$SIGN" keygen "$WORK/scoped.pem" > /dev/null
PLAIN="$("$SIGN" pubkey "$WORK/plain.pem")"; SCOPED="$("$SIGN" pubkey "$WORK/scoped.pem")"
LIB="$(cd "$BIN/commands/EchoCommand" && ls libEchoCommand.* | head -1)"
for p in "$BIN"/commands/*/; do n=$(basename "$p"); mkdir -p "$WORK/commands/$n"; cp "$p"/lib* "$p"/manifest.json "$WORK/commands/$n/" 2>/dev/null; done
BUNDLED_SHA="$(shasum -a 256 "$WORK/commands/EchoCommand/$LIB" | cut -c1-64)"
fixture plain  "$WORK/plain.pem"  echo 1.2.0
fixture v12    "$WORK/scoped.pem" echo 1.2.0
fixture v11    "$WORK/scoped.pem" echo 1.1.0
fixture other  "$WORK/scoped.pem" billing 1.0.0
fixture dup    "$WORK/scoped.pem" echo-dup 0.1.0

# 1. Uploads off.
config false; start
expect "uploads off -> 403" "$(upload v12 | cut -d' ' -f1)" 403
stop

# 2-6. Uploads on.
config true; start
OUT="$(upload plain)"
expect "plain key may not override bundled echo -> 403" "${OUT%% *}" 403
case "$OUT" in *override_bundled*) pass "  ...and says override_bundled is needed" ;; *) fail "  message: $OUT" ;; esac
OUT="$(upload v12)"; expect "scoped key stages echo 1.2.0 -> 201" "${OUT%% *}" 201
ID="$(echo "$OUT" | id_of)"
expect "install -> 200" "$(install "$ID")" 200
P="$(curl "${A[@]}" "$U/api/plugins/echo")"
expect "echo loaded from the upload directory" "$(echo "$P" | field 'd["version"]+" "+d["source"]+" over "+d["overrides"]["version"]')" "1.2.0 uploaded over 1.0.0"
expect "bundled files untouched" "$(shasum -a 256 "$WORK/commands/EchoCommand/$LIB" | cut -c1-64)" "$BUNDLED_SHA"
RUN="$(curl "${A[@]}" -H 'Prefer: wait=5' -H 'Content-Type: application/json' -d '{"steps":[{"command":"echo","params":{"message":"hi"}}]}' "$U/workflow")"
expect "a workflow runs on it" "$(echo "$RUN" | field 'd["status"]')" success
expect "renamed library -> 422" "$(upload v12 libRenamed.${LIB##*.} | cut -d' ' -f1)" 422
expect "out-of-scope name -> 403" "$(upload other | cut -d' ' -f1)" 403
# A signature that is not for these files (made for other files, by the other key).
cp "$WORK/fx/v11/$LIB.sig" "$WORK/fx/v11/$LIB.sig.ok"; cp "$WORK/fx/plain/$LIB.sig" "$WORK/fx/v11/$LIB.sig"
expect "signature not for these files -> 422" "$(upload v11 | cut -d' ' -f1)" 422
mv "$WORK/fx/v11/$LIB.sig.ok" "$WORK/fx/v11/$LIB.sig"
ID="$(upload v11 | id_of)"
expect "downgrade 1.2.0 -> 1.1.0 without confirmation -> 409" "$(install "$ID")" 409
expect "  ...with {allow_downgrade: true} -> 200" "$(install "$ID" '{"allow_downgrade":true}')" 200

# 7. A crashed install is rolled back at the next start.
ID="$(upload dup | id_of)"
stop
HALF="$WORK/uploaded-plugins/echo-dup-${ID:0:8}"; mkdir -p "$HALF"
python3 -c "import json,sys; json.dump({'name':'echo-dup','was_denied':False,'created_dir':sys.argv[1],'restore':[]}, open(sys.argv[2],'w'))" \
  "$HALF" "$WORK/uploaded-plugins/.orcha-uploads/$ID/install.json"
start
[ ! -d "$HALF" ] && pass "half-installed directory removed" || fail "half-installed directory still there"
grep -q "Rolled back an interrupted install of plugin echo-dup" "$WORK/stdout.log" && pass "rollback logged" || fail "rollback not logged"
expect "upload marked as having crashed the server" \
  "$(curl "${A[@]}" "$U/api/plugins/_uploads" | field 'bool([u for u in d["uploads"] if u["name"]=="echo-dup" and u["install_crashed_at"]])')" True
expect "override survives the restart" "$(curl "${A[@]}" "$U/api/plugins/echo" | field 'd["version"]+" "+d["source"]')" "1.1.0 uploaded"

echo
[ "$FAILS" -eq 0 ] && echo "ALL UPLOAD CHECKS PASSED" || echo "$FAILS FAILED (log: $WORK/stdout.log)"
exit $((FAILS > 0))
