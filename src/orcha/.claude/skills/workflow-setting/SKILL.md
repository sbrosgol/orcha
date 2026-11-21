---
name: workflow-setting
description: Add or change a workflow-level or step-level setting in Orcha definitions (like timeout_ms, rollback, circuit_breaker, parallel - or new ones such as retries, conditions, depends_on) end to end - parsing/validation, engine semantics, YAML round-trip, dashboard editor, API docs, README, tests. Use when a feature adds or changes a key in workflow/job definitions.
---

# Adding a definition setting

A setting touches every layer that reads, writes, runs or shows a definition.
Missing one layer silently drops or mis-runs the field. Work through all of
them, then get the `orchestration-reviewer` agent to check the semantics.

## 1. Specify before coding

Write down (and confirm with the user when it is a public behaviour change):
the key, level (workflow or step), type and allowed range, **default that keeps
every existing definition's behaviour**, the exact semantics including failure,
timeout, cancel, rollback and `parallel`/nested `run_job` cases, and what
appears in the run result. Use the `orchestration-reviewer` agent for a
proposal review when semantics are non-trivial (retries, branching, DAGs).

## 2. Parse and validate — `workflow/IWorkflowEngine.hpp`

- Add the field to `WorkflowDefinition`/`WorkflowStep` with its default.
- In `from_json`, check the JSON type explicitly (`is_boolean()`,
  `is_number_integer()`, `is_string()`) and range, and throw
  `std::invalid_argument` with a message naming the key and the expected value.
  A present field of the wrong type is an error, never ignored. Routes turn any
  exception from `from_json` into 400 `Invalid workflow: <what()>`, so the
  message is user-facing: don't rely on bare `get<int>()`, whose nlohmann
  `type_error` text is cryptic.
- If the setting references steps (names, indexes), validate the references at
  parse time, not mid-run.
- Jobs embed workflows: `JobRoute::validate` (`agent/routes/JobRoute.hpp`) runs
  the same `from_json` on save, so invalid settings are rejected when a job is
  saved, not when it fires. Definitions already stored stay as they were:
  a new required/strict rule must not make old stored jobs fail at run time
  without a migration or a clear run error.

## 3. Semantics — `workflow/WorkflowEngine.cpp` (+ `jobs/*` if lifecycle)

Implement on every path: sequential, `parallel` steps, nested `run_job`,
cancellation and deadline expiry, rollback. Keep per-run state in
`ExecutionContext`, honour `ExecutionControl`, never block without a deadline.
If the setting adds waiting (retries, delays), the wait must be interruptible by
cancellation and count against the run deadline. Record what happened in
`WorkflowStepResult::to_json` (additive fields only; clients ignore unknown
keys) and keep `success`/`error_message` meaning the same.

If `ICommand` must change (e.g. passing output to `rollback`), that is an ABI
bump (`kPluginAbiVersion`) and a user decision.

## 4. YAML

Nothing to add for plain scalars/objects: `utils/YamlJson.cpp` converts
generically. Add a YAML case to `tests/YamlTests.hpp` if the value could be
misread by the YAML 1.2 core schema (e.g. strings like `yes`, `1e3`, `null`,
durations) and check `json_to_yaml()` round-trips it.

## 5. Dashboard — `agent/dashboard_embedded.hpp` (load the `admin-ui` skill)

- Unknown fields already survive the visual editor (`editorDefExtra`,
  `step.extra`); verify with `scripts/editor-yaml.mjs`.
- If the setting gets a control, mirror it both ways like `rollback` and
  `circuit_breaker` (`setSettingsControls`, `onSettingChange`), keep it in sync
  with the JSON/YAML tabs, including while the JSON is invalid.
- Show its outcome in run detail if it changes the result (e.g. attempts).
- Review with the `admin-ui-reviewer` agent.

## 6. Docs and API description

- README: the relevant section ("Workflow runs", "Timeouts and Cancellation",
  "Rollback and Circuit Breaking" or a new one) with a JSON and a YAML example.
- OpenAPI: the workflow schema in `agent/routes/SwaggerRoute.hpp`.
- `sample_workflows/` example when it is a headline feature.
- CLAUDE.md "Per-workflow settings" contract line.
- If it has a server-wide default, it is also a config key: `orcha.yaml.example`,
  README, `config/ConfigValidator.hpp`, env override `ORCHA_<SECTION>_<KEY>`.

## 7. Tests

- Parsing: valid values, each invalid type/range → `std::invalid_argument`,
  absent → default (existing definitions unchanged).
- Semantics: one test per row of the edge-case table from step 1 (use
  `tests/ResilienceTests.hpp` / `TimeoutTests.hpp` patterns and the slow test
  plugin for timing). Concurrency-sensitive ⇒ `sanitizers` skill.
- End to end: a `server-smoke` scenario POSTing a definition that uses it, in
  JSON and YAML.
