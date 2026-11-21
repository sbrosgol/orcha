---
name: new-plugin
description: Write or change an Orcha command plugin (commands/<Name>/ - ICommand implementation, manifest.json, CMake, cooperative cancellation, failure contract, rollback, tests, signing for upload). Use when asked to add a new command/step type, wrap a tool or API as a plugin, or change a bundled plugin's behaviour or parameters.
---

# Command plugins

A plugin is a shared library that exports `create_command()` and the ABI
version, plus a `manifest.json` beside it. Bundled plugins live in
`commands/<Name>/`; the top-level `CMakeLists.txt` globs that directory, so a
new folder with a `CMakeLists.txt` is picked up after re-configuring
(`cmake cmake-build-debug`) and builds into `cmake-build-debug/commands/<Name>/`
with its manifest copied next to it. Copy `commands/ShellExec` (cancellable,
uses `Process.hpp`) or `commands/HttpRequest` (uses `HttpClient.hpp`) rather
than `EchoCommand` (too simple to show the contracts).

## The contract (CLAUDE.md "Contracts")

- **Implement `execute_with_control(params, control)`** and make `execute(params)`
  call it with `{}`. Anything that blocks polls `control.should_stop()` or
  bounds waits with `control.remaining()`. Use `Orcha::Process::run_shell` /
  `commands/HttpClient.hpp`; never `popen`, `system`, `sleep` loops or
  synchronous Beast calls (their timeouts don't apply).
- **Failure**: throw, or return `{"success": false, "error": "<message>"}`.
  Never return a success-looking object for a failure. Distinguish timeout
  from cancellation in the message (see ShellExec).
- **Validate parameters** up front: required ones present, types checked with
  `is_string()`/`is_number_integer()` before `get<>()` (a wrong type is an
  error message, not a crash or a silent default). Override `validate()` when
  the check is cheap and static.
- **Output** is JSON that later steps reference as `{{stepN.output.x}}`: keep
  keys stable and documented, bound its size (truncate large stdout/bodies and
  say so), and don't echo secrets (tokens, passwords, connection strings) into
  output or logs.
- **Rollback**: set `metadata().supports_rollback = true` only if
  `rollback(params)` really undoes the step; it must be idempotent and safe to
  call after a partial `execute`. It currently receives the step's params, not
  its output.
- **ABI**: expand `ORCHA_PLUGIN_ABI_EXPORT()` exactly once; `extern "C"
  create_command()` returns `new` of a `final` class. No globals with
  non-trivial destructors that outlive a call, no background threads that
  outlive `execute` (the library is unloaded when the last command reference
  goes away), no exceptions across the C boundary.
- **Metadata = manifest**: `name`, `version`, `description`, `tags` and every
  parameter (`name,type,required,description,default,example`) must agree
  between `metadata()` and `manifest.json`; the admin UI and `/commands` show
  them. The command `name()` is what workflows use (`snake_case`) and must
  equal the manifest `name` (or, without a manifest, the library file name
  minus `lib` and the extension): the loader refuses a mismatch, because key
  scopes and bundled-override checks are made on that name.
- **No fixed shared paths.** Downloads/extractions go to a per-run temp dir
  (two runs of the same step may run at once), are checksum-verified before
  use, and are cleaned up; never `remove_all` a directory relative to the
  server's working directory. Bound captured output (`HttpClient` body limit,
  truncated stdout).

## Security

Plugins run with the server's privileges. Treat every parameter as untrusted
(it may come from a step output placeholder). Never build a shell string from
parameters unless the plugin's purpose is to run shell (and then say so in the
description); prefer argv-style execution. Restrict schemes/hosts for network
plugins when that is the plugin's job, and never disable TLS verification by
default.

## Tests

- Unit-test the command class directly where it has logic (parameter
  validation, output shape, failure → `success:false`), in a `tests/*.hpp`
  suite registered in `run_new_features_tests()` with `ORCHA_ASSERT*`.
- Test cancellation: run it with an `ExecutionControl` whose deadline is short
  and assert it returns promptly with the timeout message (pattern:
  `tests/TimeoutTests.hpp`).
- Engine-level behaviour (failure stops the workflow, rollback called) uses a
  test plugin built in the root `CMakeLists.txt` like `orcha_test_slow_plugin`.
- End to end: start the server (`server-smoke` skill) and POST a workflow using
  the new command with `Prefer: wait=10`; check `/commands` lists it.

## Shipping it as an upload instead of bundled

Uploaded plugins are packaged and signed with `scripts/orcha-plugin-sign.sh`
(see README "Uploading Plugins"); the signature covers the library file name,
so sign the exact files you upload. Name scopes and `override_bundled` on the
server's key decide whether it may install. Test with the `server-smoke`
upload scenario.

## Checklist before done

Build (`cmake --build cmake-build-debug -j 8`), both test executables pass,
manifest/metadata agree, the README command list (if any) and a sample in
`sample_workflows/` cover it, `cpp-reviewer` on the diff; `concurrency-reviewer`
if it spawns threads or processes.
