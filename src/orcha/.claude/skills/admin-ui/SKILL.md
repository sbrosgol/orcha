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
- API contracts the page relies on: `/api/plugins`, `/api/jobs`, `/api/runs`,
  `/api/circuit-breakers` (+ `POST /{command}/reset`); run `result[]` step entries may carry
  `rollback: {status: rolled_back|failed|skipped|not_supported, message?}`.
- Auth: Basic token in `sessionStorage` (`orcha_auth`); `api()` handles 401 → login view.
  `/admin` itself is unauthenticated; only `/api/*` is gated.
- Cron in the backend is evaluated in **UTC** (`jobs/CronExpr.hpp`). The JS preview
  (`cronNext`) must match in UTC and display results in local time.
- Keep keyboard and screen-reader support: focus-visible rings, `aria-*` on toggles, tabs and
  dialogs, Esc closes overlays, Tab is trapped in overlays.

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

For behaviour (not just looks), drive the real UI over CDP in a scratch script modelled on
these harnesses: set control values and dispatch `change`/`input`, click Save, then read the
result back through the API with `fetch` and assert on it.

Finish with a JS syntax check if you only touched script code:
```bash
python3 -c "import re;s=open('agent/dashboard_embedded.hpp').read();open('<scratch>/app.js','w').write(re.findall(r'<script>(.*?)</script>',s,re.S)[-1])" && node --check <scratch>/app.js
```

Stop the agent afterwards (`pkill -f cmake-build-debug/orcha`).
