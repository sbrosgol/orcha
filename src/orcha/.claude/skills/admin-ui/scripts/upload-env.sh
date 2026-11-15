#!/usr/bin/env bash
#
# upload-env.sh - A plugin-upload test environment for upload-flow.mjs and the
# server-smoke upload scenario.
#
#   upload-env.sh start <workdir> [on_port=8092] [off_port=8093]
#   upload-env.sh stop  <workdir>
#
# start: creates (once) an Ed25519 key and signed fixtures in <workdir>/fixtures
#   (v12 = echo 1.2.0, v13 = echo 1.3.0, clash = "echo-dup", which registers the
#   command "echo" again and so never loads), and runs two servers:
#   - :on_port  uploads enabled, the key scoped to echo* / slow* with
#               override_bundled, bundled plugins copied from the build;
#               prepared state: echo 1.2.0 uploaded (overriding the bundled 1.0.0)
#               and an "echo-dup" upload whose install crashed the server (a
#               journal left behind, rolled back on restart and marked crashed);
#   - :off_port uploads off (the disabled state).
#   Both run from <workdir>/{on,off}; PIDs go to <workdir>/*.pid.
# stop: kills only the PIDs it started (never pkill: other agents may run).
#
# Needs: cmake-build-debug built (orcha + bundled plugins), python3, and
# OpenSSL 3 for signing (OPENSSL=/opt/homebrew/bin/openssl on macOS).
set -euo pipefail

cmd="${1:-}"; WORK="${2:-}"
[ -n "$cmd" ] && [ -n "$WORK" ] || { sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
ON_PORT="${3:-8092}"; OFF_PORT="${4:-8093}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ORCHA="$(cd "$HERE/../../../.." && pwd)"            # src/orcha
BIN="$ORCHA/cmake-build-debug"
SIGN="$ORCHA/scripts/orcha-plugin-sign.sh"
mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"
FX="$WORK/fixtures"

stop_one() { [ -f "$WORK/$1.pid" ] && { kill "$(cat "$WORK/$1.pid")" 2>/dev/null || true; rm -f "$WORK/$1.pid"; }; return 0; }
stop_all() { stop_one on; stop_one off; sleep 0.7; }
start_one() {   # name port
  (cd "$WORK/$1" && exec "$BIN/orcha" > stdout.log 2>&1) & echo $! > "$WORK/$1.pid"
  for _ in $(seq 1 60); do curl -s -o /dev/null "http://127.0.0.1:$2/" && return 0; sleep 0.25; done
  echo "server $1 did not start; see $WORK/$1/stdout.log" >&2; exit 1
}
config() {      # dir port uploads(true|false) key
  cat > "$WORK/$1/orcha.yaml" <<EOF
server:
  host: "127.0.0.1"
  port: $2
plugins:
  directory: "$WORK/$1/commands"
  denylist_path: "./denylist.txt"
  upload:
    enabled: $3
    directory: "./uploaded-plugins"
    public_keys:
      - key: "$4"
        plugins: ["echo*", "slow*"]
        override_bundled: true
admin:
  enabled: true
  auth_required: true
  username: "admin"
  password: "change-me"
jobs:
  db_path: "./jobs.db"
EOF
}
# POST a signed upload: lib sig manifest port -> prints the staged id.
upload() {
  python3 - "$@" <<'PY'
import base64, json, sys, urllib.request
lib, sig, man, port = sys.argv[1:5]
b = lambda p: base64.b64encode(open(p, 'rb').read()).decode()
body = {"library": {"filename": lib.rsplit('/', 1)[-1], "data": b(lib)}, "signature": b(sig), "manifest": b(man)}
req = urllib.request.Request(f"http://127.0.0.1:{port}/api/plugins/_uploads", json.dumps(body).encode(),
    {"Content-Type": "application/json", "Authorization": "Basic " + base64.b64encode(b"admin:change-me").decode()})
print(json.load(urllib.request.urlopen(req))["id"])
PY
}

case "$cmd" in
  stop) stop_all; echo "stopped" ;;
  start)
    stop_all
    # Fixtures (key once; signatures every time, they cover the files).
    mkdir -p "$FX"
    [ -f "$FX/key.pem" ] || "$SIGN" keygen "$FX/key.pem" > /dev/null
    KEY="$("$SIGN" pubkey "$FX/key.pem")"
    LIB="$(cd "$BIN/commands/EchoCommand" && ls libEchoCommand.* | grep -v '\.json$' | head -1)"
    for spec in v12:echo:1.2.0 v13:echo:1.3.0 clash:echo-dup:0.1.0; do
      IFS=: read -r dir name version <<< "$spec"
      mkdir -p "$FX/$dir"; cp "$BIN/commands/EchoCommand/$LIB" "$FX/$dir/"
      python3 -c "import json,sys; m=json.load(open(sys.argv[1])); m.update(name=sys.argv[2], version=sys.argv[3], description='Echoes back the input message (uploaded)'); json.dump(m, open(sys.argv[4],'w'), indent=2)" \
        "$BIN/commands/EchoCommand/manifest.json" "$name" "$version" "$FX/$dir/manifest.json"
      "$SIGN" sign "$FX/key.pem" "$FX/$dir/$LIB" "$FX/$dir/manifest.json" > /dev/null
    done
    echo "$KEY" | "${OPENSSL:-openssl}" base64 -d -A | "${OPENSSL:-openssl}" dgst -sha256 -r | cut -c1-16 > "$FX/key-id"
    echo "$LIB" > "$FX/lib-name"
    # Two fresh run dirs.
    for d in on off; do
      rm -rf "$WORK/$d"; mkdir -p "$WORK/$d/commands"
      for p in "$BIN"/commands/*/; do n=$(basename "$p"); mkdir -p "$WORK/$d/commands/$n"
        cp "$p"/lib* "$p"/manifest.json "$WORK/$d/commands/$n/" 2>/dev/null || true; done
    done
    config on "$ON_PORT" true "$KEY"; config off "$OFF_PORT" false "$KEY"
    start_one on "$ON_PORT"
    # echo 1.2.0 overrides the bundled echo; echo-dup is staged...
    ID="$(upload "$FX/v12/$LIB" "$FX/v12/$LIB.sig" "$FX/v12/manifest.json" "$ON_PORT")"
    curl -s -o /dev/null -u admin:change-me -X POST "http://127.0.0.1:$ON_PORT/api/plugins/_uploads/$ID/install"
    ID="$(upload "$FX/clash/$LIB" "$FX/clash/$LIB.sig" "$FX/clash/manifest.json" "$ON_PORT")"
    # ...and "crashes" the server mid-install: a journal and a half-installed dir.
    stop_one on; sleep 0.7
    UP="$WORK/on/uploaded-plugins"; mkdir -p "$UP/echo-dup-${ID:0:8}"
    python3 -c "import json,sys; json.dump({'name':'echo-dup','was_denied':False,'created_dir':sys.argv[1],'restore':[]}, open(sys.argv[2],'w'))" \
      "$UP/echo-dup-${ID:0:8}" "$UP/.orcha-uploads/$ID/install.json"
    start_one on "$ON_PORT"; start_one off "$OFF_PORT"
    echo "uploads on:  http://127.0.0.1:$ON_PORT   (logs: $WORK/on/stdout.log)"
    echo "uploads off: http://127.0.0.1:$OFF_PORT  (logs: $WORK/off/stdout.log)"
    echo "fixtures:    $FX   (key id $(cat "$FX/key-id"))"
    ;;
  *) sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
