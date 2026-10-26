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

ASan on Darwin does not unregister an unloaded image's globals. If a **different**
instrumented dylib is later mapped at the address of one that was `dlclose`d, any
read of its string literals reports `global-buffer-overflow` "inside of global
variable '.str' defined in <old plugin>". Confirm by reproducing with plain
`dlopen`/`dlclose`/`dlopen` (no Orcha code) and by checking `dladdr` bases match.
Avoid it in tests by ordering loads so nothing is mapped over a refused/unloaded
plugin (see `test_plugin_abi_version_checked`), not by changing product code.

## Reading reports

- TSan: the two stacks are the conflicting accesses; the "Location"/"Mutex" blocks
  name the object. Find the missing lock or the unsynchronised handoff.
- ASan `SEGV on unknown address` inside a plugin call after unload = library
  unmapped while code was running (lifetime bug), `stack-overflow` = unbounded
  recursion (look for recursive JSON walkers on untrusted input).
- Exit 134 with no report usually means `std::terminate` (exception escaped a
  thread, or a joinable `std::thread` destroyed).
