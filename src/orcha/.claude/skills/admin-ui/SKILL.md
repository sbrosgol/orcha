---
name: admin-ui
description: Build, run, seed and visually verify the Orcha /admin dashboard (agent/dashboard_embedded.hpp). Use when changing the admin UI, asked to run/screenshot/check the dashboard, or to confirm a UI change works in a real browser in both themes.
---

# Orcha admin UI — change, run, verify

The whole dashboard is **one embedded HTML/CSS/JS file**: `agent/dashboard_embedded.hpp`
(`kDashboardHtml`, a C++ raw string `R"HTML( ... )HTML"`), served at `GET /admin` by
`agent/routes/DashboardRoute.hpp`. There is no build step for the front end; any edit
requires rebuilding the `orcha` binary.

## Rules for editing the dashboard

- **No external assets.** No CDN scripts, web fonts or remote images: the agent must work
  air-gapped. Icons are an inline SVG `<symbol>` sprite (`<use href="#i-name"/>`); charts are
  hand-rolled SVG (`renderRunsChart`); the flow chart is Canvas (`flow` module).
- **Never write the sequence `)HTML"`** inside the page. It terminates the raw string.
- **Colors come from tokens only** (`--bg --panel --line --fg --muted --faint --accent --ok
  --warn --err --info --violet --pink` …). Each token is defined three times: `:root` (light),
  `:root[data-theme="dark"]`, and `@media (prefers-color-scheme: dark) :root[data-theme="system"]`.
  Add any new token to **all three** blocks. Canvas code reads tokens through `cssVar('--x')`.
- **Reuse existing components** before inventing new ones: `.btn` (`primary`, `danger`,
  `danger-solid`, `ghost`, `sm`, `icon`, `.loading` via `busy(btn,on)`), `.badge <status>`
  (also breaker states `closed/open/half_open` and rollback `rb-done/rb-failed/rb-skipped/rb-na`),
  `.tag` chips (`.chips` row), `.card/.card-hd/.card-bd`, `.tbl`, `.empty` via `emptyState()`,
  `.alert` (`.err`, `.info`, `.mid` for an inline action), `.switch`, `.seg`,
  `toast(msg,'ok'|'err')`, `confirmDialog(title, body, okLabel, {tone:'primary'})` (default tone
  is destructive; use `primary` for recovery actions; never `window.confirm`), the `#modal`
  editor, and the `#drawer` side panel.
- **Every list has loading, empty and error states.** A failed request must not leave a
  skeleton forever: record the error in `state` (see `state.breakersErr`) and render an
  `.alert.err` with a Retry button; summaries elsewhere (overview, nav counts) must say the
  status is unavailable rather than look healthy.
- **Phone width (≤ 820px):** hide secondary table columns with `.hide-sm`; hide labels that
  screen readers still need with `.sr-sm` (e.g. icon-only buttons keep an `aria-label`);
  rows with badges must wrap (`flex-wrap`), never clip.
- **The job editor must never drop fields it does not edit.** `jsonToVisual()` keeps
  workflow-level extras in `editorDefExtra` and per-step extras in `step.extra`;
  `visualToJson()` writes them back. Workflow settings with controls (`rollback`,
  `circuit_breaker`) are mirrored both ways between the controls, the visual builder and
  the JSON tab (`setSettingsControls`, `onSettingChange`). New editable fields follow the
  same pattern.
- **Escape everything interpolated into HTML** with `esc()`. All API data is untrusted.
- Routing is hash-based (`#/overview`, `#/plugins`, `#/jobs/<id>`, `#/breakers`) and handled
  in `route()`. Data loads in `refreshAll()` (plugins, jobs, runs, circuit breakers; fail-soft
  per endpoint) and re-renders every view.
- The job editor has Visual / JSON / YAML tabs. `#jDef` (JSON text) is the common
  format; the YAML tab converts through `POST /api/definitions/convert` (never parse
  YAML in JS), caches the last conversion in `yamlSync`, and serialises async work
  with `setDefBusy()`. File import (button or drop on `#defField`) accepts a bare
  definition or a whole job and never saves by itself.
- Plugin uploads (`#upload` dialog, `renderStagedUploads()`): states are disabled
  (show the server's `reason`), pick files (sorted by extension into library /
  signature / manifest), and review → Install / Keep for later / Discard. The
  review must always say that installing runs the plugin's code in the server.
  `/api/plugins/_uploads` `keys` are `{id, plugins|null, override_bundled}`; staged
  uploads carry `allowed_by` (the key scope that permits the name). A downgrade
  review (`change` `downgrade`, or `unversioned`: an unversioned build over a
  versioned one; see `upNeedsConfirm`) shows `#upConfirm`; Install stays
  disabled until it is ticked and then sends `{allow_downgrade: true}`.
  `/api/plugins` entries carry `source` (`bundled`|`uploaded`) and, for an upload
  that overrides a bundled plugin, `overrides {version, library_path}`.
  `/api/plugins/_uploads` is fetched in `refreshAll()` (fail-soft).
