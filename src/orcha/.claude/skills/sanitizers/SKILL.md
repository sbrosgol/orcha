---
name: sanitizers
description: Build Orcha under ThreadSanitizer and AddressSanitizer+UBSan and run the test suites (and ad-hoc repro/stress programs) under them. Use after changing anything concurrent or lifetime-related (threads, locks, futures, jthread/stop tokens, dlopen/plugin lifetime, the HTTP server, the logger), when hunting a race/crash/UB, or when asked for a concurrency or robustness check.
---

# Sanitizer builds and runs

The normal `cmake-build-debug` tree is uninstrumented. Sanitizer trees are built
**out of tree in the session scratchpad**, reusing the already-installed vcpkg
packages (no dependency rebuild).

## Run the suites

```bash
bash .claude/skills/sanitizers/scripts/sanitize.sh <scratchpad>/san 2      # both, 2 runs each
bash .claude/skills/sanitizers/scripts/sanitize.sh <scratchpad>/san 3 tsan # TSan only
```

It configures `build-tsan` and `build-asan` in parallel (incremental on reruns),
runs `test_new_features` and `test_dependency_resolver` N times under each, prints
`exit / tsan / asan/ubsan` counts per run and exits non-zero on any failure or
report. Races are timing-dependent: run TSan at least 2-3 times before calling
something clean. Report "not reproduced" honestly instead of "fixed" when a
suspected race never fired.

## Repro and stress programs

For a suspected bug that the suites don't exercise, write a small program in the
scratchpad that compiles the relevant `.cpp` files directly with the sanitizer
(use a bash script with arrays — zsh will not word-split flags):

```bash
ROOT=<repo>/src/orcha; VI=$ROOT/cmake-build-debug/vcpkg_installed/arm64-osx
COMMON=(-std=c++20 -g -O1 -fno-omit-frame-pointer -I$ROOT -I$ROOT/cmake-build-debug/generated -I$VI/include)
SRCS=(repro.cpp $ROOT/workflow/WorkflowEngine.cpp $ROOT/core/CommandRegistry.cpp
      $ROOT/utils/Logger.cpp $ROOT/jobs/SqliteJobStore.cpp $VI/lib/libyaml-cpp.a $VI/lib/libsqlite3.a)
c++ "${COMMON[@]}" -fsanitize=thread "${SRCS[@]}" -o repro-tsan
c++ "${COMMON[@]}" -fsanitize=address,undefined "${SRCS[@]}" -o repro-asan
```

Patterns that worked: widen race windows with many short steps around a few slow
ones; guard possible hangs with a watchdog thread that prints state and
`std::_Exit`s; for plugin lifetime bugs build a tiny `-shared -fPIC` test plugin
(see `tests/plugins/SlowTestPlugin.cpp`). Once a repro fails, turn it into a
regression test in `tests/` before fixing.

## Known false positive (macOS ASan)

ASan on Darwin does not unregister an unloaded image's globals. When instrumented
dylibs are loaded in turn at the same address (install → refused legacy upgrade →
rollback reload of the same plugin), reading the reloaded plugin's own string
literals reports `global-buffer-overflow` "inside of global variable '.str'
defined in <plugin source>", often listed several times. Confirm by reproducing
with plain `dlopen`/`create_command`/`dlclose` of the real plugin libraries (no
Orcha host code) and checking that the `dladdr` bases match. A toy plugin may not
reproduce it; use the real ones.

The fix in this repo: the test fixture plugins (`orcha_test_slow_plugin`,
`orcha_test_legacy_plugin`) are built with `-fno-sanitize=address` (root
`CMakeLists.txt`). Host code stays instrumented. Do the same for any new test
plugin that tests load more than once, rather than reordering tests (upgrade and
rollback tests have to reload into the freed range) or setting `report_globals=0`
(which switches off global-overflow detection for the whole suite).

## Reading the summary

- A run only counts if it **finished**: each line must end with `All ... Tests
  Passed`. An abort part-way (exit 134, `[FAIL]`) means every test after it went
  unchecked under that sanitizer. Don't pipe the script into `tail`: that hides
  its exit code.
- Timing assertions must survive a sanitizer's slowdown (TSan ~1.5-10x). Assert
  growth ratios (4x input → ~4x time when linear) or orderings, never a fixed
  wall-clock budget (see `test_yaml_large_inputs_are_bounded_and_linear`).

## Reading reports

- TSan: the two stacks are the conflicting accesses; the "Location"/"Mutex" blocks
  name the object. Find the missing lock or the unsynchronised handoff.
- ASan `SEGV on unknown address` inside a plugin call after unload = library
  unmapped while code was running (lifetime bug), `stack-overflow` = unbounded
  recursion (look for recursive JSON walkers on untrusted input).
- Exit 134 with no report usually means `std::terminate` (exception escaped a
  thread, or a joinable `std::thread` destroyed).
