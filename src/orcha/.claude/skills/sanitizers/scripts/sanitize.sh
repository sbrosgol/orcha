#!/usr/bin/env bash
#
# sanitize.sh - Build Orcha under ThreadSanitizer and ASan+UBSan (out of tree)
# and run both test executables under each, N times.
#
# usage: sanitize.sh <out-dir> [runs=2] [tsan|asan|both=both]
#   <out-dir>  scratch directory for the build trees and logs (never the repo)
#
# Reuses the vcpkg packages already installed for cmake-build-debug, so no
# dependency rebuild is needed. Prints one summary line per run and exits
# non-zero if any test failed or any sanitizer report was found.
set -uo pipefail

OUT="${1:?usage: sanitize.sh <out-dir> [runs] [tsan|asan|both]}"
RUNS="${2:-2}"
WHICH="${3:-both}"
SRC="$(cd "$(dirname "$0")/../../../.." && pwd)"           # src/orcha
VI="$SRC/cmake-build-debug/vcpkg_installed"
TOOLCHAIN="${VCPKG_ROOT:-$HOME/vcpkg}/scripts/buildsystems/vcpkg.cmake"
[ -d "$VI" ] || { echo "missing $VI - build cmake-build-debug first"; exit 2; }
mkdir -p "$OUT"

kinds=()
case "$WHICH" in tsan) kinds=(tsan);; asan) kinds=(asan);; *) kinds=(tsan asan);; esac

build() {
  local kind=$1 flags
  if [ "$kind" = tsan ]; then flags="-fsanitize=thread"
  else flags="-fsanitize=address,undefined -fno-sanitize-recover=undefined"; fi
  cmake -S "$SRC" -B "$OUT/build-$kind" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR="$VI" \
    -DCMAKE_CXX_FLAGS="$flags -g -O1 -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
    > "$OUT/configure-$kind.log" 2>&1 \
  && cmake --build "$OUT/build-$kind" -j 4 > "$OUT/build-$kind.log" 2>&1
}

pids=()
for k in "${kinds[@]}"; do build "$k" & pids+=($!); done
status=0
for i in "${!pids[@]}"; do
  if ! wait "${pids[$i]}"; then
    echo "BUILD FAILED (${kinds[$i]}) - see $OUT/build-${kinds[$i]}.log"; status=1
  fi
done
[ $status -eq 0 ] || exit 1

for k in "${kinds[@]}"; do
  for t in test_new_features test_dependency_resolver; do
    for r in $(seq 1 "$RUNS"); do
      log="$OUT/$k-$t-$r.log"
      TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" ASAN_OPTIONS="detect_leaks=0" \
        "$OUT/build-$k/$t" > "$log" 2>&1
      rc=$?
      tsan=$(grep -c 'WARNING: ThreadSanitizer' "$log")
      asan=$(grep -cE 'ERROR: AddressSanitizer|runtime error:' "$log")
      echo "$k $t run$r exit=$rc tsan=$tsan asan/ubsan=$asan | $(tail -1 "$log")"
      [ $rc -eq 0 ] && [ "$tsan" -eq 0 ] && [ "$asan" -eq 0 ] || status=1
    done
  done
done
echo "logs: $OUT"
exit $status