- API contracts the page relies on: `/api/plugins`, `/api/plugins/_uploads`, `/api/jobs`, `/api/runs`,
  `/api/definitions/convert`, `/api/circuit-breakers` (+ `POST /{command}/reset`); run `result[]` step entries may carry
  `rollback: {status: rolled_back|failed|skipped|not_supported, message?}`.
- Auth: Basic token in `sessionStorage` (`orcha_auth`); `api()` handles 401 → login view.
  `/admin` itself is unauthenticated; only `/api/*` is gated. `showLogin()` asks
  `GET /api/setup` (no login) and shows `#setupForm` ("Create the admin account")
  instead of `#loginForm` when `setup_required`; `#login[data-mode]` is
  `checking|login|setup` (forms hidden while checking, so sign-in never flashes).
  Setup success stores the new Basic token and enters the app; 409 (claimed
  meanwhile) falls back to sign-in with a message. `#login` scrolls: keep auth
  cards reachable on short windows.
- Cron in the backend is evaluated in **UTC** (`jobs/CronExpr.hpp`). The JS preview
  (`cronNext`) must match in UTC and display results in local time.
- Keep keyboard and screen-reader support: focus-visible rings, `aria-*` on toggles, tabs and
  dialogs, Esc closes overlays, Tab is trapped in overlays.

## Design principles (orchestration console)

The operator's questions, in order: *Is anything broken or about to break?
What is running now? Why did this run fail, and where did the time go?* Layout
follows that order.

- **Action first.** Things needing attention (open breakers, failed rollbacks,
  recent failures, a full queue) come before KPIs and breakdowns, on desktop and
  phone. Healthy summaries never hide unavailable data.
- **Colour means status.** `--ok/--warn/--err` are reserved for run, step,
  breaker and rollback outcomes. Metadata badges (trigger, source, tags) use
  neutral tones, never red/pink next to a `failed` badge.
- **Every number states its window.** A KPI computed from a page of runs says
  "last N runs"; a time chart is fed by a server aggregate covering the whole
  period, not by the latest page. Polling must not re-download step output.
- **Show every status.** Counts and filters include `cancelled`, `interrupted`
  and `skipped`, not only success/failed/running/queued.
- **Runs are addressable.** Selecting a run updates the hash
  (`#/jobs/<id>/runs/<runId>`); a link reproduces the view.
- **History is rendered from the run, not the current job.** Step labels come
  from the stored `result[].step`/`name`, never `definition.steps[i]` of a job
  that may have been edited since.
- **No hints for behaviour that doesn't exist.** Copy describes what an
  interaction does (the flow canvas drag only moves the drawing).
- **Visualisations have a DOM equivalent.** Canvas/SVG charts and graphs get a
  text alternative (sr-only list/table) and keyboard-focusable nodes; anything
  clickable in them is also reachable without a mouse or by touch.
- **Rows that open something are keyboard targets.** A clickable `<tr>` puts a
  `<button class="linkish">` or `<a href>` in its primary cell; never rely on a
  row click handler alone.
- **Invalid is not "off".** A control mirroring a definition value that is
  present but invalid shows an inline invalid state, not its default.
- Empty output renders "(no output)" for both `undefined` and `null`.

## Run it

1. Build: `cmake --build cmake-build-debug --target orcha`
2. Make a scratch run dir (scratchpad, not the repo) with an `orcha.yaml` in **block style**:
   ```yaml
   server:
     host: "127.0.0.1"
     port: 8091
   plugins:
     directory: "<abs path>/cmake-build-debug/commands"
     denylist_path: "./denylist.txt"
   admin:
     enabled: true
     auth_required: true
     username: "admin"
     password: "change-me"
   jobs:
     db_path: "./jobs.db"
   ```
   `main` loads `./orcha.yaml` from the **current directory**. Start the binary from that dir
   in the background: `cd <run dir> && exec <repo>/cmake-build-debug/orcha > stdout.log 2>&1`.
   Check the log for `Admin dashboard` warnings (admin is fail-closed without a password).
