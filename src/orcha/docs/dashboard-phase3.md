# Orcha Admin Dashboard — Phase 3 (scheduling + plugin denylist)

Phase 3 adds cron scheduling for jobs, enable/disable controls, and a persistent
plugin disabled-denylist. Builds on Phases 1-2.

## Scope delivered

- **Cron scheduler.** `JobScheduler` runs on a background thread, evaluating
  enabled jobs with a `schedule_cron` each tick and firing those due in the
  current UTC minute (de-duplicated per minute, trigger `schedule`).
- **Cron parser.** `CronExpr` — hand-rolled 5-field parser/matcher (no new deps).
- **Schedule + enable UI.** Job editor has a cron field and an Enabled toggle;
  the detail view shows the schedule + status and a quick Disable/Enable button;
  the job list marks disabled/scheduled jobs.
- **Persistent plugin denylist.** Disabling a plugin records its name so it stays
  unloaded across restarts; enabling clears it; reload does not touch it.

## Cron (`CronExpr`)

Fields: `minute(0-59) hour(0-23) day-of-month(1-31) month(1-12) day-of-week(0-6, Sun=0)`.
Per field: `*`, `a`, `a-b`, `a-b/n`, `*/n`, and comma lists. `dow` also accepts 7
for Sunday. Day-of-month / day-of-week use the classic rule: if BOTH are
restricted, a tick matches when EITHER matches; otherwise they are ANDed.
All matching is UTC.

## Scheduler (`JobScheduler`)

- Ticks every `jobs.scheduler_tick_seconds` (default 30; must be < 60 so no
  minute is missed). Each tick reads jobs fresh, so edits are picked up live.
- `evaluate(tm)` is public and pure (no threads/clock) for deterministic tests;
  the thread calls it with the current UTC time.
- De-dups by `(job id, "YYYY-MM-DDTHH:MM")` so a job fires at most once per minute.
- Invalid cron expressions are warned once and skipped (not fatal).
- Runs jobs synchronously through `JobService` (so runs are recorded and the
  store's single-writer discipline holds). A slow job delays later jobs in the
  same tick but they still fire with the correct minute key.

Config:
```yaml
jobs:
  scheduler_enabled: true
  scheduler_tick_seconds: 30
```
The scheduler runs only in server mode and stops on shutdown.

## Plugin denylist (`IPluginDenylist` / `FilePluginDenylist`)

- File-backed (one plugin name per line) at `plugins.denylist_path`
  (default `./orcha-disabled-plugins.txt`).
- `PluginManager::load_plugins_from_directory` skips denylisted plugins (logged).
- `PluginAdminRoute`: **disable** unloads + adds to the denylist; **enable**
  removes from the denylist + loads; **reload** is unchanged. Available-on-disk
  plugins that are denylisted report status `disabled` (vs `available`).

## Wiring

- `bootstrap_services` registers `IPluginDenylist` (FilePluginDenylist) and calls
  `PluginManager::set_denylist` before plugins load.
- `run_server_mode` builds + starts a `JobScheduler` (when a job service exists
  and `scheduler_enabled`), and passes the denylist into `CommandAgent` →
  `PluginAdminRoute`.
- New cron/scheduler sources live in the `jobs` lib; `PluginDenylist.hpp` in `core`.

## Tests

- `tests/CronTests.hpp` — parser validation + matching (steps, lists, ranges,
  weekday ranges, the dom/dow OR rule, Sunday 0/7, daily midnight).
- `tests/JobStoreTests.hpp::test_scheduler_fires` — enabled vs disabled vs
  not-due, per-minute de-dup, and that a fired run is recorded with trigger
  `schedule` (drives `evaluate(tm)` directly — no sleeping).
- `tests/PluginAdminTests.hpp::test_plugin_denylist` — add/remove/list + file
  persistence across instances.

Verified: Debug + Release build clean; unit tests pass in both; live checks
confirm the background scheduler fires a `* * * * *` job and that a disabled
plugin stays unloaded across a restart (and re-enabling restores it).

## Known limitation

Environment overrides via `merge_environment` map every `_` to `.`, so a leaf key
containing underscores (e.g. `jobs.scheduler_tick_seconds`) cannot be set through
`ORCHA_*` env vars — use the YAML config file for those. (Pre-existing behaviour.)