3. Seed demo data: `ORCHA_URL=http://127.0.0.1:8091 bash .claude/skills/admin-ui/scripts/seed.sh`
   (creates 3 scheduled jobs, a failing `broken-step` job, `deploy-with-rollback` (fails
   after a completed step, rollback enabled) and `flaky-api` (circuit breaker enabled,
   always fails), runs each once and `flaky-api` five more times so its breaker opens).
   Breaker state is in memory: re-seed after restarting the agent.

Shell gotcha: zsh does not word-split `$VAR`, so `A='-u admin:pw'; curl $A` sends a bad
header. Pass `-u admin:change-me` literally or use a function.

## Verify visually

If the Claude-in-Chrome extension is connected, use it. Otherwise use the bundled headless
harness (needs Google Chrome and Node ≥ 22):

```bash
node .claude/skills/admin-ui/scripts/screenshots.mjs http://127.0.0.1:8091 <scratch>/shots           # desktop
node .claude/skills/admin-ui/scripts/screenshots.mjs http://127.0.0.1:8091 <scratch>/shots-m 390 844 # phone
```

It logs in through the real form, captures login, overview, plugins, plugin drawer, job
detail, editor, confirm dialog, the rolled-back `deploy-with-rollback` run, the breakers
page and the reset confirm in **light and dark**, and prints uncaught JS errors
(exit 1 if any). **Read the PNGs** and check them. A clean exit code only proves the page
didn't crash. Look for overlapping or clipped text, low contrast (especially dark mode),
broken alignment, and empty states.

Real data cannot produce every state (no bundled plugin implements rollback, so only
"no rollback support" appears). Use the states harness, which fakes the breakers and
run-history responses via Chrome's Fetch interception — all rollback statuses, all
breaker states, HTML in names, the breakers API failing, half-open-only overview, the
primary confirm, phone width, and an editor round-trip:

```bash
node .claude/skills/admin-ui/scripts/states.mjs http://127.0.0.1:8091 <scratch>/states
```

For behaviour (not just looks), the flow harnesses drive the real UI over CDP and check the
results through the API (each prints `ok`/`FAIL` per check; exit 1 on a failure or JS error):

```bash
# Job editor: Visual/JSON/YAML tabs, live YAML errors, settings sync, file import
# (picker + drop), tab keyboard support, remembered tab. Any agent with the echo plugin.
node .claude/skills/admin-ui/scripts/editor-yaml.mjs http://127.0.0.1:8091 <scratch>/editor [390 844]

# Plugin uploads: needs a prepared pair of servers (uploads on/off), a signing key and
# signed fixtures. OpenSSL 3 is required (OPENSSL=/opt/homebrew/bin/openssl on macOS).
bash .claude/skills/admin-ui/scripts/upload-env.sh start <scratch>/up 8094 8095
node .claude/skills/admin-ui/scripts/upload-flow.mjs http://127.0.0.1:8094 http://127.0.0.1:8095 \
     <scratch>/up/fixtures <scratch>/up/shots [390 844]
bash .claude/skills/admin-ui/scripts/upload-env.sh stop <scratch>/up
```

`upload-flow.mjs` changes server state: run `upload-env.sh start` again before re-running it.

First-run setup needs servers with **no** admin password and a fresh
`admin.credentials_file` (the run claims both; restart with new files to rerun):

```bash
# two such servers, e.g. ORCHA_SERVER_PORT=8096 / 8097, each from its own empty run dir
node .claude/skills/admin-ui/scripts/setup-flow.mjs http://127.0.0.1:8096 http://127.0.0.1:8097 <scratch>/setup [390 844]
```

Run harnesses from bash (or pass width and height as separate words): zsh does
not split `$size`, and a single "390 844" argument silently runs at a NaN width.
For a new flow, build on `scripts/cdp.mjs` (browser, `check`, `until`, `mouse`, `setFiles`,
`login`, `finish`). Pitfalls it documents:

- `el.click()` from `Runtime.evaluate` does **not** focus the element (a real click does in
  Chrome, not in Safari). App code must not assume the clicked button is focused; tests that
  care use `mouse()`.
- The app's `state`/`up` objects are not reachable from `evaluate`: assert on the DOM,
  `location.hash` and the API.
- Never sleep a fixed time for a request: `until()` the UI settles (a dialog closes, a
  button leaves `.loading`). Installs and slow CI make fixed sleeps flaky.

Finish with a JS syntax check if you only touched script code:
```bash
python3 -c "import re;s=open('agent/dashboard_embedded.hpp').read();open('<scratch>/app.js','w').write(re.findall(r'<script>(.*?)</script>',s,re.S)[-1])" && node --check <scratch>/app.js
```

Stop the agent afterwards by the PID you started (`kill <pid>`). Do not `pkill -f
cmake-build-debug/orcha`: other sessions or reviewer agents may be running their own.
