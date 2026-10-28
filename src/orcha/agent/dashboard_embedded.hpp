#pragma once

namespace Orcha::Agent {

// Embedded admin dashboard served at /admin. Vanilla HTML/JS, no build step and
// no external assets (works air-gapped; charts are hand-rolled SVG). Talks to
// the /api/plugins and /api/jobs JSON APIs.
//
// Features: custom login view, Light/Dark/System themes, hash routing
// (#/overview, #/plugins, #/jobs/<id>), Overview KPIs + charts, Plugins admin
// with search/filter + detail drawer, Jobs (CRUD + run + history + cron
// preview), and an interactive HTML Canvas workflow flow chart.
inline constexpr const char kDashboardHtml[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <meta name="color-scheme" content="light dark" />
  <title>Orcha Admin</title>
  <link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='8' fill='%230f766e'/%3E%3Ccircle cx='16' cy='16' r='7' fill='none' stroke='white' stroke-width='3'/%3E%3C/svg%3E" />
  <script>try{document.documentElement.dataset.theme=localStorage.getItem('orcha_theme')||'system';}catch(e){document.documentElement.dataset.theme='system';}</script>
  <style>
    /* ---------- Design tokens ----------
       Neutral slate surfaces, one teal accent, semantic status colors.
       Status colors are used for text/dots only; tinted backgrounds are
       derived with color-mix so both themes stay legible. */
    :root {
      --bg:#f7f8fa; --bg-sunken:#f1f3f6; --panel:#ffffff; --panel-2:#f8fafc; --hover:#f1f5f9;
      --line:#e3e7ed; --line-strong:#cfd6df;
      --fg:#0f172a; --fg-2:#334155; --muted:#64748b; --faint:#94a3b8;
      --accent:#0f766e; --accent-hover:#115e59; --accent-soft:#ccfbf1; --on-accent:#ffffff;
      --ok:#15803d; --warn:#b45309; --err:#b91c1c; --info:#0369a1; --violet:#6d28d9; --pink:#be185d;
      --ring:color-mix(in srgb,var(--accent) 45%,transparent);
      --shadow-sm:0 1px 2px rgba(15,23,42,.06);
      --shadow:0 1px 3px rgba(15,23,42,.08), 0 8px 24px rgba(15,23,42,.06);
      --shadow-lg:0 12px 40px rgba(15,23,42,.18);
      --scrim:rgba(15,23,42,.42);
      --radius:10px; --radius-sm:6px;
      --font:ui-sans-serif,-apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,"Helvetica Neue",Arial,sans-serif;
      --mono:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace;
    }
    :root[data-theme="dark"] {
      --bg:#0a0d12; --bg-sunken:#07090d; --panel:#11151c; --panel-2:#151a23; --hover:#1a2029;
      --line:#222a35; --line-strong:#2f3946;
      --fg:#e7ecf3; --fg-2:#c4cdd9; --muted:#8b97a8; --faint:#5d6878;
      --accent:#2dd4bf; --accent-hover:#5eead4; --accent-soft:#0f2e2b; --on-accent:#04201d;
      --ok:#4ade80; --warn:#fbbf24; --err:#f87171; --info:#38bdf8; --violet:#a78bfa; --pink:#f472b6;
      --shadow-sm:0 1px 2px rgba(0,0,0,.4);
      --shadow:0 1px 3px rgba(0,0,0,.45), 0 8px 24px rgba(0,0,0,.35);
      --shadow-lg:0 16px 48px rgba(0,0,0,.6);
      --scrim:rgba(0,0,0,.6);
    }
    @media (prefers-color-scheme: dark) {
      :root[data-theme="system"] {
        --bg:#0a0d12; --bg-sunken:#07090d; --panel:#11151c; --panel-2:#151a23; --hover:#1a2029;
        --line:#222a35; --line-strong:#2f3946;
        --fg:#e7ecf3; --fg-2:#c4cdd9; --muted:#8b97a8; --faint:#5d6878;
        --accent:#2dd4bf; --accent-hover:#5eead4; --accent-soft:#0f2e2b; --on-accent:#04201d;
        --ok:#4ade80; --warn:#fbbf24; --err:#f87171; --info:#38bdf8; --violet:#a78bfa; --pink:#f472b6;
        --shadow-sm:0 1px 2px rgba(0,0,0,.4);
        --shadow:0 1px 3px rgba(0,0,0,.45), 0 8px 24px rgba(0,0,0,.35);
        --shadow-lg:0 16px 48px rgba(0,0,0,.6);
        --scrim:rgba(0,0,0,.6);
      }
    }

    /* ---------- Base ---------- */
    * { box-sizing:border-box; }
    html, body { height:100%; }
    body { margin:0; font:14px/1.5 var(--font); background:var(--bg); color:var(--fg);
           -webkit-font-smoothing:antialiased; font-feature-settings:"cv11","ss01"; }
    a { color:var(--accent); text-decoration:none; } a:hover { text-decoration:underline; }
    code, .mono { font-family:var(--mono); font-size:12px; }
    code { background:var(--bg-sunken); border:1px solid var(--line); border-radius:4px; padding:1px 5px; }
    .num { font-variant-numeric:tabular-nums; }
    .muted { color:var(--muted); } .faint { color:var(--faint); }
    .sr-only { position:absolute; width:1px; height:1px; overflow:hidden; clip:rect(0 0 0 0); white-space:nowrap; }
    :focus-visible { outline:2px solid var(--accent); outline-offset:2px; }
    svg.i { width:16px; height:16px; flex:none; display:block; }
    ::selection { background:color-mix(in srgb,var(--accent) 30%,transparent); }

    /* ---------- Buttons ---------- */
    .btn { --b:var(--line-strong); display:inline-flex; align-items:center; justify-content:center; gap:6px;
           height:32px; padding:0 12px; border-radius:var(--radius-sm); border:1px solid var(--b);
           background:var(--panel); color:var(--fg); font:500 13px/1 var(--font); cursor:pointer;
           white-space:nowrap; box-shadow:var(--shadow-sm); transition:background .12s, border-color .12s, color .12s; }
    .btn:hover { background:var(--hover); }
    .btn:disabled { opacity:.55; cursor:not-allowed; }
    .btn.primary { background:var(--accent); border-color:var(--accent); color:var(--on-accent); }
    .btn.primary:hover:not(:disabled) { background:var(--accent-hover); border-color:var(--accent-hover); }
    .btn.danger { color:var(--err); }
    .btn.danger:hover:not(:disabled) { border-color:var(--err); background:color-mix(in srgb,var(--err) 8%,var(--panel)); }
    .btn.danger-solid { background:#dc2626; border-color:#dc2626; color:#fff; } /* fixed red: white text stays AA in both themes */
    .btn.danger-solid:hover { background:#b91c1c; border-color:#b91c1c; }
    .btn.ghost { background:transparent; border-color:transparent; box-shadow:none; color:var(--muted); }
    .btn.ghost:hover { background:var(--hover); color:var(--fg); }
    .btn.sm { height:28px; padding:0 9px; font-size:12px; }
    .btn.icon { width:32px; padding:0; } .btn.sm.icon { width:28px; }
    .btn .spin { display:none; }
    .btn.loading .spin { display:block; } .btn.loading .ico { display:none; }
    @keyframes spin { to { transform:rotate(360deg); } }
    .spin, .spinning svg { animation:spin .8s linear infinite; }

    /* ---------- Form controls ---------- */
    .input, select.input, textarea.input {
      width:100%; height:34px; padding:0 10px; border-radius:var(--radius-sm); border:1px solid var(--line-strong);
      background:var(--panel); color:var(--fg); font:13px var(--font); box-shadow:var(--shadow-sm); }
    textarea.input { height:auto; min-height:220px; padding:10px; font:12.5px/1.55 var(--mono); resize:vertical; }
    .input::placeholder { color:var(--faint); }
    .input:focus { outline:none; border-color:var(--accent); box-shadow:0 0 0 3px var(--ring); }
    .input.invalid { border-color:var(--err); box-shadow:0 0 0 3px color-mix(in srgb,var(--err) 25%,transparent); }
    .field { margin-bottom:16px; }
    .field > label, .field .label { display:block; font-size:12.5px; font-weight:600; color:var(--fg-2); margin-bottom:6px; }
    .field .hint { font-size:12px; color:var(--muted); margin-top:6px; }
    .field .hint.err { color:var(--err); }
    .search { position:relative; }
    .search svg { position:absolute; left:9px; top:50%; transform:translateY(-50%); color:var(--faint); pointer-events:none; }
    .search .input { padding-left:32px; }
    input[type=checkbox] { accent-color:var(--accent); width:15px; height:15px; }

    /* Toggle switch */
    .switch { display:inline-flex; align-items:center; gap:10px; cursor:pointer; user-select:none; font-size:13px; color:var(--fg-2); }
    .field > label.switch { display:inline-flex; font-weight:500; margin-bottom:0; }
    .switch input { position:absolute; opacity:0; pointer-events:none; }
    .switch .track { width:34px; height:20px; border-radius:999px; background:var(--line-strong); position:relative; transition:background .15s; flex:none; }
    .switch .track::after { content:""; position:absolute; top:2px; left:2px; width:16px; height:16px; border-radius:50%;
                            background:#fff; box-shadow:0 1px 2px rgba(0,0,0,.3); transition:transform .15s; }
    .switch input:checked + .track { background:var(--accent); }
    .switch input:checked + .track::after { transform:translateX(14px); }
    .switch input:focus-visible + .track { outline:2px solid var(--accent); outline-offset:2px; }
    .switch input:disabled + .track { opacity:.5; }

    /* Segmented control */
    .seg { display:inline-flex; padding:2px; gap:2px; border-radius:8px; background:var(--bg-sunken); border:1px solid var(--line); }
    .seg button { border:none; background:none; color:var(--muted); font:500 12.5px var(--font); height:26px; padding:0 10px;
                  border-radius:6px; cursor:pointer; display:inline-flex; align-items:center; gap:6px; }
    .seg button:hover { color:var(--fg); }
    .seg button.active { background:var(--panel); color:var(--fg); box-shadow:var(--shadow-sm); }
    .seg .ct { font-size:11px; color:var(--faint); font-variant-numeric:tabular-nums; }

    /* ---------- Badges & status ---------- */
    .badge { --c:var(--muted); display:inline-flex; align-items:center; gap:6px; height:22px; padding:0 8px;
             border-radius:999px; font:500 12px/1 var(--font); white-space:nowrap;
             color:var(--c); background:color-mix(in srgb,var(--c) 11%,transparent);
             border:1px solid color-mix(in srgb,var(--c) 22%,transparent); }
    .badge::before { content:""; width:6px; height:6px; border-radius:50%; background:var(--c); }
    .badge.plain::before { display:none; }
    .badge.loaded, .badge.success, .badge.enabled { --c:var(--ok); }
    .badge.available, .badge.api { --c:var(--info); }
    .badge.failed, .badge.error { --c:var(--err); }
    .badge.running, .badge.queued { --c:var(--info); }
    .badge.running::before { animation:pulse 1.2s ease-in-out infinite; }
    @keyframes pulse { 50% { opacity:.3; } }
    .badge.cancelled, .badge.skipped { --c:var(--muted); }
    .badge.interrupted { --c:var(--warn); }
    .badge.schedule, .badge.cron { --c:var(--violet); }
    .badge.manual { --c:var(--pink); }
    .badge.disabled, .badge.unknown { --c:var(--muted); }
    /* Circuit breaker states and per-step rollback outcomes */
    .badge.closed { --c:var(--ok); }
    .badge.open { --c:var(--err); }
    .badge.half_open { --c:var(--warn); }
    .badge.rb-done { --c:var(--violet); }
    .badge.rb-failed { --c:var(--err); }
    .badge.rb-skipped, .badge.rb-na { --c:var(--muted); }
    .dot { width:8px; height:8px; border-radius:50%; background:var(--faint); flex:none; display:inline-block; }
    .dot.ok { background:var(--ok); } .dot.err { background:var(--err); }
    .dot.live { background:var(--info); } .dot.warn { background:var(--warn); } .dot.muted { background:var(--faint); } .dot.off { background:transparent; border:1.5px solid var(--faint); }
    .tag { display:inline-block; font-size:11.5px; color:var(--fg-2); background:var(--bg-sunken); border:1px solid var(--line);
           border-radius:4px; padding:0 6px; line-height:18px; margin:0 4px 4px 0; }

    /* ---------- App shell ---------- */
    .shell { display:grid; grid-template-columns:232px 1fr; min-height:100vh; }
    .sidebar { position:sticky; top:0; height:100vh; display:flex; flex-direction:column; gap:4px;
               border-right:1px solid var(--line); background:var(--panel); padding:14px 12px; }
    .brand { display:flex; align-items:center; gap:10px; padding:4px 8px 16px; }
    .logo { width:30px; height:30px; border-radius:8px; background:linear-gradient(135deg,#14b8a6,#0f766e);
            display:flex; align-items:center; justify-content:center; color:#fff; flex:none; box-shadow:var(--shadow-sm); }
    .logo svg { width:16px; height:16px; }
    .brand .nm { font-weight:650; font-size:15px; letter-spacing:-.01em; line-height:1.1; }
    .brand .sub { font-size:11.5px; color:var(--muted); }
    .navlbl { font-size:11px; font-weight:600; color:var(--faint); text-transform:uppercase; letter-spacing:.06em; padding:10px 10px 4px; }
    .nav a { display:flex; align-items:center; gap:10px; height:34px; padding:0 10px; border-radius:7px;
             color:var(--fg-2); font-weight:500; text-decoration:none; }
    .nav a:hover { background:var(--hover); color:var(--fg); }
    .nav a.active { background:var(--accent-soft); color:var(--accent); }
    :root[data-theme="dark"] .nav a.active { color:var(--accent-hover); }
    .nav a .ct { margin-left:auto; font-size:11.5px; color:var(--muted); font-variant-numeric:tabular-nums; }
    .nav a .ct.hot { color:var(--err); font-weight:650; }
    .sidebar .foot { margin-top:auto; display:flex; flex-direction:column; gap:10px; padding-top:12px; border-top:1px solid var(--line); }
    .user { display:flex; align-items:center; gap:10px; padding:4px 4px 0; }
    .avatar { width:28px; height:28px; border-radius:50%; background:var(--accent-soft); color:var(--accent);
              font-weight:600; font-size:12px; display:flex; align-items:center; justify-content:center; flex:none; text-transform:uppercase; }
    .user .who { flex:1; min-width:0; } .user .who div { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .user .who .r { font-size:11.5px; color:var(--muted); }

    .theme-switch { display:flex; padding:2px; gap:2px; border-radius:8px; background:var(--bg-sunken); border:1px solid var(--line); }
    .theme-switch button { flex:1; border:none; background:none; color:var(--muted); height:26px; border-radius:6px; cursor:pointer;
                           display:inline-flex; align-items:center; justify-content:center; }
    .theme-switch button:hover { color:var(--fg); }
    .theme-switch button.active { background:var(--panel); color:var(--fg); box-shadow:var(--shadow-sm); }
    .theme-switch svg { width:15px; height:15px; }

    .main { min-width:0; display:flex; flex-direction:column; }
    .topbar { position:sticky; top:0; z-index:20; display:flex; align-items:center; gap:12px; min-height:60px;
              padding:12px 32px; background:color-mix(in srgb,var(--bg) 85%,transparent);
              backdrop-filter:saturate(1.4) blur(8px); border-bottom:1px solid var(--line); }
    .crumbs { display:flex; align-items:center; gap:6px; font-size:13px; color:var(--muted); min-width:0; }
    .crumbs a { color:var(--muted); } .crumbs a:hover { color:var(--fg); text-decoration:none; }
    .crumbs .cur { color:var(--fg); font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .topbar .sp { flex:1; }
    .updated { font-size:12px; color:var(--faint); white-space:nowrap; }
    .menu-btn { display:none; }
    .content { padding:28px 32px 48px; max-width:1320px; width:100%; }
    .page-hd { display:flex; align-items:flex-end; gap:16px; margin-bottom:22px; flex-wrap:wrap; }
    .page-hd h1 { margin:0; font-size:22px; font-weight:650; letter-spacing:-.015em; }
    .page-hd p { margin:4px 0 0; color:var(--muted); }
    .page-hd .sp { flex:1; }
    .view { display:none; } .view.active { display:block; animation:fade .18s ease-out; }
    @keyframes fade { from { opacity:0; transform:translateY(3px); } to { opacity:1; transform:none; } }

    /* ---------- Cards ---------- */
    .card { background:var(--panel); border:1px solid var(--line); border-radius:var(--radius); box-shadow:var(--shadow-sm); min-width:0; }
    .card-hd { display:flex; align-items:center; gap:10px; padding:14px 18px; border-bottom:1px solid var(--line); min-height:52px; }
    .card-hd h2 { margin:0; font-size:14px; font-weight:600; }
    .card-hd .sub { font-size:12.5px; color:var(--muted); }
    .card-hd .sp { flex:1; }
    .card-bd { padding:18px; }
    .card-bd.flush { padding:0; }

    .grid { display:grid; gap:16px; }
    .g-kpi { grid-template-columns:repeat(4,minmax(0,1fr)); margin-bottom:16px; }
    .g-2-1 { grid-template-columns:minmax(0,2fr) minmax(0,1fr); margin-bottom:16px; }
    .kpi { padding:16px 18px; }
    .kpi .lbl { display:flex; align-items:center; gap:8px; color:var(--muted); font-size:12.5px; font-weight:500; }
    .kpi .lbl svg { color:var(--faint); }
    .kpi .val { font-size:28px; font-weight:650; letter-spacing:-.02em; line-height:1.15; margin-top:8px; font-variant-numeric:tabular-nums; }
    .kpi .val small { font-size:15px; font-weight:500; color:var(--muted); margin-left:2px; }
    .kpi .meta { font-size:12.5px; color:var(--muted); margin-top:4px; }
    .meter { height:6px; border-radius:999px; background:var(--bg-sunken); overflow:hidden; margin-top:10px; display:flex; }
    .meter span { display:block; height:100%; }

    /* Legend + breakdown rows */
    .legend { display:flex; gap:14px; font-size:12px; color:var(--muted); }
    .legend i { display:inline-block; width:10px; height:10px; border-radius:3px; margin-right:6px; vertical-align:-1px; }
    .brk { display:flex; flex-direction:column; gap:14px; }
    .brk-row .top { display:flex; align-items:center; gap:8px; font-size:13px; margin-bottom:6px; }
    .brk-row .top .v { margin-left:auto; font-variant-numeric:tabular-nums; color:var(--fg-2); }
    .brk-row .top .pct { width:42px; text-align:right; color:var(--faint); font-size:12px; font-variant-numeric:tabular-nums; }
    .brk-row .bar { height:6px; border-radius:999px; background:var(--bg-sunken); overflow:hidden; }
    .brk-row .bar span { display:block; height:100%; border-radius:999px; }
    .divider { height:1px; background:var(--line); margin:18px 0; }
    .sec-lbl { font-size:11.5px; font-weight:600; text-transform:uppercase; letter-spacing:.06em; color:var(--faint); margin-bottom:12px; }

    /* SVG chart */
    .chart { position:relative; height:240px; }
    .chart svg { width:100%; height:100%; display:block; overflow:visible; }
    .chart .gl { stroke:var(--line); stroke-width:1; }
    .chart .ax { fill:var(--faint); font:11px var(--font); }
    .chart .hit { fill:transparent; cursor:default; }
    .chart .hit:hover { fill:color-mix(in srgb,var(--fg) 5%,transparent); }
    .ctip { position:absolute; pointer-events:none; z-index:5; background:var(--panel); border:1px solid var(--line);
            border-radius:8px; box-shadow:var(--shadow); padding:8px 10px; font-size:12px; min-width:130px; display:none; }
    .ctip .t { font-weight:600; margin-bottom:4px; }
    .ctip .r { display:flex; align-items:center; gap:6px; color:var(--fg-2); }
    .ctip .r b { margin-left:auto; font-weight:600; font-variant-numeric:tabular-nums; }

    /* ---------- Tables ---------- */
    .table-wrap { overflow-x:auto; }
    table.tbl { width:100%; border-collapse:separate; border-spacing:0; font-size:13px; }
    .tbl th { position:sticky; top:0; text-align:left; font-size:11.5px; font-weight:600; text-transform:uppercase; letter-spacing:.05em;
              color:var(--muted); background:var(--panel-2); padding:9px 16px; border-bottom:1px solid var(--line); white-space:nowrap; }
    .tbl td { padding:11px 16px; border-bottom:1px solid var(--line); vertical-align:middle; }
    .tbl tr:last-child td { border-bottom:none; }
    .tbl tbody tr { transition:background .1s; }
    .tbl tbody tr.click { cursor:pointer; }
    .tbl tbody tr.click:hover, .tbl tbody tr.sel { background:var(--hover); }
    .tbl tbody tr.sel td:first-child { box-shadow:inset 3px 0 0 var(--accent); }
    .tbl .r { text-align:right; }
    .tbl .nowrap { white-space:nowrap; }
    .tbl .pname { font-weight:600; } .tbl .pname.wrap { overflow-wrap:anywhere; } .tbl .pdesc { color:var(--muted); font-size:12.5px; margin-top:1px; }
    .tbl .trunc { max-width:340px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .tbl .acts { display:flex; gap:6px; justify-content:flex-end; }

    /* Empty / skeleton / error states */
    .empty { text-align:center; padding:44px 24px; color:var(--muted); }
    .empty .ic { width:44px; height:44px; border-radius:12px; margin:0 auto 12px; display:flex; align-items:center; justify-content:center;
                 background:var(--bg-sunken); border:1px solid var(--line); color:var(--faint); }
    .empty .ic svg { width:20px; height:20px; }
    .empty h3 { margin:0 0 4px; font-size:14px; color:var(--fg); font-weight:600; }
    .empty p { margin:0 auto 14px; max-width:360px; font-size:13px; }
    .skel { display:block; height:12px; border-radius:4px; background:linear-gradient(90deg,var(--bg-sunken),var(--hover),var(--bg-sunken));
            background-size:200% 100%; animation:shimmer 1.2s infinite; }
    @keyframes shimmer { from { background-position:200% 0; } to { background-position:-200% 0; } }
    .alert { display:flex; gap:10px; align-items:flex-start; padding:10px 12px; border-radius:8px; font-size:13px; margin-bottom:12px;
             --c:var(--warn); color:var(--fg-2); background:color-mix(in srgb,var(--c) 9%,var(--panel));
             border:1px solid color-mix(in srgb,var(--c) 30%,transparent); }
    .alert svg { color:var(--c); margin-top:1px; }
    .alert.err { --c:var(--err); } .alert.info { --c:var(--info); }
    .alert a { color:inherit; font-weight:600; }
    .alert.mid { align-items:center; margin-bottom:0; } .alert.mid > span { flex:1; } .alert.mid svg { margin-top:0; }
    .chips { display:flex; gap:6px; flex-wrap:wrap; margin-top:8px; }
    .chips .tag, .step-card .shd .tag { display:inline-flex; align-items:center; gap:4px; white-space:nowrap; }
    .tag svg.i { width:12px; height:12px; flex:none; }

    /* ---------- Toolbar ---------- */
    .toolbar { display:flex; align-items:center; gap:10px; padding:12px 16px; border-bottom:1px solid var(--line); flex-wrap:wrap; }
    .toolbar .search { width:280px; max-width:100%; }
    .toolbar .sp { flex:1; }

    /* ---------- Jobs ---------- */
    .jobs-layout { display:grid; grid-template-columns:320px minmax(0,1fr); gap:16px; align-items:start; }
    .joblist { position:sticky; top:76px; max-height:calc(100vh - 100px); display:flex; flex-direction:column; }
    .joblist .toolbar .search { width:100%; }
    .joblist .items { overflow:auto; flex:1; padding:6px; }
    .jitem { display:block; width:100%; text-align:left; border:1px solid transparent; background:none; color:inherit; font:inherit;
             padding:10px 12px; border-radius:8px; cursor:pointer; }
    .jitem:hover { background:var(--hover); }
    .jitem.active { background:var(--accent-soft); border-color:color-mix(in srgb,var(--accent) 25%,transparent); }
    .jitem .l1 { display:flex; align-items:center; gap:8px; }
    .jitem .nm { font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; min-width:0; }
    .jitem .l2 { display:flex; align-items:center; gap:8px; margin-top:4px; padding-left:16px; font-size:12px; color:var(--muted); }
    .jitem .l2 .desc { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; min-width:0; }
    .jitem .cron { font-family:var(--mono); font-size:11px; color:var(--violet); white-space:nowrap; }

    .detail-hd { padding:18px 20px; border-bottom:1px solid var(--line); }
    .detail-hd .row { display:flex; align-items:flex-start; gap:12px; flex-wrap:wrap; }
    .detail-hd h2 { margin:0; font-size:18px; font-weight:650; letter-spacing:-.01em; display:flex; align-items:center; gap:10px; flex-wrap:wrap; }
    .detail-hd p { margin:4px 0 0; color:var(--muted); }
    .detail-hd .acts { margin-left:auto; display:flex; gap:8px; flex-wrap:wrap; }
    .facts { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); border-bottom:1px solid var(--line); }
    .facts > div { padding:12px 20px; border-right:1px solid var(--line); min-width:0; }
    .facts > div:last-child { border-right:none; }
    .facts .k { font-size:11.5px; color:var(--muted); font-weight:500; margin-bottom:3px; }
    .facts .v { font-size:13px; font-weight:500; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .section { padding:18px 20px; border-bottom:1px solid var(--line); }
    .section:last-child { border-bottom:none; }
    .section-hd { display:flex; align-items:center; gap:10px; margin-bottom:12px; }
    .section-hd h3 { margin:0; font-size:13px; font-weight:600; }
    .section-hd .sp { flex:1; }

    /* Dropdown menu */
    .dd { position:relative; }
    .dd-menu { position:absolute; right:0; top:calc(100% + 4px); z-index:30; min-width:180px; padding:4px; display:none;
               background:var(--panel); border:1px solid var(--line); border-radius:8px; box-shadow:var(--shadow); }
    .dd.open .dd-menu { display:block; }
    .dd-menu button { display:flex; align-items:center; gap:8px; width:100%; border:none; background:none; color:var(--fg); text-align:left;
                      font:13px var(--font); padding:7px 10px; border-radius:6px; cursor:pointer; }
    .dd-menu button:hover { background:var(--hover); }
    .dd-menu button.danger { color:var(--err); }
    .dd-menu hr { border:none; border-top:1px solid var(--line); margin:4px 0; }

    /* Flow chart */
    .chartwrap { position:relative; overflow:auto; border:1px solid var(--line); border-radius:8px;
                 background:var(--bg-sunken) radial-gradient(circle, var(--line) 1px, transparent 1px) 0 0/16px 16px; }
    #flow { display:block; }
    #fctip { position:absolute; pointer-events:none; display:none; max-width:300px; background:var(--panel); border:1px solid var(--line);
             border-radius:8px; padding:8px 10px; font:11.5px/1.45 var(--mono); box-shadow:var(--shadow); z-index:5; white-space:pre-wrap; }

    /* Step output cards */
    .outcard { border:1px solid var(--line); border-radius:8px; margin-bottom:10px; overflow:hidden; background:var(--panel); }
    .outcard summary { display:flex; align-items:center; flex-wrap:wrap; gap:6px 10px; padding:9px 12px; background:var(--panel-2); cursor:pointer; list-style:none; font-size:13px; }
    .outcard summary strong { min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
    .outcard summary .sbadges { display:inline-flex; flex-wrap:wrap; gap:6px; margin-left:auto; }
    .outcard summary::-webkit-details-marker { display:none; }
    .outcard summary .chev { color:var(--faint); transition:transform .15s; }
    .outcard[open] summary .chev { transform:rotate(90deg); }
    .outcard summary .idx { font:600 11px var(--mono); color:var(--muted); }
    .outcard summary .sp { flex:1; }
    .outcard[open] summary { border-bottom:1px solid var(--line); }
    .outcard .rb { display:flex; gap:8px; align-items:flex-start; padding:9px 14px; font-size:12.5px; color:var(--fg-2);
                   border-bottom:1px solid var(--line); }
    .outcard .rb svg.i { color:var(--violet); margin-top:1px; }
    .outcard .rb.err, .outcard .rb.err svg.i, .runhd .rbsum .bad { color:var(--err); }
    .runhd .rbsum { display:inline-flex; align-items:center; gap:6px; }
    .runhd .rbsum svg.i { color:var(--violet); }
    .outcard pre { margin:0; padding:12px 14px; max-height:320px; overflow:auto; font:12px/1.55 var(--mono); white-space:pre-wrap; word-break:break-word; }
    .outcard .err { color:var(--err); padding:10px 14px; font-size:13px; border-bottom:1px solid var(--line);
                    background:color-mix(in srgb,var(--err) 6%,transparent); }
    .outcard.flash { box-shadow:0 0 0 2px var(--accent); transition:box-shadow .2s; }
    .runhd { display:flex; align-items:center; gap:10px; flex-wrap:wrap; font-size:12.5px; color:var(--muted); margin-bottom:12px; }

    /* ---------- Login ---------- */
    #login { position:fixed; inset:0; z-index:50; display:flex; align-items:center; justify-content:center; padding:20px;
             background:radial-gradient(1200px 600px at 50% -10%, color-mix(in srgb,var(--accent) 12%,transparent), transparent), var(--bg); }
    .login-card { width:380px; max-width:100%; padding:32px; }
    .login-card .logo { width:40px; height:40px; border-radius:10px; margin-bottom:20px; }
    .login-card .logo svg { width:20px; height:20px; }
    .login-card h1 { margin:0; font-size:20px; font-weight:650; letter-spacing:-.015em; }
    .login-card .sub { margin:4px 0 24px; color:var(--muted); }
    .login-card .btn { width:100%; height:38px; font-size:14px; }
    .pw { position:relative; } .pw .input { padding-right:40px; }
    .pw button { position:absolute; right:4px; top:50%; transform:translateY(-50%); }
    .login-foot { display:flex; align-items:center; justify-content:space-between; margin-top:24px; padding-top:18px; border-top:1px solid var(--line); font-size:12px; color:var(--faint); }
    .login-foot .theme-switch { width:110px; }
    body:not(.authed) .shell { display:none; }
    body.authed #login { display:none; }
    body.booting #login, body.booting .shell { visibility:hidden; }

    /* ---------- Overlays: modal / drawer / confirm / toast ---------- */
    .overlay { position:fixed; inset:0; z-index:70; background:var(--scrim); display:none; align-items:center; justify-content:center; padding:20px; }
    .overlay.show { display:flex; animation:fadein .15s ease-out; }
    @keyframes fadein { from { opacity:0; } to { opacity:1; } }
    .dialog { background:var(--panel); border:1px solid var(--line); border-radius:12px; box-shadow:var(--shadow-lg);
              width:720px; max-width:100%; max-height:calc(100vh - 40px); display:flex; flex-direction:column; animation:pop .16s ease-out; }
    .dialog.sm { width:420px; }
    .dialog.md { width:600px; }
    @keyframes pop { from { transform:translateY(6px) scale(.985); opacity:.6; } to { transform:none; opacity:1; } }
    .dialog .hd { display:flex; align-items:center; gap:10px; padding:16px 20px; border-bottom:1px solid var(--line); }
    .dialog .hd h2 { margin:0; font-size:16px; font-weight:650; }
    .dialog .hd .sp { flex:1; }
    .dialog .bd { padding:20px; overflow:auto; }
    .dialog .ft { display:flex; align-items:center; gap:8px; padding:14px 20px; border-top:1px solid var(--line); background:var(--panel-2); border-radius:0 0 12px 12px; }
    .dialog .ft .sp { flex:1; }
    .dialog .ft .kbd-hint { font-size:12px; color:var(--faint); }
    kbd { font:11px var(--mono); border:1px solid var(--line-strong); border-bottom-width:2px; border-radius:4px; padding:0 4px; background:var(--panel); color:var(--muted); }
    .confirm-bd { display:flex; gap:14px; }
    .confirm-bd .ic { width:38px; height:38px; border-radius:50%; flex:none; display:flex; align-items:center; justify-content:center;
                      color:var(--err); background:color-mix(in srgb,var(--err) 12%,transparent); }
    #confirm.tone-primary .confirm-bd .ic { color:var(--accent); background:var(--accent-soft); }
    .confirm-bd h3 { margin:0 0 4px; font-size:15px; }
    .confirm-bd p { margin:0; color:var(--muted); }
    .row2 { display:grid; grid-template-columns:1fr 1fr; gap:14px; }

    .drawer { position:fixed; inset:0; z-index:65; display:none; }
    .drawer.show { display:block; }
    .drawer .scrim { position:absolute; inset:0; background:var(--scrim); animation:fadein .15s; }
    .drawer .panel { position:absolute; right:0; top:0; bottom:0; width:520px; max-width:100%; background:var(--panel);
                     border-left:1px solid var(--line); box-shadow:var(--shadow-lg); display:flex; flex-direction:column; animation:slidein .2s ease-out; }
    @keyframes slidein { from { transform:translateX(24px); opacity:.5; } to { transform:none; opacity:1; } }
    .drawer .hd { display:flex; align-items:flex-start; gap:10px; padding:18px 20px; border-bottom:1px solid var(--line); }
    .drawer .hd h2 { margin:0; font-size:17px; }
    .drawer .bd { padding:20px; overflow:auto; flex:1; }
    .kv { display:grid; grid-template-columns:130px minmax(0,1fr); gap:10px 14px; font-size:13px; margin-bottom:20px; }
    .kv .k { color:var(--muted); } .kv .v { word-break:break-word; }
    /* ---------- Plugin upload ---------- */
    .dropzone { border:1.5px dashed var(--line-strong); border-radius:10px; padding:22px 16px; text-align:center; color:var(--muted);
                background:var(--panel-2); display:flex; flex-direction:column; align-items:center; gap:6px; font-size:13px; }
    .dropzone > .i { width:22px; height:22px; color:var(--faint); }
    .dropzone.dropping { border-color:var(--accent); color:var(--accent); background:color-mix(in srgb,var(--panel) 90%,var(--accent)); }
    .dropzone .hint { font-size:12px; color:var(--muted); }
    .linkbtn { background:none; border:none; padding:0; font:inherit; color:var(--accent); cursor:pointer; text-decoration:underline; text-underline-offset:2px; }
    .upfiles { list-style:none; margin:14px 0 0; padding:0; border:1px solid var(--line); border-radius:8px; }
    .upfiles li { display:flex; gap:10px; align-items:center; padding:8px 8px 8px 12px; font-size:13px; min-height:44px; }
    .upfiles li + li { border-top:1px solid var(--line); }
    .upfiles .role { width:84px; flex:none; color:var(--muted); }
    .upfiles .fname { flex:1; min-width:0; font-family:var(--mono); font-size:12.5px; overflow-wrap:anywhere; }
    .upfiles .fname.missing { font-family:var(--font); color:var(--muted); }
    .wrap-any { overflow-wrap:anywhere; }
    .stack { margin-bottom:16px; }
    .alert.flush { margin-bottom:0; }
    .err-text { color:var(--err); }
    p.tight { margin:0 0 12px; }
    pre.snippet { margin:0; padding:10px 12px; background:var(--panel-2); border:1px solid var(--line); border-radius:8px; font:12px/1.5 var(--mono); overflow:auto; }
    .upfiles .size { color:var(--faint); font-size:12px; flex:none; }
    #upFt { flex-wrap:wrap; }
    .confirm-row { display:flex; gap:8px; align-items:flex-start; margin:0 0 12px; font-size:13px; cursor:pointer; }
    .confirm-row input { margin-top:2px; }
    .ok-text { color:var(--ok); display:inline-flex; align-items:center; gap:4px; vertical-align:bottom; }
    .hash { font:12px/1.5 var(--mono); word-break:break-all; color:var(--fg-2); }

    #toasts { position:fixed; right:20px; bottom:20px; z-index:90; display:flex; flex-direction:column; gap:8px; width:360px; max-width:calc(100vw - 40px); }
    .toast { --c:var(--info); display:flex; align-items:flex-start; gap:10px; padding:12px 12px 12px 14px; border-radius:10px; background:var(--panel);
             border:1px solid var(--line); box-shadow:var(--shadow-lg); font-size:13px; animation:pop .18s ease-out; border-left:3px solid var(--c); }
    .toast.ok { --c:var(--ok); } .toast.err { --c:var(--err); }
    .toast svg.i { color:var(--c); margin-top:1px; }
    .toast .msg { flex:1; min-width:0; word-break:break-word; }
    .toast .x { border:none; background:none; color:var(--faint); cursor:pointer; padding:0; }
    .toast .x:hover { color:var(--fg); }

    /* ---------- Editor (step builder) ---------- */
    .tabs { display:flex; gap:2px; border-bottom:1px solid var(--line); margin-bottom:12px; }
    .tab { background:none; border:none; color:var(--muted); padding:8px 12px; cursor:pointer; font:500 13px var(--font);
           border-bottom:2px solid transparent; margin-bottom:-1px; }
    .tab.active { color:var(--fg); border-bottom-color:var(--accent); }
    .tab:hover { color:var(--fg); }
    .tab:disabled { cursor:progress; }
    .tabs .tablist { display:flex; gap:2px; }
    .tabs .sp { flex:1; }
    .tabs > .btn { align-self:center; margin-bottom:3px; }
    .field .hint.ok { color:var(--ok); }
    #jYaml { white-space:pre; overflow-x:auto; }
    .def-field { position:relative; }
    .def-field .drop-hint { display:none; position:absolute; inset:0; z-index:2; align-items:flex-start; justify-content:center; gap:8px;
                            padding:72px 16px 16px; text-align:center; border:2px dashed var(--accent); border-radius:var(--radius);
                            background:color-mix(in srgb,var(--panel) 90%,var(--accent)); color:var(--accent); font-weight:600; font-size:13px; }
    .def-field.dropping .drop-hint { display:flex; }
    .step-card { border:1px solid var(--line); border-radius:8px; margin-bottom:10px; background:var(--panel); }
    .step-card .shd { display:flex; align-items:center; gap:8px; padding:8px 10px 8px 12px; border-bottom:1px solid var(--line); background:var(--panel-2); border-radius:8px 8px 0 0; }
    .step-card .shd .n { width:22px; height:22px; border-radius:6px; background:var(--accent-soft); color:var(--accent);
                         font:600 11.5px var(--mono); display:flex; align-items:center; justify-content:center; }
    .step-card .shd .cmd { font-weight:600; font-family:var(--mono); font-size:12.5px; }
    .step-card .shd .sp { flex:1; }
    .step-card .sbd { padding:12px; }
    .step-card .param { display:grid; grid-template-columns:140px minmax(0,1fr); gap:10px; align-items:start; margin-bottom:8px; }
    .step-card .param:last-child { margin-bottom:0; }
    .step-card .param .pname { font-size:12.5px; color:var(--fg-2); padding-top:7px; font-family:var(--mono); }
    .step-card .param .pname .req { color:var(--err); margin-left:2px; }
    .step-card .param .pname .opt { color:var(--faint); font-family:var(--font); font-size:11.5px; }
    .step-card .param .pdesc { font-size:11.5px; color:var(--muted); margin-top:4px; }
    .step-card .param input[type=checkbox] { margin-top:9px; }
    .step-card textarea.input { min-height:90px; }
    .kv-rows { display:flex; flex-direction:column; gap:6px; }
    .kv-row { display:flex; gap:6px; }
    .kv-row .input { flex:1; min-width:0; }
    .kv-add { align-self:flex-start; background:none; border:1px dashed var(--line-strong); border-radius:6px; padding:4px 10px; cursor:pointer;
              color:var(--muted); font:12px var(--font); }
    .kv-add:hover { color:var(--accent); border-color:var(--accent); }
    .builder-add { display:flex; gap:8px; align-items:center; margin-top:4px; }
    .builder-add select { flex:1; min-width:0; }
    .empty-hint { color:var(--muted); font-size:13px; padding:20px; border:1px dashed var(--line-strong); border-radius:8px; margin-bottom:10px; text-align:center; }
    .cron-row { display:grid; grid-template-columns:210px minmax(0,1fr); gap:8px; }

    /* ---------- Responsive ---------- */
    @media (max-width:1180px) {
      .g-kpi { grid-template-columns:repeat(2,minmax(0,1fr)); }
      .g-2-1 { grid-template-columns:minmax(0,1fr); }
      .facts { grid-template-columns:repeat(2,minmax(0,1fr)); }
      .facts > div:nth-child(2) { border-right:none; }
      .facts > div:nth-child(-n+2) { border-bottom:1px solid var(--line); }
    }
    @media (max-width:960px) {
      .jobs-layout { grid-template-columns:minmax(0,1fr); }
      .joblist { position:static; max-height:340px; }
    }
    @media (max-width:820px) {
      .shell { grid-template-columns:1fr; }
      .sidebar { position:fixed; z-index:60; left:0; width:260px; transform:translateX(-100%); transition:transform .2s; box-shadow:var(--shadow-lg); }
      body.nav-open .sidebar { transform:none; }
      .menu-btn { display:inline-flex; }
      .topbar { padding:10px 16px; } .content { padding:20px 16px 40px; }
      .updated, .crumbs > .root, .crumbs > .root + svg, .flow-hint { display:none; }
      .row2, .cron-row { grid-template-columns:1fr; }
      .step-card .param { grid-template-columns:1fr; gap:4px; }
      .step-card .param .pname { padding-top:0; }
      .g-kpi { grid-template-columns:minmax(0,1fr); }
      .tbl .hide-sm { display:none; }
      .tbl .sr-sm, .tabs .sr-sm, .dialog .ft .sr-sm { position:absolute; width:1px; height:1px; overflow:hidden; clip:rect(0 0 0 0); white-space:nowrap; }
    }
    @media (prefers-reduced-motion: reduce) { *, *::before, *::after { animation:none !important; transition:none !important; } }
  </style>
</head>
<body class="booting">
  <!-- SVG icon sprite (Lucide-style, stroke icons) -->
  <svg width="0" height="0" style="position:absolute" aria-hidden="true">
    <defs>
      <symbol id="i-logo" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/></symbol>
      <symbol id="i-home" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="9" rx="1"/><rect x="14" y="3" width="7" height="5" rx="1"/><rect x="14" y="12" width="7" height="9" rx="1"/><rect x="3" y="16" width="7" height="5" rx="1"/></symbol>
      <symbol id="i-plug" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22v-5"/><path d="M9 8V2"/><path d="M15 8V2"/><path d="M18 8v5a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V8Z"/></symbol>
      <symbol id="i-flow" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="6" height="6" rx="1"/><rect x="15" y="15" width="6" height="6" rx="1"/><path d="M6 9v3a3 3 0 0 0 3 3h6"/></symbol>
      <symbol id="i-play" viewBox="0 0 24 24" fill="currentColor" stroke="none"><path d="M7 4.5v15a1 1 0 0 0 1.5.86l12.5-7.5a1 1 0 0 0 0-1.72L8.5 3.64A1 1 0 0 0 7 4.5Z"/></symbol>
      <symbol id="i-refresh" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-3.51-7.13"/><path d="M21 4v5h-5"/></symbol>
      <symbol id="i-plus" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 5v14"/><path d="M5 12h14"/></symbol>
      <symbol id="i-search" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></symbol>
      <symbol id="i-edit" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4Z"/></symbol>
      <symbol id="i-trash" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/></symbol>
      <symbol id="i-more" viewBox="0 0 24 24" fill="currentColor"><circle cx="5" cy="12" r="1.8"/><circle cx="12" cy="12" r="1.8"/><circle cx="19" cy="12" r="1.8"/></symbol>
      <symbol id="i-x" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></symbol>
      <symbol id="i-check" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="m8.5 12 2.5 2.5 4.5-5"/></symbol>
      <symbol id="i-alert" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M12 8v4"/><path d="M12 16h.01"/></symbol>
      <symbol id="i-warn" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/><path d="M12 9v4"/><path d="M12 17h.01"/></symbol>
      <symbol id="i-info" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 16v-4"/><path d="M12 8h.01"/></symbol>
      <symbol id="i-clock" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
      <symbol id="i-activity" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></symbol>
      <symbol id="i-gauge" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 14l4-4"/><path d="M3.3 19a10 10 0 1 1 17.4 0"/></symbol>
      <symbol id="i-chev" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 18 6-6-6-6"/></symbol>
      <symbol id="i-up" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m18 15-6-6-6 6"/></symbol>
      <symbol id="i-down" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 9 6 6 6-6"/></symbol>
      <symbol id="i-power" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.8 0"/></symbol>
      <symbol id="i-logout" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/></symbol>
      <symbol id="i-menu" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16"/><path d="M4 12h16"/><path d="M4 18h16"/></symbol>
      <symbol id="i-eye" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/></symbol>
      <symbol id="i-loader" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M21 12a9 9 0 1 1-6.2-8.56"/></symbol>
      <symbol id="i-sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41"/></symbol>
      <symbol id="i-moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/></symbol>
      <symbol id="i-shield" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10Z"/></symbol>
      <symbol id="i-undo" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 14 4 9l5-5"/><path d="M4 9h11a5 5 0 0 1 0 10h-4"/></symbol>
      <symbol id="i-upload" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="m17 8-5-5-5 5"/><path d="M12 3v12"/></symbol>
      <symbol id="i-monitor" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8M12 17v4"/></symbol>
    </defs>
  </svg>

  <!-- ===================== Login ===================== -->
  <div id="login">
    <form class="card login-card" id="loginForm" autocomplete="on" novalidate>
      <div class="logo"><svg><use href="#i-logo"/></svg></div>
      <h1>Sign in to Orcha</h1>
      <p class="sub">Administer plugins, workflows and scheduled jobs.</p>
      <div id="loginErr" class="alert err" role="alert" style="display:none"><svg class="i"><use href="#i-alert"/></svg><span></span></div>
      <div class="field"><label for="user">Username</label>
        <input class="input" id="user" type="text" name="username" autocomplete="username" autocapitalize="off" spellcheck="false" required /></div>
      <div class="field"><label for="pass">Password</label>
        <div class="pw"><input class="input" id="pass" type="password" name="current-password" autocomplete="current-password" required />
          <button type="button" class="btn ghost sm icon" id="pwToggle" aria-label="Show password" title="Show password"><svg class="i"><use href="#i-eye"/></svg></button></div></div>
      <button class="btn primary" type="submit" id="signin"><svg class="i spin"><use href="#i-loader"/></svg><span>Sign in</span></button>
      <div class="login-foot"><span>Orcha workflow agent</span>
        <div class="theme-switch" role="radiogroup" aria-label="Theme"></div></div>
    </form>
  </div>

  <!-- ===================== App ===================== -->
  <div class="shell">
    <aside class="sidebar" aria-label="Primary">
      <div class="brand"><div class="logo"><svg><use href="#i-logo"/></svg></div>
        <div><div class="nm">Orcha</div><div class="sub">Admin console</div></div></div>
      <div class="navlbl">Workspace</div>
      <nav class="nav">
        <a href="#/overview" data-nav="overview"><svg class="i"><use href="#i-home"/></svg>Overview</a>
        <a href="#/plugins" data-nav="plugins"><svg class="i"><use href="#i-plug"/></svg>Plugins<span class="ct" id="navPlugins"></span></a>
        <a href="#/jobs" data-nav="jobs"><svg class="i"><use href="#i-flow"/></svg>Jobs<span class="ct" id="navJobs"></span></a>
        <a href="#/breakers" data-nav="breakers"><svg class="i"><use href="#i-shield"/></svg>Circuit breakers<span class="ct" id="navBreakers"></span></a>
      </nav>
      <div class="foot">
        <div class="theme-switch" role="radiogroup" aria-label="Theme"></div>
        <div class="user"><div class="avatar" id="avatar">?</div>
          <div class="who"><div id="who">—</div><div class="r">Administrator</div></div>
          <button class="btn ghost sm icon" id="logout" title="Sign out" aria-label="Sign out"><svg class="i"><use href="#i-logout"/></svg></button></div>
      </div>
    </aside>

    <div class="main">
      <header class="topbar">
        <button class="btn ghost icon menu-btn" id="menuBtn" aria-label="Open navigation"><svg class="i"><use href="#i-menu"/></svg></button>
        <div class="crumbs" id="crumbs"></div>
        <span class="sp"></span>
        <span class="updated" id="updated"></span>
        <button class="btn sm" id="refreshBtn" title="Refresh (R)"><svg class="i"><use href="#i-refresh"/></svg>Refresh</button>
      </header>

      <main class="content" id="content">
        <!-- ---------- Overview ---------- -->
        <section class="view" id="view-overview" aria-labelledby="h-overview">
          <div class="page-hd"><div><h1 id="h-overview">Overview</h1><p>Health of plugins, jobs and recent workflow runs.</p></div></div>
          <div id="ovAlerts"></div>
          <div class="grid g-kpi" id="kpis"></div>
          <div class="grid g-2-1">
            <div class="card">
              <div class="card-hd"><h2>Run activity</h2><span class="sub">Last 14 days · UTC</span><span class="sp"></span>
                <div class="legend"><span><i style="background:var(--ok)"></i>Success</span><span><i style="background:var(--err)"></i>Failed</span></div></div>
              <div class="card-bd"><div class="chart" id="chRuns"></div></div>
            </div>
            <div class="card">
              <div class="card-hd"><h2>Breakdown</h2><span class="sub" id="brkSub"></span></div>
              <div class="card-bd" id="breakdown"></div>
            </div>
          </div>
          <div class="grid g-2-1">
            <div class="card">
              <div class="card-hd"><h2>Recent runs</h2><span class="sp"></span><a href="#/jobs" class="sub">View jobs →</a></div>
              <div class="table-wrap"><table class="tbl">
                <thead><tr><th>Status</th><th>Job</th><th>Trigger</th><th title="Start time (queue time while queued)">When</th><th class="r">Duration</th><th>Error</th></tr></thead>
                <tbody id="recentRuns"></tbody></table></div>
            </div>
            <div class="card">
              <div class="card-hd"><h2>Upcoming</h2><span class="sub">Scheduled jobs</span></div>
              <div class="card-bd flush" id="upcoming"></div>
            </div>
          </div>
        </section>

        <!-- ---------- Plugins ---------- -->
        <section class="view" id="view-plugins" aria-labelledby="h-plugins">
          <div class="page-hd"><div><h1 id="h-plugins">Plugins</h1><p id="pluginsSub">Command plugins discovered in the plugin directory.</p></div>
            <span class="sp"></span>
            <label class="switch" title="Automatically load, reload and unload plugins as files change on disk">
              <input type="checkbox" id="watch" /><span class="track"></span>Watch directory</label>
            <button class="btn" id="uploadPluginBtn"><svg class="i"><use href="#i-upload"/></svg>Upload plugin</button></div>
          <div id="stagedUploads"></div>
          <div class="card">
            <div class="toolbar">
              <div class="search"><svg class="i"><use href="#i-search"/></svg>
                <input class="input" id="pluginSearch" type="search" placeholder="Search plugins, tags…" aria-label="Search plugins" /></div>
              <div class="seg" id="pluginFilter" role="tablist" aria-label="Filter by status"></div>
              <span class="sp"></span><span class="muted num" id="count"></span>
            </div>
            <div class="table-wrap"><table class="tbl">
              <thead><tr><th>Plugin</th><th>Version</th><th>Status</th><th>Dependencies</th><th>Tags</th><th class="r">Actions</th></tr></thead>
              <tbody id="rows"></tbody></table></div>
          </div>
        </section>

        <!-- ---------- Jobs ---------- -->
        <section class="view" id="view-jobs" aria-labelledby="h-jobs">
          <div class="page-hd"><div><h1 id="h-jobs">Jobs</h1><p>Saved workflows, their schedules and run history.</p></div>
            <span class="sp"></span>
            <button class="btn primary" id="newJob"><svg class="i"><use href="#i-plus"/></svg>New job</button></div>
          <div id="jobsAlerts"></div>
          <div class="jobs-layout">
            <div class="card joblist">
              <div class="toolbar"><div class="search"><svg class="i"><use href="#i-search"/></svg>
                <input class="input" id="jobSearch" type="search" placeholder="Search jobs…" aria-label="Search jobs" /></div></div>
              <div class="items" id="jobItems" role="listbox" aria-label="Jobs"></div>
            </div>
            <div class="card" id="jobDetail"></div>
          </div>
        </section>

        <!-- ---------- Circuit breakers ---------- -->
        <section class="view" id="view-breakers" aria-labelledby="h-breakers">
          <div class="page-hd"><div><h1 id="h-breakers">Circuit breakers</h1>
            <p>Per-command breakers used by workflows with <code>"circuit_breaker": true</code>. An open circuit fails those steps
              without running the command until a recovery probe succeeds.</p></div></div>
          <div class="card">
            <div class="table-wrap"><table class="tbl">
              <thead><tr><th>Command</th><th>State</th><th class="r hide-sm" title="Consecutive failures">Failures</th><th class="r hide-sm">Calls</th>
                <th class="r hide-sm">Failed</th><th class="r hide-sm">Rejected</th><th class="hide-sm">Last failure</th><th class="r"><span class="sr-sm">Actions</span></th></tr></thead>
              <tbody id="breakerRows"></tbody></table></div>
          </div>
        </section>
      </main>
    </div>
  </div>

  <!-- ===================== Job editor ===================== -->
  <div class="overlay" id="modal" role="dialog" aria-modal="true" aria-labelledby="modalTitle">
    <div class="dialog">
      <div class="hd"><h2 id="modalTitle">New job</h2><span class="sp"></span>
        <button class="btn ghost sm icon" data-close aria-label="Close"><svg class="i"><use href="#i-x"/></svg></button></div>
      <div class="bd">
        <div id="modalErr" class="alert err" role="alert" style="display:none"><svg class="i"><use href="#i-alert"/></svg><span></span></div>
        <div class="row2">
          <div class="field"><label for="jName">Name</label><input class="input" id="jName" type="text" placeholder="nightly-report" autocomplete="off" />
            <div class="hint">Unique. Used by <code>run_job</code> to reference this job.</div></div>
          <div class="field"><label for="jDesc">Description</label><input class="input" id="jDesc" type="text" placeholder="What this job does" autocomplete="off" /></div>
        </div>
        <div class="field"><label for="jSchedule">Schedule</label>
          <div class="cron-row">
            <select class="input" id="cronPreset" aria-label="Schedule preset">
              <option value="">Custom / manual</option>
              <option value="0 * * * *">Hourly</option>
              <option value="0 */6 * * *">Every 6 hours</option>
              <option value="0 9 * * *">Daily at 09:00</option>
              <option value="0 9 * * 1-5">Weekdays at 09:00</option>
              <option value="0 9 * * 1">Weekly, Monday 09:00</option>
              <option value="0 0 1 * *">Monthly, 1st 00:00</option>
            </select>
            <input class="input mono" id="jSchedule" type="text" placeholder="m h dom mon dow — blank = manual only" spellcheck="false" autocomplete="off" />
          </div>
          <div id="cronPreview" class="hint"></div>
        </div>
        <div class="field"><label class="switch"><input id="jEnabled" type="checkbox" /><span class="track"></span>Enabled — the scheduler runs this job when due</label></div>
        <div class="row2">
          <div class="field"><label for="jRollback">On failure</label>
            <select class="input" id="jRollback">
              <option value="none">Do nothing</option>
              <option value="reverse">Roll back, stop on error</option>
              <option value="best_effort">Roll back, best effort</option>
            </select>
            <div class="hint">Rolls back completed steps in reverse order (commands that support it) when a run fails, times out or is cancelled. Skipped when the server shuts down.</div></div>
          <div class="field"><span class="label">Circuit breaker</span>
            <label class="switch"><input id="jBreaker" type="checkbox" /><span class="track"></span>Fail fast while a command’s circuit is open</label>
            <div class="hint">Thresholds are set server-wide under <code>workflow.circuit_breaker</code>.</div></div>
        </div>
        <div class="field def-field" id="defField" style="margin-bottom:0"><span class="label">Workflow definition</span>
          <div class="tabs">
            <div class="tablist" role="tablist" aria-label="Definition editor">
              <button class="tab active" type="button" role="tab" id="tabVisual" data-deftab="visual" aria-selected="true" aria-controls="defVisual">Visual builder</button>
              <button class="tab" type="button" role="tab" id="tabJson" data-deftab="json" aria-selected="false" aria-controls="defJson" tabindex="-1">JSON</button>
              <button class="tab" type="button" role="tab" id="tabYaml" data-deftab="yaml" aria-selected="false" aria-controls="defYaml" tabindex="-1">YAML</button>
            </div>
            <span class="sp"></span>
            <button type="button" class="btn ghost sm" id="defImport" title="Import a .yaml, .yml or .json file — or drop one here" aria-label="Import file"><svg class="i"><use href="#i-upload"/></svg><span class="sr-sm">Import file</span></button>
            <input type="file" id="defFile" accept=".yaml,.yml,.json,application/json,application/yaml,text/yaml" hidden />
          </div>
          <div id="defWarn" class="alert" style="display:none"><svg class="i"><use href="#i-warn"/></svg><span></span></div>
          <div id="defVisual" role="tabpanel" aria-labelledby="tabVisual">
            <div id="defSteps"></div>
            <div class="builder-add">
              <select class="input" id="addStepCmd" aria-label="Command for new step"><option value="">Select a command…</option></select>
              <button type="button" class="btn" id="addStepBtn"><svg class="i"><use href="#i-plus"/></svg>Add step</button>
            </div>
            <div class="hint">Reference earlier outputs with <code>{{steps.NAME.output.field}}</code> or <code>{{stepN.output}}</code>.</div>
          </div>
          <div id="defJson" role="tabpanel" aria-labelledby="tabJson" style="display:none">
            <textarea class="input" id="jDef" spellcheck="false" aria-label="Workflow definition JSON"></textarea>
          </div>
          <div id="defYaml" role="tabpanel" aria-labelledby="tabYaml" style="display:none">
            <textarea class="input" id="jYaml" spellcheck="false" autocapitalize="off" wrap="off" aria-label="Workflow definition YAML" aria-describedby="yamlStatus"></textarea>
            <div class="hint" id="yamlStatus" aria-live="polite"></div>
          </div>
          <div class="drop-hint" aria-hidden="true"><svg class="i"><use href="#i-upload"/></svg>Drop a .yaml, .yml or .json file to import it</div>
        </div>
      </div>
      <div class="ft"><span class="kbd-hint"><kbd>⌘</kbd>/<kbd>Ctrl</kbd> + <kbd>Enter</kbd> to save</span><span class="sp"></span>
        <button class="btn" data-close>Cancel</button>
        <button class="btn primary" id="modalSave"><svg class="i spin"><use href="#i-loader"/></svg><span>Save job</span></button></div>
    </div>
  </div>

  <!-- ===================== Plugin upload ===================== -->
  <div class="overlay" id="upload" role="dialog" aria-modal="true" aria-labelledby="upTitle">
    <div class="dialog md">
      <div class="hd"><h2 id="upTitle">Upload plugin</h2><span class="sp"></span>
        <button class="btn ghost sm icon" data-close aria-label="Close"><svg class="i"><use href="#i-x"/></svg></button></div>
      <div class="bd" id="upBody"></div>
      <div class="ft" id="upFt"></div>
    </div>
  </div>
  <input type="file" id="upFile" multiple hidden />

  <!-- ===================== Confirm dialog ===================== -->
  <div class="overlay" id="confirm" role="alertdialog" aria-modal="true" aria-labelledby="cfTitle" aria-describedby="cfBody">
    <div class="dialog sm">
      <div class="bd"><div class="confirm-bd"><div class="ic"><svg class="i"><use href="#i-warn"/></svg></div>
        <div><h3 id="cfTitle"></h3><p id="cfBody"></p></div></div></div>
      <div class="ft"><span class="sp"></span><button class="btn" id="cfCancel">Cancel</button><button class="btn danger-solid" id="cfOk">Confirm</button></div>
    </div>
  </div>

  <!-- ===================== Plugin drawer ===================== -->
  <div class="drawer" id="drawer" role="dialog" aria-modal="true" aria-labelledby="drTitle">
    <div class="scrim" data-close></div>
    <div class="panel">
      <div class="hd"><div style="flex:1;min-width:0"><h2 id="drTitle"></h2><div class="muted" id="drSub"></div></div>
        <button class="btn ghost sm icon" data-close aria-label="Close"><svg class="i"><use href="#i-x"/></svg></button></div>
      <div class="bd" id="drBody"></div>
    </div>
  </div>

  <div id="toasts" aria-live="polite" aria-atomic="false"></div>

  <script>
  (function(){
    'use strict';
    const $ = (id) => document.getElementById(id);
    const $$ = (sel, root) => Array.from((root||document).querySelectorAll(sel));
    const esc = (s) => String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
    const icon = (name, cls) => `<svg class="i ${cls||''}" aria-hidden="true"><use href="#i-${name}"/></svg>`;
    const store = {
      get(s,k){ try { return s.getItem(k); } catch(e){ return null; } },
      set(s,k,v){ try { s.setItem(k,v); } catch(e){} },
      del(s,k){ try { s.removeItem(k); } catch(e){} }
    };

    // ================= Formatting =================
    const fmtNum = (n) => (n==null) ? '—' : Number(n).toLocaleString();
    function parseTs(iso){ if(!iso) return null; const d=new Date(iso); return isNaN(d) ? null : d; }
    function fmtAbs(iso){ const d=parseTs(iso); return d ? d.toLocaleString(undefined,{dateStyle:'medium', timeStyle:'medium'}) : (iso||''); }
    function fmtAgo(iso){
      const d=parseTs(iso); if(!d) return '—';
      const s=(Date.now()-d.getTime())/1000;
      if(s<0){ return 'in '+fmtSpan(-s); }
      if(s<45) return 'just now';
      return fmtSpan(s)+' ago';
    }
    function fmtSpan(s){
      if(s<60) return Math.round(s)+'s';
      if(s<3600) return Math.round(s/60)+'m';
      if(s<86400) return Math.round(s/3600)+'h';
      if(s<86400*30) return Math.round(s/86400)+'d';
      return Math.round(s/86400/30)+'mo';
    }
    function fmtDur(run){
      const a=parseTs(run.started_at);
      const b=parseTs(run.finished_at) || (run.status==='running' ? new Date() : null); // live elapsed
      if(!a||!b) return '—';
      const ms=b-a; if(ms<1000) return '<1s';
      const s=ms/1000; if(s<60) return s.toFixed(s<10?1:0)+'s';
      return Math.floor(s/60)+'m '+Math.round(s%60)+'s';
    }
    const timeCell = (iso) => `<time datetime="${esc(iso||'')}" title="${esc(fmtAbs(iso))}">${esc(fmtAgo(iso))}</time>`;
    const statusBadge = (st) => `<span class="badge ${esc(st||'unknown')}">${esc(st||'unknown')}</span>`;
    // Run lifecycle: queued -> running -> success | failed | cancelled; or interrupted / skipped.
    const isLive = (st) => st==='queued' || st==='running';
    const runTime = (r) => r.started_at || r.queued_at;   // queued runs have no started_at yet

    // ================= Theme =================
    const THEME_KEY='orcha_theme';
    const THEMES=[['light','sun','Light'],['dark','moon','Dark'],['system','monitor','System']];
    $$('.theme-switch').forEach(el => {
      el.innerHTML = THEMES.map(([t,ic,lbl]) =>
        `<button type="button" role="radio" data-theme="${t}" title="${lbl} theme" aria-label="${lbl} theme">${icon(ic)}</button>`).join('');
    });
    function syncThemeButtons(t){
      $$('.theme-switch button').forEach(b => { const on=b.dataset.theme===t; b.classList.toggle('active',on); b.setAttribute('aria-checked',on); });
    }
    function setTheme(t){
      document.documentElement.dataset.theme=t; store.set(localStorage,THEME_KEY,t); syncThemeButtons(t);
      if(flow.job) flow.render();
      if(state.view==='overview') renderOverview();
    }
    syncThemeButtons(document.documentElement.dataset.theme||'system');
    $$('.theme-switch button').forEach(b => b.addEventListener('click', () => setTheme(b.dataset.theme)));
    if(window.matchMedia) window.matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => {
      if(document.documentElement.dataset.theme==='system'){ if(flow.job) flow.render(); if(state.view==='overview') renderOverview(); }
    });

    // ================= Toasts =================
    function toast(msg, kind){
      const host=$('toasts'); const el=document.createElement('div');
      el.className='toast '+(kind||''); el.setAttribute('role', kind==='err'?'alert':'status');
      el.innerHTML = icon(kind==='ok'?'check':kind==='err'?'alert':'info')+`<div class="msg">${esc(msg)}</div>`+
        `<button class="x" aria-label="Dismiss">${icon('x')}</button>`;
      const kill=()=>{ el.remove(); };
      el.querySelector('.x').onclick=kill;
      host.appendChild(el);
      while(host.children.length>4) host.firstChild.remove();
      setTimeout(kill, kind==='err'?7000:4000);
    }

    // ================= Overlays (modal / confirm / drawer) =================
    let lastFocus=null;
    function openOverlay(el, focusSel){
      lastFocus=document.activeElement; el.classList.add('show');
      // Resolve the target when focusing: the content may have re-rendered meanwhile.
      setTimeout(()=>{
        const f = (focusSel && el.querySelector(focusSel)) || el.querySelector('input,select,textarea,button');
        if(f && !el.contains(document.activeElement)) f.focus();
      }, 30);
    }
    function closeOverlay(el){
      el.classList.remove('show');
      const back = el._returnFocus || lastFocus; el._returnFocus = null;
      if(back && back.isConnected && back.focus) back.focus();
      else if(el.id==='upload') $('uploadPluginBtn').focus();
    }
    function topOverlay(){ return ['confirm','upload','modal','drawer'].map($).find(e=>e.classList.contains('show')); }
    // Trap Tab inside the top-most overlay; Esc closes it.
    document.addEventListener('keydown', e => {
      const ov=topOverlay();
      if(e.key==='Escape' && ov){ e.preventDefault(); if(ov.id==='confirm') confirmResolve(false); else closeOverlay(ov); return; }
      if(e.key==='Tab' && ov){
        const f=$$('button:not([disabled]),input:not([disabled]),select,textarea,a[href]',ov).filter(x=>x.offsetParent!==null);
        if(!f.length) return; const first=f[0], last=f[f.length-1];
        if(e.shiftKey && document.activeElement===first){ e.preventDefault(); last.focus(); }
        else if(!e.shiftKey && document.activeElement===last){ e.preventDefault(); first.focus(); }
      }
    });
    $$('[data-close]').forEach(b => b.addEventListener('click', () => closeOverlay(b.closest('.overlay,.drawer'))));
    $('modal').addEventListener('mousedown', e => { if(e.target===$('modal')) closeOverlay($('modal')); });

    let confirmResolve=()=>{};
    // opts.tone: 'danger' (default, destructive) or 'primary' (recovery actions).
    // opts.cancelLabel: the dismiss button's text (default "Cancel"; use it when
    // the action itself is a cancel, e.g. "Keep run").
    function confirmDialog(title, body, okLabel, opts){
      const primary = !!(opts && opts.tone==='primary');   // toggle(x, undefined) would flip, not clear
      $('cfTitle').textContent=title; $('cfBody').textContent=body; $('cfOk').textContent=okLabel||'Confirm';
      $('cfCancel').textContent=(opts && opts.cancelLabel) || 'Cancel';
      $('cfOk').className = 'btn '+(primary?'primary':'danger-solid');
      $('confirm').classList.toggle('tone-primary', primary);
      $('confirm').querySelector('.confirm-bd .ic use').setAttribute('href', primary?'#i-info':'#i-warn');
      openOverlay($('confirm'), '#cfCancel');
      return new Promise(res => { confirmResolve = (v)=>{ closeOverlay($('confirm')); confirmResolve=()=>{}; res(v); }; });
    }
    $('cfOk').onclick=()=>confirmResolve(true);
    $('cfCancel').onclick=()=>confirmResolve(false);
    $('confirm').addEventListener('mousedown', e => { if(e.target===$('confirm')) confirmResolve(false); });

    function busy(btn, on){ if(!btn) return; btn.disabled=on; btn.classList.toggle('loading',on); }

    // ================= Auth / API =================
    const AUTH_KEY='orcha_auth', USER_KEY='orcha_user';
    const getAuth=()=>store.get(sessionStorage,AUTH_KEY);
    const b64=(s)=>btoa(unescape(encodeURIComponent(s)));
    const isAuthErr=(e)=>e && e.message==='Unauthorized';

    async function api(path, opts){
      opts=opts||{}; const headers=Object.assign({}, opts.headers||{});
      const auth=getAuth(); if(auth) headers['Authorization']=auth;
      if(opts.json!==undefined){ headers['Content-Type']='application/json'; opts.body=JSON.stringify(opts.json); }
      const res=await fetch(path, Object.assign({}, opts, {headers}));
      if(res.status===401){ signOut('Your session has expired. Please sign in again.'); throw new Error('Unauthorized'); }
      let body=null; try{ body=await res.json(); }catch(e){}
      if(!res.ok){
        const err=new Error((body&&(body.error||body.message))||('HTTP '+res.status));
        err.status=res.status; err.body=body;
        const ra=parseInt(res.headers.get('Retry-After')||'',10); if(ra>0) err.retryAfter=ra;
        throw err;
      }
      return body;
    }
    function loginError(msg){
      const el=$('loginErr'); el.style.display=msg?'flex':'none'; el.querySelector('span').textContent=msg||'';
    }
    function showLogin(msg){
      document.body.classList.remove('authed'); loginError(msg);
      $('pass').value=''; setTimeout(()=>($('user').value?$('pass'):$('user')).focus(),0);
    }
    function signOut(msg){
      store.del(sessionStorage,AUTH_KEY); stopPolling();
      state.plugins=null; state.jobs=[]; state.runs=[]; state.currentJob=null;
      showLogin(msg);
    }
    function enterApp(){
      const u=store.get(sessionStorage,USER_KEY)||'admin';
      $('who').textContent=u; $('avatar').textContent=u.slice(0,1);
      document.body.classList.add('authed');
      route(); refreshAll(); startPolling();
    }
    $('loginForm').addEventListener('submit', async e => {
      e.preventDefault();
      const user=$('user').value.trim(), pass=$('pass').value, btn=$('signin');
      if(!user||!pass){ loginError('Enter your username and password.'); return; }
      busy(btn,true); loginError('');
      try {
        const token='Basic '+b64(user+':'+pass);
        const res=await fetch('/api/plugins',{headers:{Authorization:token}});
        if(res.status===401){ loginError('Invalid username or password.'); $('pass').select(); return; }
        if(!res.ok){ loginError('Server error (HTTP '+res.status+'). Check the agent logs.'); return; }
        store.set(sessionStorage,AUTH_KEY,token); store.set(sessionStorage,USER_KEY,user); enterApp();
      } catch(err){ loginError('Could not reach the Orcha agent. Is it running?'); }
      finally { busy(btn,false); }
    });
    $('pwToggle').addEventListener('click', () => {
      const p=$('pass'), show=p.type==='password'; p.type=show?'text':'password';
      $('pwToggle').setAttribute('aria-label', show?'Hide password':'Show password'); p.focus();
    });
    $('logout').addEventListener('click', () => { signOut(''); toast('Signed out','ok'); });

    // ================= State + routing =================
    const state = { view:'overview', plugins:null, jobs:[], runs:[], queue:null, breakers:null, breakersErr:null, currentJob:null, selectedRunId:null,
                    pluginFilter:'all', pluginQuery:'', jobQuery:'', loadedAt:null, jobRuns:[], persistent:true, watchErr:null };

    function route(){
      const h=(location.hash||'#/overview').replace(/^#\/?/,'').split('/');
      const view=['overview','plugins','jobs','breakers'].includes(h[0]) ? h[0] : 'overview';
      const jobId = view==='jobs' && h[1] ? decodeURIComponent(h[1]) : null;
      state.view=view;
      $$('.view').forEach(v=>v.classList.toggle('active', v.id==='view-'+view));
      $$('.nav a').forEach(a=>{ const on=a.dataset.nav===view; a.classList.toggle('active',on); if(on) a.setAttribute('aria-current','page'); else a.removeAttribute('aria-current'); });
      document.body.classList.remove('nav-open');
      if(view==='jobs'){
        if(jobId && (!state.currentJob || state.currentJob.id!==jobId)) selectJob(jobId);
        else if(!jobId){ state.currentJob=null; renderJobList(); renderJobDetail(); }
        else if(flow.job) flow.resize();
      }
      if(view==='overview') renderOverview();
      if(view==='breakers') renderBreakers();
      renderCrumbs();
    }
    window.addEventListener('hashchange', route);
    function renderCrumbs(){
      const titles={overview:'Overview', plugins:'Plugins', jobs:'Jobs', breakers:'Circuit breakers'};
      let html=`<span class="faint root">Orcha</span>${icon('chev','faint')}`;
      if(state.view==='jobs' && state.currentJob){
        html+=`<a href="#/jobs">Jobs</a>${icon('chev','faint')}<span class="cur">${esc(state.currentJob.name)}</span>`;
      } else html+=`<span class="cur">${titles[state.view]}</span>`;
      $('crumbs').innerHTML=html;
    }
    $('menuBtn').addEventListener('click', ()=>document.body.classList.toggle('nav-open'));
    document.addEventListener('click', e => {
      if(document.body.classList.contains('nav-open') && !e.target.closest('.sidebar,#menuBtn')) document.body.classList.remove('nav-open');
    });

    // ================= Data loading =================
    async function loadPluginsData(){ state.plugins = await api('/api/plugins'); }
    async function loadUploadsData(){
      try { state.uploads = await api('/api/plugins/_uploads'); state.uploadsErr=null; }
      catch(e){ if(!isAuthErr(e)) state.uploadsErr=e.message||'request failed'; throw e; }
    }
    async function loadJobsData(){ const d=await api('/api/jobs'); state.jobs=d.jobs||[]; }
    async function loadRunsData(){ const d=await api('/api/runs?limit=200'); state.runs=d.runs||[]; state.queue=d.queue||null; state.persistent=d.persistent!==false; }
    async function loadBreakersData(){
      try { const d=await api('/api/circuit-breakers'); state.breakers=d.circuit_breakers||[]; state.breakersErr=null; }
      catch(e){ if(!isAuthErr(e)) state.breakersErr=e.message||'request failed'; throw e; }
    }

    async function refreshAll(){
      const btn=$('refreshBtn'); btn.classList.add('spinning'); btn.disabled=true;
      // Fail soft per endpoint: render whatever we can.
      const results = await Promise.allSettled([loadPluginsData(), loadJobsData(), loadRunsData(), loadBreakersData(), loadUploadsData()]);
      btn.classList.remove('spinning'); btn.disabled=false;
      if(!document.body.classList.contains('authed')) return;
      const failed=results.filter(r=>r.status==='rejected' && !isAuthErr(r.reason));
      if(failed.length) toast('Some data failed to load: '+failed[0].reason.message,'err');
      state.loadedAt=Date.now(); tickUpdated();
      renderNavCounts(); renderPlugins(); renderStagedUploads(); renderJobList(); renderOverview(); renderBreakers();
      if(state.view==='jobs' && state.currentJob) refreshCurrentJob();
    }
    $('refreshBtn').addEventListener('click', refreshAll);

    // Background refresh of overview/run data while the tab is visible.
    let pollT=null, updT=null;
    function startPolling(){
      stopPolling();
      pollT=setInterval(()=>{ if(!document.hidden && !topOverlay()) refreshAll(); }, 30000);
      updT=setInterval(tickUpdated, 10000);
    }
    function stopPolling(){ clearInterval(pollT); clearInterval(updT); }
    function tickUpdated(){ $('updated').textContent = state.loadedAt ? 'Updated '+fmtAgo(new Date(state.loadedAt).toISOString()) : ''; }

    function renderNavCounts(){
      const pl=(state.plugins&&state.plugins.plugins)||[];
      $('navPlugins').textContent = state.plugins ? pl.length : '';
      $('navJobs').textContent = state.jobs.length || '';
      const tripped=trippedBreakers(), nb=$('navBreakers');
      if(state.breakersErr){ nb.textContent='?'; nb.classList.add('hot'); nb.title='Circuit breaker status unavailable'; return; }
      nb.textContent = tripped.length || '';
      nb.classList.toggle('hot', tripped.length>0);
      nb.title = tripped.length ? tripped.length+' circuit'+(tripped.length===1?'':'s')+' not closed' : '';
    }
    const trippedBreakers = () => (state.breakers||[]).filter(b=>b.state!=='closed');
    const jobNameById = () => Object.fromEntries(state.jobs.map(j=>[j.id,j.name]));
    function lastRunByJob(){
      const m={}; state.runs.forEach(r=>{ if(r.job_id && !m[r.job_id]) m[r.job_id]=r; }); return m; // runs are newest-first
    }

    // ================= Overview =================
    const cssVar = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim() || '#888';

    // The job database could not be opened: the server runs on an in-memory store.
    function storeNotice(){
      return state.persistent ? '' : `<div class="alert err" role="alert">${icon('alert')}<span><strong>Run history is not saved.</strong>
        The job database could not be opened, so jobs and runs are kept in memory and will be lost when the server restarts. Check <code>jobs.db_path</code> and the server log.</span></div>`;
    }
    function renderOverview(){
      if(!document.body.classList.contains('authed')) return;
      const pl=(state.plugins&&state.plugins.plugins)||[];
      const loaded=pl.filter(p=>p.status==='loaded').length, available=pl.filter(p=>p.status==='available').length,
            disabled=pl.filter(p=>p.status==='disabled').length;
      const jobs=state.jobs, enabled=jobs.filter(j=>j.enabled).length, scheduled=jobs.filter(j=>j.schedule_cron).length;
      const runs=state.runs, succ=runs.filter(r=>r.status==='success').length, fail=runs.filter(r=>r.status==='failed').length;
      // Rate over decided outcomes; queued/running/cancelled/skipped runs say nothing about health.
      const rate=(succ+fail) ? Math.round(100*succ/(succ+fail)) : null;
      const rateColor = rate==null ? 'var(--faint)' : rate>=90 ? 'var(--ok)' : rate>=60 ? 'var(--warn)' : 'var(--err)';
      const pct=(a,b)=>b?(100*a/b):0;
      const lastFail = runs.find(r=>r.status==='failed');

      $('kpis').innerHTML = [
        `<div class="card kpi"><div class="lbl">${icon('plug')}Plugins loaded</div>
          <div class="val">${fmtNum(loaded)}<small>/ ${fmtNum(pl.length)}</small></div>
          <div class="meter"><span style="width:${pct(loaded,pl.length)}%;background:var(--ok)"></span><span style="width:${pct(available,pl.length)}%;background:var(--info)"></span></div>
          <div class="meta">${available} available · ${disabled} disabled</div></div>`,
        `<div class="card kpi"><div class="lbl">${icon('flow')}Jobs</div>
          <div class="val">${fmtNum(jobs.length)}</div>
          <div class="meter"><span style="width:${pct(enabled,jobs.length)}%;background:var(--accent)"></span></div>
          <div class="meta">${enabled} enabled · ${scheduled} scheduled</div></div>`,
        `<div class="card kpi"><div class="lbl">${icon('activity')}Recent runs</div>
          <div class="val">${fmtNum(runs.length)}</div>
          <div class="meter"><span style="width:${pct(succ,runs.length)}%;background:var(--ok)"></span><span style="width:${pct(fail,runs.length)}%;background:var(--err)"></span></div>
          <div class="meta">${succ} succeeded · ${fail} failed${queueMeta()}</div></div>`,
        `<div class="card kpi"><div class="lbl">${icon('gauge')}Success rate</div>
          <div class="val" style="color:${rateColor}">${rate==null?'—':rate+'<small>%</small>'}</div>
          <div class="meter"><span style="width:${rate||0}%;background:${rateColor}"></span></div>
          <div class="meta">${lastFail ? 'Last failure '+timeCell(runTime(lastFail)) : 'No failures recorded'}</div></div>`
      ].join('');

      $('ovAlerts').innerHTML = storeNotice()+breakerNotice();

      renderRunsChart(runs);

      // Breakdown: triggers + plugin health as proportional bars.
      const trig={}; runs.forEach(r=>{ const t=r.trigger||'unknown'; trig[t]=(trig[t]||0)+1; });
      const tColor={manual:'var(--pink)', api:'var(--info)', schedule:'var(--violet)', unknown:'var(--faint)'};
      const brkRow=(label,v,total,color)=>`<div class="brk-row"><div class="top"><span class="dot" style="background:${color}"></span>${esc(label)}
          <span class="v">${fmtNum(v)}</span><span class="pct">${total?Math.round(100*v/total):0}%</span></div>
          <div class="bar"><span style="width:${pct(v,total)}%;background:${color}"></span></div></div>`;
      const trigKeys=Object.keys(trig).sort((a,b)=>trig[b]-trig[a]);
      $('brkSub').textContent = runs.length ? runs.length+' runs' : '';
      $('breakdown').innerHTML =
        `<div class="sec-lbl">Run triggers</div>`+
        (trigKeys.length ? `<div class="brk">${trigKeys.map(t=>brkRow(t,trig[t],runs.length,tColor[t]||'var(--faint)')).join('')}</div>`
                         : `<div class="muted">No runs yet.</div>`)+
        `<div class="divider"></div><div class="sec-lbl">Plugin status</div>`+
        (pl.length ? `<div class="brk">${brkRow('Loaded',loaded,pl.length,'var(--ok)')}${brkRow('Available',available,pl.length,'var(--info)')}${brkRow('Disabled',disabled,pl.length,'var(--faint)')}</div>`
                   : `<div class="muted">No plugins discovered.</div>`);

      // Recent runs.
      const names=jobNameById();
      const recent=runs.slice(0,8);
      $('recentRuns').innerHTML = recent.length ? recent.map(r=>{
        const jn = r.job_id ? (names[r.job_id] ? `<a href="#/jobs/${encodeURIComponent(r.job_id)}">${esc(names[r.job_id])}</a>` : `<span class="mono muted">${esc(r.job_id.slice(0,8))}</span>`)
                            : '<span class="muted">ad-hoc</span>';
        return `<tr><td>${statusBadge(r.status)}</td><td class="nowrap">${jn}</td><td><span class="badge plain ${esc(r.trigger||'unknown')}">${esc(r.trigger||'—')}</span></td>
          <td class="nowrap muted">${timeCell(runTime(r))}</td><td class="r num muted">${fmtDur(r)}</td>
          <td><div class="trunc muted" title="${esc(r.error||'')}">${esc(r.error||'')}</div></td></tr>`;
      }).join('') : `<tr><td colspan="6">${emptyState('activity','No runs yet','Run a job manually or wait for the scheduler to pick one up.')}</td></tr>`;

      // Upcoming scheduled runs (UTC cron, shown in local time).
      const up=[];
      jobs.filter(j=>j.enabled && j.schedule_cron).forEach(j=>{ try { const n=cronNext(j.schedule_cron,1)[0]; if(n) up.push({j,n}); } catch(e){} });
      up.sort((a,b)=>a.n-b.n);
      $('upcoming').innerHTML = up.length ? `<table class="tbl"><tbody>${up.slice(0,8).map(({j,n})=>`
          <tr class="click" data-href="#/jobs/${encodeURIComponent(j.id)}"><td><div class="pname">${esc(j.name)}</div><div class="mono muted">${esc(j.schedule_cron)}</div></td>
          <td class="r nowrap"><div>${esc(fmtAgo(n.toISOString()))}</div><div class="muted" style="font-size:12px">${esc(n.toLocaleString(undefined,{weekday:'short',hour:'2-digit',minute:'2-digit'}))}</div></td></tr>`).join('')}</tbody></table>`
        : emptyState('clock','Nothing scheduled','Give an enabled job a cron schedule to see its next run here.');
    }

    // " · 2 running · 3 queued of 64" from the /api/runs queue stats (empty when idle).
    function queueMeta(){
      const q=state.queue; if(!q || (!q.running && !q.queued)) return '';
      return ` · <span title="Runner slots ${fmtNum(q.max_concurrent)}, queue capacity ${fmtNum(q.max_queued)}">${fmtNum(q.running)} running · ${fmtNum(q.queued)} queued</span>`;
    }

    function emptyState(ic, title, body, action){
      return `<div class="empty"><div class="ic">${icon(ic)}</div><h3>${esc(title)}</h3><p>${esc(body)}</p>${action||''}</div>`;
    }

    // Stacked bar chart of success/failed runs per UTC day (hand-rolled SVG).
    function renderRunsChart(runs){
      const host=$('chRuns');
      const days=[]; const today=new Date(); today.setUTCHours(0,0,0,0);
      for(let i=13;i>=0;--i){ const d=new Date(today); d.setUTCDate(d.getUTCDate()-i); days.push(d.toISOString().slice(0,10)); }
      const S=Object.fromEntries(days.map(d=>[d,0])), F=Object.fromEntries(days.map(d=>[d,0]));
      runs.forEach(r=>{ const k=(runTime(r)||'').slice(0,10); if(k in S){ if(r.status==='success') S[k]++; else if(r.status==='failed') F[k]++; } });
      const max=Math.max(1,...days.map(d=>S[d]+F[d]));
      const nice = max<=4 ? 4 : Math.ceil(max/4)*4;
      const W=host.clientWidth||600, H=host.clientHeight||240, L=30, R=4, T=8, B=24;
      const iw=W-L-R, ih=H-T-B, bw=iw/days.length, barW=Math.max(4,Math.min(28,bw*.6));
      const y=v=>T+ih-(v/nice)*ih;
      let g='';
      for(let k=0;k<=4;k++){ const v=nice*k/4, yy=y(v);
        g+=`<line class="gl" x1="${L}" x2="${W-R}" y1="${yy}" y2="${yy}"${k?' stroke-dasharray="3 3"':''}/><text class="ax" x="${L-8}" y="${yy+4}" text-anchor="end">${v}</text>`; }
      let bars='', hits='';
      days.forEach((d,i)=>{
        const cx=L+bw*i+bw/2, x=cx-barW/2, s=S[d], f=F[d];
        const ys=y(s), yf=y(s+f);
        if(s) bars+=`<rect x="${x}" y="${ys}" width="${barW}" height="${T+ih-ys}" rx="2" fill="var(--ok)"/>`;
        if(f) bars+=`<rect x="${x}" y="${yf}" width="${barW}" height="${ys-yf-(s?1:0)}" rx="2" fill="var(--err)"/>`;
        if(i%2===days.length%2 || days.length<=7){ const dt=new Date(d+'T00:00:00Z');
          g+=`<text class="ax" x="${cx}" y="${H-6}" text-anchor="middle">${dt.toLocaleDateString(undefined,{month:'short',day:'numeric',timeZone:'UTC'})}</text>`; }
        hits+=`<rect class="hit" x="${L+bw*i}" y="${T}" width="${bw}" height="${ih}" data-d="${d}" data-s="${s}" data-f="${f}"/>`;
      });
      const total=days.reduce((a,d)=>a+S[d]+F[d],0);
      host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Runs per day for the last 14 days: ${total} total">${g}${bars}${hits}</svg><div class="ctip"></div>`+
        (total?'':`<div style="position:absolute;inset:0;display:flex;align-items:center;justify-content:center" class="muted">No finished runs in the last 14 days</div>`);
      const tip=host.querySelector('.ctip');
      host.querySelectorAll('.hit').forEach(h=>{
        h.addEventListener('mouseenter',()=>{
          const dt=new Date(h.dataset.d+'T00:00:00Z');
          tip.innerHTML=`<div class="t">${esc(dt.toLocaleDateString(undefined,{weekday:'short',month:'short',day:'numeric',timeZone:'UTC'}))}</div>
            <div class="r"><span class="dot ok"></span>Success<b>${h.dataset.s}</b></div><div class="r"><span class="dot err"></span>Failed<b>${h.dataset.f}</b></div>`;
          tip.style.display='block';
          const x=parseFloat(h.getAttribute('x'))+parseFloat(h.getAttribute('width'))/2;
          tip.style.left=Math.min(Math.max(0,x-70), W-150)+'px'; tip.style.top='0px';
        });
        h.addEventListener('mouseleave',()=>{ tip.style.display='none'; });
      });
    }
    let rzT=null; window.addEventListener('resize', ()=>{ clearTimeout(rzT); rzT=setTimeout(()=>{ if(state.view==='overview') renderRunsChart(state.runs); },120); });

    // ================= Plugins =================
    function pluginMatches(p){
      if(state.pluginFilter!=='all' && p.status!==state.pluginFilter) return false;
      const q=state.pluginQuery; if(!q) return true;
      return [p.name,p.description,p.author,...(p.tags||[])].some(x=>String(x||'').toLowerCase().includes(q));
    }
    function renderPlugins(){
      const d=state.plugins;
      if(!d){ $('rows').innerHTML=skeletonRows(6,4); return; }
      const pl=d.plugins||[];
      $('watch').checked=!!d.watching;
      $('pluginsSub').innerHTML = d.directory ? `Command plugins discovered in <code>${esc(d.directory)}</code>.` : 'Command plugins discovered in the plugin directory.';
      const counts={all:pl.length}; pl.forEach(p=>counts[p.status]=(counts[p.status]||0)+1);
      $('pluginFilter').innerHTML = [['all','All'],['loaded','Loaded'],['available','Available'],['disabled','Disabled']]
        .map(([k,l])=>`<button type="button" role="tab" data-pf="${k}" class="${state.pluginFilter===k?'active':''}" aria-selected="${state.pluginFilter===k}">${l}<span class="ct">${counts[k]||0}</span></button>`).join('');
      const list=pl.filter(pluginMatches);
      $('count').textContent = `${list.length} of ${pl.length} · ${(d.commands||[]).length} commands registered`;
      if(!pl.length){ $('rows').innerHTML=`<tr><td colspan="6">${emptyState('plug','No plugins found','Drop a plugin library into the plugin directory, or enable “Watch directory” to pick it up automatically.')}</td></tr>`; return; }
      if(!list.length){ $('rows').innerHTML=`<tr><td colspan="6">${emptyState('search','No matching plugins','Try a different search term or status filter.')}</td></tr>`; return; }
      $('rows').innerHTML = list.map(p=>{
        const deps=(p.dependencies||[]).length ? (p.dependencies||[]).map(x=>`<span class="tag mono">${esc(x)}</span>`).join('') : '<span class="faint">—</span>';
        const tags=(p.tags||[]).length ? (p.tags||[]).map(t=>`<span class="tag">${esc(t)}</span>`).join('') : '<span class="faint">—</span>';
        const n=esc(p.name);
        const acts = p.status==='loaded'
          ? `<button class="btn sm" data-pa="reload" data-name="${n}" title="Reload library from disk">${icon('refresh','ico')}${icon('loader','spin')}Reload</button>
             <button class="btn sm danger" data-pa="disable" data-name="${n}" title="Unload and keep disabled across restarts">${icon('power','ico')}${icon('loader','spin')}Disable</button>`
          : `<button class="btn sm" data-pa="enable" data-name="${n}">${icon('power','ico')}${icon('loader','spin')}Enable</button>`;
        const src = p.source==='uploaded' ? ` <span class="tag" title="${p.overrides?'Uploaded; overrides the bundled '+esc(p.overrides.version):'Uploaded from the dashboard'}">uploaded</span>` : '';
        return `<tr class="click" data-plugin="${n}"><td><div class="pname">${n}${src}</div><div class="pdesc">${esc(p.description||'')}</div></td>
          <td class="mono nowrap">${esc(p.version||'—')}</td><td>${statusBadge(p.status)}</td><td>${deps}</td><td>${tags}</td>
          <td><div class="acts">${acts}</div></td></tr>`;
      }).join('');
    }
    function skeletonRows(cols, n){
      return Array.from({length:n},()=>`<tr>${Array.from({length:cols},(_,i)=>`<td><span class="skel" style="width:${i?50:80}%"></span></td>`).join('')}</tr>`).join('');
    }
    $('pluginFilter').addEventListener('click', e=>{ const b=e.target.closest('[data-pf]'); if(!b) return; state.pluginFilter=b.dataset.pf; renderPlugins(); });
    $('pluginSearch').addEventListener('input', e=>{ state.pluginQuery=e.target.value.trim().toLowerCase(); renderPlugins(); });

    async function pluginAct(btn, name, action){
      if(action==='disable'){
        const ok=await confirmDialog('Disable '+name+'?', 'The plugin is unloaded now and stays disabled across restarts until you enable it again. Jobs that use its commands will fail.', 'Disable plugin');
        if(!ok) return;
      }
      busy(btn,true);
      try { const r=await api('/api/plugins/'+encodeURIComponent(name)+'/'+action,{method:'POST'});
        toast((r&&r.message)||(action+' succeeded'),'ok'); manifests=null;
        await loadPluginsData(); renderPlugins(); renderNavCounts(); renderOverview();
      } catch(e){ if(!isAuthErr(e)) toast(action[0].toUpperCase()+action.slice(1)+' failed: '+e.message,'err'); }
      finally { busy(btn,false); }
    }
    $('watch').addEventListener('change', async e=>{
      const t=e.target; t.disabled=true;
      try { await api('/api/plugins/_watch',{method:'PUT', json:{enabled:t.checked}});
        toast('Directory watcher '+(t.checked?'enabled':'disabled'),'ok');
      } catch(err){ if(!isAuthErr(err)) toast('Watcher toggle failed: '+err.message,'err'); t.checked=!t.checked; }
      finally { t.disabled=false; }
    });

    function openPluginDrawer(name){
      const p=((state.plugins&&state.plugins.plugins)||[]).find(x=>x.name===name); if(!p) return;
      $('drTitle').innerHTML=esc(p.name)+' '+statusBadge(p.status);
      $('drSub').textContent=p.description||'';
      const params=p.parameters||[];
      $('drBody').innerHTML = `<div class="kv">
          <div class="k">Version</div><div class="v mono">${esc(p.version||'—')}</div>
          <div class="k">Author</div><div class="v">${esc(p.author||'—')}</div>
          <div class="k">Source</div><div class="v">${p.source==='uploaded'?'Uploaded'+(p.overrides?` <span class="muted">· overrides the bundled ${esc(p.overrides.version)}</span>`:''):'Bundled'}</div>
          <div class="k">Library</div><div class="v mono wrap-any">${esc(p.library_path||'—')}</div>
          ${p.provenance?`<div class="k">Installed from</div><div class="v">upload by ${esc(p.provenance.uploaded_by||'?')}, installed by ${esc(p.provenance.installed_by||'?')} ${timeCell(p.provenance.installed_at)} · key <code>${esc(p.provenance.signer||'?')}</code><div class="hash">${esc(p.provenance.sha256||'')}</div></div>`:''}
          <div class="k">Dependencies</div><div class="v">${(p.dependencies||[]).map(x=>`<span class="tag mono">${esc(x)}</span>`).join('')||'—'}</div>
          <div class="k">Tags</div><div class="v">${(p.tags||[]).map(x=>`<span class="tag">${esc(x)}</span>`).join('')||'—'}</div>
        </div>
        <div class="sec-lbl">Parameters</div>`+
        (params.length ? `<div class="card"><table class="tbl"><thead><tr><th>Name</th><th>Type</th><th>Default</th></tr></thead><tbody>${params.map(x=>`
          <tr><td><div class="mono pname">${esc(x.name)}${x.required?' <span style="color:var(--err)" title="Required">*</span>':''}</div>${x.description?`<div class="pdesc">${esc(x.description)}</div>`:''}</td>
          <td class="mono muted">${esc(x.type||'string')}</td><td class="mono muted">${x.default!=null?esc(x.default):'—'}</td></tr>`).join('')}</tbody></table></div>`
          : `<div class="muted">This plugin does not declare a parameter manifest.</div>`);
      openOverlay($('drawer'), '[data-close].btn');
    }

    // ================= Plugin uploads =================
    // Upload -> stage (server checks format, arch, manifest and signature
    // without loading anything) -> review -> install. Installing runs the
    // plugin's code in the server, so the review says so; uploads are off
    // unless the server enables them (state.uploads.enabled/reason).
    const up = { files:{library:null, signature:null, manifest:null}, staged:null, err:'', note:'', sending:false, statusErr:'' };
    function fmtBytes(n){
      n=Number(n)||0; if(n<1024) return n+' B';
      const u=['KB','MB','GB']; let i=-1; do { n/=1024; i++; } while(n>=1024 && i<u.length-1);
      return (n<10?n.toFixed(1):Math.round(n))+' '+u[i];
    }
    const upExt = () => (state.uploads && state.uploads.platform && state.uploads.platform.extension) || '.so';
    const upVersion = s => s.version || 'unversioned';
    // What installing it does, relative to the installed version.
    const upChange = s => (({
      new:'New plugin', upgrade:`Upgrade from ${s.replaces_version}`, downgrade:`Downgrade from ${s.replaces_version}`,
      reinstall:`Reinstall of ${s.replaces_version}`, replace:`Replaces ${s.replaces_version||'the installed version'}`
    })[s.change] || 'New plugin') + (s.replaces_bundled && s.change!=='new' ? ' (bundled)' : '');
    const upAction = s => ({ upgrade:'Upgrade to '+upVersion(s), downgrade:'Downgrade to '+upVersion(s), reinstall:'Reinstall', replace:'Replace' })[s.change] || 'Install';

    function renderStagedUploads(focusAfter){
      const host=$('stagedUploads'), list=(state.uploads&&state.uploads.uploads)||[];
      if(state.uploadsErr){
        host.innerHTML=`<div class="alert err mid stack" role="alert">${icon('alert')}<span>Could not load staged plugin uploads: ${esc(state.uploadsErr)}</span>
          <button type="button" class="btn sm" data-up="retry">${icon('refresh','ico')}${icon('loader','spin')}Retry</button></div>`;
        return;
      }
      if(!list.length){ host.innerHTML=''; if(focusAfter) $('uploadPluginBtn').focus(); return; }
      host.innerHTML = `<div class="card stack"><div class="card-hd"><h2>Waiting for install</h2><span class="muted num">${list.length}</span></div>
        <div class="table-wrap"><table class="tbl"><thead><tr><th>Plugin</th><th class="hide-sm">Size</th><th class="hide-sm">Signed by</th><th class="hide-sm">Uploaded</th><th class="r">Actions</th></tr></thead><tbody>${list.map(u=>`
          <tr><td><div class="pname">${esc(u.name)} <span class="mono muted">${esc(upVersion(u))}</span></div>
            <div class="pdesc">${esc(upChange(u))}${u.install_crashed_at?' · <span class="err-text">crashed the server</span>':''}</div></td>
          <td class="muted num nowrap hide-sm">${esc(fmtBytes(u.size))}</td><td class="mono muted hide-sm">${esc(u.signer)}</td>
          <td class="muted nowrap hide-sm">${esc(u.uploaded_by)} · ${timeCell(u.uploaded_at)}</td>
          <td><div class="acts"><button class="btn sm primary" data-up="review" data-id="${esc(u.id)}">Review</button>
            <button class="btn sm danger" data-up="discard" data-id="${esc(u.id)}" aria-label="Discard ${esc(u.name)} ${esc(upVersion(u))}">${icon('trash','ico')}${icon('loader','spin')}<span class="sr-sm">Discard</span></button></div></td></tr>`).join('')}
        </tbody></table></div></div>`;
      if(focusAfter){ const b=host.querySelector('[data-up="review"]'); (b||$('uploadPluginBtn')).focus(); }
    }
    function openUpload(staged){
      up.files={library:null, signature:null, manifest:null}; up.staged=staged||null; up.err=''; up.note=''; up.statusErr='';
      // A mouse click does not focus a button in every browser: fall back to
      // the "Upload plugin" button so focus never returns to <body>.
      const act=document.activeElement;
      const opener = act && act!==document.body && act.closest('button,a,[tabindex]') ? act : $('uploadPluginBtn');
      paintUpload();
      openOverlay($('upload'), '#upInstall,#upPick,#upRetry,#upFt [data-close]');
      $('upload')._returnFocus = opener;   // closeOverlay returns focus here
    }
    const upFocusables = () => $$('#upload button:not([disabled]), #upload [tabindex]:not([tabindex="-1"])').filter(x=>x.offsetParent!==null);
    // Re-renders the dialog and keeps focus inside it: on the same control if
    // it still exists, else on @p focusSel, else on the first control.
    function renderUpload(focusSel){
      const dlg=$('upload'), act=document.activeElement;
      const keep = act && dlg.contains(act) ? (act.id ? '#'+act.id : act.dataset.upx ? `[data-upx="${act.dataset.upx}"]` : null) : null;
      paintUpload();
      if(!dlg.classList.contains('show')) return;
      if(!focusSel && dlg.contains(document.activeElement)) return;   // focus survived the re-render
      const target = (focusSel && dlg.querySelector(focusSel)) || (keep && dlg.querySelector(keep)) ||
                     dlg.querySelector('#upInstall,#upPick,#upRetry') || upFocusables()[0];
      if(target) target.focus();
    }
    function paintUpload(){
      const u=state.uploads, body=$('upBody'), ft=$('upFt');
      const err = up.err ? `<div class="alert err" role="alert">${icon('alert')}<span>${esc(up.err)}</span></div>` : '';
      if(up.staged){
        const s=up.staged, replacing=s.replaces_version!=null;
        $('upTitle').textContent='Review plugin';
        body.innerHTML = err + (s.install_crashed_at ? `<div class="alert err">${icon('alert')}<span>The server stopped while installing this upload (${timeCell(s.install_crashed_at)}), and the install was rolled back. Installing it again may stop the server again.</span></div>` : '') +
          `<div class="kv">
            <div class="k">Plugin</div><div class="v"><strong>${esc(s.name)}</strong> <span class="tag">${esc(upChange(s).toLowerCase())}</span></div>
            <div class="k">Version</div><div class="v mono">${esc(upVersion(s))}</div>
            ${s.description?`<div class="k">Description</div><div class="v">${esc(s.description)}</div>`:''}
            <div class="k">Library</div><div class="v"><span class="mono wrap-any">${esc(s.library_filename)}</span> <span class="muted">· ${esc(fmtBytes(s.size))}</span></div>
            <div class="k">SHA-256</div><div class="v hash">${esc(s.sha256)}</div>
            <div class="k">Platform</div><div class="v">${esc(s.format)} ${esc(s.arch)} <span class="ok-text">· loads on this server</span></div>
            <div class="k">Signature</div><div class="v"><span class="ok-text">${icon('check')} Verified</span> · key <code>${esc(s.signer)}</code> <span class="muted">(covers the file name and manifest)</span>${s.allowed_by?`<div class="muted">This key may sign: ${esc(s.allowed_by)}</div>`:''}</div>
            <div class="k">Manifest</div><div class="v">${s.has_manifest?`${esc(String(s.parameters))} parameter${s.parameters===1?'':'s'} declared`:'None: the job editor will show raw JSON parameters'}</div>
            <div class="k">Uploaded</div><div class="v">${esc(s.uploaded_by)} · ${timeCell(s.uploaded_at)}</div>
          </div>
          ${s.change==='downgrade'?`<div class="alert">${icon('warn')}<span>${esc(upVersion(s))} is older than the installed ${esc(s.replaces_version)}.</span></div>
          <label class="confirm-row"><input type="checkbox" id="upConfirm" /> Install ${esc(upVersion(s))} even though ${esc(s.replaces_version)} is installed</label>`:''}
          ${s.replaces_bundled&&replacing?`<div class="alert info">${icon('info')}<span>This overrides the bundled ${esc(s.replaces_version)}. The bundled files are not changed.</span></div>`:''}
          <div class="alert flush">${icon('warn')}<span>Installing loads this library into the Orcha server: its code runs with the server’s permissions. Install only plugins you trust.${replacing?` If ${esc(upVersion(s))} fails to load, ${esc(s.replaces_version)} is restored.`:''}</span></div>`;
        ft.innerHTML = `<button class="btn danger" id="upDiscard" aria-label="Discard upload">${icon('trash','ico')}${icon('loader','spin')}<span class="sr-sm">Discard</span></button><span class="sp"></span>
          <button class="btn" data-close>Keep for later</button>
          <button class="btn primary" id="upInstall" ${s.change==='downgrade'?'disabled':''}>${icon('loader','spin')}<span>${esc(upAction(s))}${s.install_crashed_at?' anyway':''}</span></button>`;
      } else if(up.statusErr){
        $('upTitle').textContent='Upload plugin';
        body.innerHTML = `<div class="alert err flush" role="alert">${icon('alert')}<span>Could not check whether this server accepts plugin uploads: ${esc(up.statusErr)}</span></div>`;
        ft.innerHTML = `<span class="sp"></span><button class="btn" data-close>Close</button><button class="btn primary" id="upRetry">${icon('refresh','ico')}${icon('loader','spin')}Retry</button>`;
      } else if(!u || !u.enabled){
        $('upTitle').textContent='Upload plugin';
        const off = u && String(u.reason||'').startsWith('Plugin uploads are off');
        body.innerHTML = `<div class="alert info${off?'':' flush'}">${icon('info')}<span>${u?esc(u.reason||'Plugin uploads are not available.'):'Checking whether this server accepts plugin uploads…'}</span></div>` + (off ? `
          <p class="muted tight">Uploads are off by default because an installed plugin runs code inside the server. To allow signed uploads, set in <code>orcha.yaml</code>:</p>
          <pre class="snippet">plugins:
  upload:
    enabled: true
    public_keys: ["&lt;key from scripts/orcha-plugin-sign.sh keygen&gt;"]</pre>` : '');
        ft.innerHTML = `<span class="sp"></span><button class="btn" data-close>Close</button>`;
      } else {
        $('upTitle').textContent='Upload plugin';
        const p=u.platform||{}, ext=upExt(), f=up.files;
        const row=(role, label, file, note)=>`<li><span class="role">${label}</span>${file
          ? `<span class="fname">${esc(file.name)}</span><span class="size">${esc(fmtBytes(file.size))}</span>
             <button class="btn ghost sm icon" data-upx="${role}" aria-label="Remove ${label.toLowerCase()} ${esc(file.name)}">${icon('x')}</button>`
          : `<span class="fname missing">${note}</span>`}</li>`;
        body.innerHTML = err + `<p class="muted tight">Signed plugins only. This server loads <code>${esc(ext)}</code> libraries for ${esc(p.format||'')} ${esc(p.arch||'')} and trusts ${(u.keys||[]).map(k=>`<code>${esc(k.id||k)}</code>${k.plugins?` <span class="muted">(${esc(k.plugins.join(', '))})</span>`:''}`).join(', ')||'no keys'}. Upload the library under the file name it was signed with.</p>
          <div class="dropzone" id="upDrop">${icon('upload')}
            <div><strong>Drop files here</strong> or <button type="button" class="linkbtn" id="upPick">choose files</button></div>
            <div class="hint">The library, its <code>.sig</code> from <code>orcha-plugin-sign.sh</code>, and optionally <code>manifest.json</code>. Up to ${esc(fmtBytes(u.max_bytes))}.</div></div>
          <ul class="upfiles">${row('library','Library',f.library,'Required: a '+esc(ext)+' file')}${row('signature','Signature',f.signature,'Required: the .sig file')}${row('manifest','Manifest',f.manifest,'Optional: name, version and parameters')}</ul>
          <div class="sr-only" aria-live="polite" id="upLive">${esc(up.note)}</div>`;
        ft.innerHTML = `<span class="sp"></span><button class="btn" data-close>Cancel</button>
          <button class="btn primary${up.sending?' loading':''}" id="upSend" ${f.library&&f.signature&&!up.sending?'':'disabled'}>${icon('loader','spin')}<span>Upload for review</span></button>`;
      }
      $$('[data-close]', ft).forEach(b=>b.addEventListener('click', ()=>closeOverlay($('upload'))));
    }
    function addUploadFiles(fileList){
      const ext=upExt(), max=(state.uploads&&state.uploads.max_bytes)||Infinity, problems=[], added=[];
      up.err='';
      Array.from(fileList||[]).forEach(file=>{
        const n=file.name.toLowerCase();
        const role = n.endsWith('.sig') ? 'signature' : n.endsWith('.json') ? 'manifest' : n.endsWith(ext) ? 'library' : null;
        if(!role){ problems.push(`${file.name} is not a ${ext} library, .sig or manifest.json`); return; }
        if(role==='library' && file.size>max){ problems.push(`${file.name} is larger than ${fmtBytes(max)}`); return; }
        if(role!=='library' && file.size>1024*1024){ problems.push(`${file.name} is too large for a ${role} file`); return; }
        up.files[role]=file; added.push(`${file.name} (${role})`);
      });
      up.err = problems.join('. ') + (problems.length ? '.' : '');
      const f=up.files;
      up.note = (added.length ? 'Added '+added.join(', ')+'. ' : '') + (f.library&&f.signature ? 'Ready to upload.' : 'Still needed: '+[!f.library&&'library', !f.signature&&'signature'].filter(Boolean).join(' and ')+'.');
      renderUpload(f.library&&f.signature ? '#upSend' : '#upPick');
    }
    const readBase64 = file => new Promise((resolve, reject)=>{
      const r=new FileReader();
      r.onload=()=>{ const s=String(r.result); resolve(s.slice(s.indexOf(',')+1)); };
      r.onerror=()=>reject(r.error||new Error('Could not read '+file.name));
      r.readAsDataURL(file);
    });
    // Errors after the dialog was closed must not vanish.
    const upFail = (msg) => { if($('upload').classList.contains('show')){ up.err=msg; renderUpload(); } else toast(msg,'err'); };
    async function sendUpload(){
      const f=up.files; if(!f.library||!f.signature||up.sending) return;
      up.sending=true; up.err=''; renderUpload();
      try {
        const [data, signature, manifest] = await Promise.all([readBase64(f.library), readBase64(f.signature), f.manifest?readBase64(f.manifest):null]);
        const staged = await api('/api/plugins/_uploads', {method:'POST', json:{library:{filename:f.library.name, data}, signature, manifest}});
        up.sending=false;
        await loadUploadsData().catch(()=>{}); renderStagedUploads();
        if($('upload').classList.contains('show')){
          up.staged=staged; up.files={library:null, signature:null, manifest:null}; renderUpload('#upInstall');
        } else toast(`Uploaded ${staged.name} ${upVersion(staged)}: it is waiting for install`, 'ok');
      } catch(e){
        up.sending=false;
        if(isAuthErr(e)) return;
        upFail(e.status===413 ? 'The upload is larger than the server accepts.' : 'Upload failed: '+e.message);
      }
    }
    async function installStaged(btn){
      const s=up.staged; if(!s || btn.disabled) return;
      busy(btn,true); up.err='';
      try {
        const confirmed = !!($('upConfirm') && $('upConfirm').checked);
        const r=await api('/api/plugins/_uploads/'+encodeURIComponent(s.id)+'/install', {method:'POST', json:{allow_downgrade:confirmed}});
        closeOverlay($('upload'));
        toast(r.previous_version!=null ? `${r.name}: ${r.previous_version} → ${r.version||'unversioned'}` : `Installed ${r.name} ${r.version||''}`.trim(), 'ok');
        manifests=null;
        await Promise.allSettled([loadPluginsData(), loadUploadsData()]);
        renderPlugins(); renderStagedUploads(); renderNavCounts(); renderOverview();
      } catch(e){
        if(isAuthErr(e)) return;
        await Promise.allSettled([loadPluginsData(), loadUploadsData()]); renderPlugins(); renderStagedUploads();
        // It became a downgrade since it was staged: show it as one (with the checkbox).
        if(e.body && e.body.requires==='allow_downgrade' && $('upload').classList.contains('show')){
          const fresh=((state.uploads&&state.uploads.uploads)||[]).find(x=>x.id===s.id);
          if(fresh) up.staged=fresh;
          up.err=e.message; renderUpload('#upConfirm'); return;
        }
        upFail('Install failed: '+e.message);
      } finally { const b=$('upInstall'); if(b) busy(b,false); }
    }
    async function discardStaged(id, btn){
      const s=((state.uploads&&state.uploads.uploads)||[]).find(x=>x.id===id) || up.staged;
      const label = s ? s.name+' '+upVersion(s) : 'The upload';
      const ok=await confirmDialog('Discard this upload?', `${label} is deleted from the staging area. Nothing is installed.`, 'Discard');
      if(!ok) return;
      if(btn) busy(btn,true);
      try {
        await api('/api/plugins/_uploads/'+encodeURIComponent(id), {method:'DELETE'});
        toast('Discarded '+label, 'ok');
        const fromDialog = $('upload').classList.contains('show') && up.staged && up.staged.id===id;
        if(fromDialog) closeOverlay($('upload'));
        await loadUploadsData().catch(()=>{}); renderStagedUploads(!fromDialog);
      } catch(e){ if(!isAuthErr(e)) toast('Discard failed: '+e.message,'err'); }
      finally { if(btn && btn.isConnected) busy(btn,false); }
    }
    async function refreshUploadStatus(){
      try { await loadUploadsData(); up.statusErr=''; }
      catch(e){ if(isAuthErr(e)) return; up.statusErr=e.message||'request failed'; }
      if($('upload').classList.contains('show') && !up.staged) renderUpload();
    }
    $('uploadPluginBtn').addEventListener('click', ()=>{ openUpload(null); refreshUploadStatus(); });
    $('stagedUploads').addEventListener('click', async e=>{
      const b=e.target.closest('[data-up]'); if(!b) return;
      if(b.dataset.up==='retry'){ busy(b,true); await loadUploadsData().catch(()=>{}); renderStagedUploads(); return; }
      const s=((state.uploads&&state.uploads.uploads)||[]).find(x=>x.id===b.dataset.id); if(!s) return;
      if(b.dataset.up==='review') openUpload(s); else discardStaged(s.id, b);
    });
    $('upload').addEventListener('click', e=>{
      const t=e.target;
      if(t.closest('#upPick')){ $('upFile').accept=[upExt(),'.sig','.json'].join(','); $('upFile').click(); return; }
      const x=t.closest('[data-upx]');
      if(x){ const f=up.files[x.dataset.upx]; up.files[x.dataset.upx]=null; up.err=''; up.note=f?`Removed ${f.name}.`:''; renderUpload('#upPick'); return; }
      if(t.closest('#upSend')){ sendUpload(); return; }
      if(t.closest('#upRetry')){ up.statusErr=''; renderUpload(); refreshUploadStatus(); return; }
      const inst=t.closest('#upInstall'); if(inst){ installStaged(inst); return; }
      if(t.closest('#upConfirm')){ $('upInstall').disabled = !$('upConfirm').checked; return; }
      const dis=t.closest('#upDiscard'); if(dis && up.staged){ discardStaged(up.staged.id, dis); }
    });
    $('upFile').addEventListener('change', e=>{ addUploadFiles(e.target.files); e.target.value=''; });
    // Dropping files: on the dialog's drop zone, or anywhere on the Plugins page.
    let upDragDepth=0;
    const upHasFiles = e => Array.from((e.dataTransfer&&e.dataTransfer.types)||[]).includes('Files');
    $('upload').addEventListener('dragenter', e=>{ const z=$('upDrop'); if(!z||!upHasFiles(e)) return; e.preventDefault(); upDragDepth++; z.classList.add('dropping'); });
    $('upload').addEventListener('dragover', e=>{ if(upHasFiles(e)) e.preventDefault(); });
    $('upload').addEventListener('dragleave', e=>{ const z=$('upDrop'); if(!z||!upHasFiles(e)) return; if(--upDragDepth<=0){ upDragDepth=0; z.classList.remove('dropping'); } });
    $('upload').addEventListener('drop', e=>{
      if(!upHasFiles(e)) return; e.preventDefault(); e.stopPropagation(); upDragDepth=0;
      const z=$('upDrop'); if(!z) return;
      z.classList.remove('dropping'); addUploadFiles(e.dataTransfer.files);
    });
    $('view-plugins').addEventListener('dragover', e=>{ if(upHasFiles(e)) e.preventDefault(); });
    $('view-plugins').addEventListener('drop', e=>{
      if(!upHasFiles(e) || $('upload').classList.contains('show')) return;
      e.preventDefault();
      const files=e.dataTransfer.files;
      openUpload(null);
      if(state.uploads && state.uploads.enabled) addUploadFiles(files);
      else refreshUploadStatus().then(()=>{ if(state.uploads && state.uploads.enabled) addUploadFiles(files); });
    });
    $('upload').addEventListener('mousedown', e => { if(e.target===$('upload')) closeOverlay($('upload')); });

    // ================= Circuit breakers =================
    const BREAKER_STATES = { closed:'closed', open:'open', half_open:'half-open' };
    const breakerLabel = (st) => BREAKER_STATES[st] || String(st||'unknown');
    const breakerBadge = (st) => `<span class="badge ${BREAKER_STATES[st]?st:'unknown'}">${esc(breakerLabel(st))}</span>`;

    function renderBreakers(){
      const host=$('breakerRows'); if(!host) return;
      const list=state.breakers;
      if(state.breakersErr){
        host.innerHTML=`<tr><td colspan="8"><div class="alert err mid" role="alert">${icon('alert')}<span>Could not load circuit breakers: ${esc(state.breakersErr)}</span>
          <button type="button" class="btn sm" data-act="retry-breakers">${icon('refresh','ico')}${icon('loader','spin')}Retry</button></div></td></tr>`;
        return;
      }
      if(!list){ host.innerHTML=skeletonRows(8,3); return; }
      if(!list.length){
        host.innerHTML=`<tr><td colspan="8">${emptyState('shield','No circuit breakers yet','A breaker appears once a workflow with "circuit_breaker": true runs a step using that command.')}</td></tr>`;
        return;
      }
      const order={open:0, half_open:1, closed:2};
      const rows=[...list].sort((a,b)=>(order[a.state]??3)-(order[b.state]??3) || String(a.command).localeCompare(String(b.command)));
      host.innerHTML = rows.map(b=>{
        const n=esc(b.command);
        const act = b.state!=='closed'
          ? `<button class="btn sm" data-br="${n}" title="Close the circuit so steps run again" aria-label="Reset circuit for ${n}">${icon('refresh','ico')}${icon('loader','spin')}<span class="sr-sm">Reset</span></button>`
          : '<span class="faint">—</span>';
        return `<tr><td><div class="pname mono wrap">${n}</div></td><td>${breakerBadge(b.state)}</td>
          <td class="r num hide-sm" title="Consecutive failures">${fmtNum(b.consecutive_failures)}</td><td class="r num muted hide-sm">${fmtNum(b.total_calls)}</td>
          <td class="r num muted hide-sm">${fmtNum(b.failed_calls)}</td><td class="r num muted hide-sm">${fmtNum(b.rejected_calls)}</td>
          <td class="nowrap muted hide-sm">${b.last_failure?timeCell(b.last_failure):'—'}</td><td><div class="acts">${act}</div></td></tr>`;
      }).join('');
    }
    // Overview notice: open circuits (error), only recovering ones (warning),
    // or breaker status unavailable.
    function breakerNotice(){
      if(state.breakersErr) return `<div class="alert" role="status">${icon('warn')}<span>Circuit breaker status is unavailable (${esc(state.breakersErr)}). <a href="#/breakers">View breakers →</a></span></div>`;
      const tripped=trippedBreakers(); if(!tripped.length) return '';
      const open=tripped.filter(b=>b.state==='open');
      const names=tripped.slice(0,5).map(b=>`<span class="mono">${esc(b.command)}</span> (${esc(breakerLabel(b.state))})`).join(', ')+(tripped.length>5?', …':'');
      const n=tripped.length, plural=n===1?'':'s';
      const detail = open.length
        ? 'Steps using an open circuit fail fast in workflows that opt in.'
        : 'Each lets a single recovery probe through; the circuit closes if it succeeds.';
      return `<div class="alert${open.length?' err':''}" role="status">${icon('shield')}<span>
          ${n} circuit breaker${plural} ${open.length?'not closed':'recovering'}: ${names}. ${detail} <a href="#/breakers">View breakers →</a></span></div>`;
    }

    async function retryBreakers(btn){
      busy(btn,true);
      try { await loadBreakersData(); } catch(e){ /* state.breakersErr is set */ }
      finally { busy(btn,false); renderBreakers(); renderNavCounts(); renderOverview(); }
    }
    async function resetBreaker(btn, command){
      const ok=await confirmDialog('Reset circuit for '+command+'?',
        'Steps using this command will run again immediately. If it is still failing, the circuit opens again after the configured number of failures.',
        'Reset circuit', {tone:'primary'});
      if(!ok) return;
      busy(btn,true);
      try { await api('/api/circuit-breakers/'+encodeURIComponent(command)+'/reset',{method:'POST'});
        toast('Circuit for '+command+' reset','ok');
        await loadBreakersData(); renderBreakers(); renderNavCounts(); renderOverview();
      } catch(e){ if(!isAuthErr(e)) toast('Reset failed: '+e.message,'err'); }
      finally { busy(btn,false); }
    }

    // ================= Jobs =================
    const runDot = (st) => st==='success' ? 'ok' : st==='failed' ? 'err' : st==='interrupted' ? 'warn'
                         : isLive(st) ? 'live' : 'muted';
    function renderJobList(){
      if($('jobsAlerts')) $('jobsAlerts').innerHTML=storeNotice();
      const q=state.jobQuery, last=lastRunByJob();
      const list=state.jobs.filter(j=>!q || (j.name+' '+(j.description||'')).toLowerCase().includes(q));
      const host=$('jobItems');
      if(!state.jobs.length){ host.innerHTML=emptyState('flow','No jobs yet','Create a job to save a workflow, run it on demand or on a schedule.',
          `<button class="btn primary sm" data-act="new">${icon('plus')}New job</button>`); return; }
      if(!list.length){ host.innerHTML=emptyState('search','No matching jobs','Try a different search.'); return; }
      host.innerHTML = list.map(j=>{
        const lr=last[j.id];
        const dot = !j.enabled ? '<span class="dot off" title="Disabled"></span>'
                  : lr ? `<span class="dot ${runDot(lr.status)}" title="Last run ${esc(lr.status)}"></span>` : '<span class="dot" title="Never run"></span>';
        const active=state.currentJob&&state.currentJob.id===j.id;
        return `<button type="button" role="option" aria-selected="${active}" class="jitem ${active?'active':''}" data-job="${esc(j.id)}">
          <div class="l1">${dot}<span class="nm">${esc(j.name)}</span>${j.schedule_cron?`<span class="cron" title="Cron (UTC)">${esc(j.schedule_cron)}</span>`:''}</div>
          <div class="l2"><span class="desc">${esc(j.description || (j.enabled?'':'Disabled'))}</span>${lr?`<span class="nowrap">${esc(fmtAgo(runTime(lr)))}</span>`:''}</div></button>`;
      }).join('');
    }
    $('jobSearch').addEventListener('input', e=>{ state.jobQuery=e.target.value.trim().toLowerCase(); renderJobList(); });

    async function selectJob(id){
      try {
        state.currentJob = await api('/api/jobs/'+encodeURIComponent(id));
        state.selectedRunId=null; state.jobRuns=[];
        renderJobList(); renderJobDetail(); renderCrumbs();
      } catch(e){ if(!isAuthErr(e)){ toast('Could not open job: '+e.message,'err'); location.hash='#/jobs'; } }
    }
    async function refreshCurrentJob(){
      try { state.currentJob = await api('/api/jobs/'+encodeURIComponent(state.currentJob.id)); renderJobHeader(); loadRuns(state.currentJob.id); }
      catch(e){ /* job may have been deleted elsewhere */ }
    }

    function cronDescribe(expr){
      const opt=Array.from($('cronPreset').options).find(o=>o.value&&o.value===expr);
      return opt ? opt.textContent : 'Custom schedule';
    }

    function renderJobDetail(){
      const j=state.currentJob, host=$('jobDetail');
      if(!j){
        host.innerHTML = emptyState('flow', state.jobs.length?'Select a job':'No job selected',
          state.jobs.length?'Choose a job from the list to see its workflow, latest output and run history.':'Create your first job to get started.');
        flow.clear(); return;
      }
      const steps=(j.definition&&j.definition.steps)||[];
      host.innerHTML = `
        <div class="detail-hd" id="jobHeader"></div>
        <div class="facts" id="jobFacts"></div>
        <div class="section">
          <div class="section-hd"><h3>Workflow</h3><span class="muted">${steps.length} step${steps.length===1?'':'s'}</span><span class="sp"></span>
            <span class="faint flow-hint" style="font-size:12px">Drag to rearrange · click a step to jump to its output</span></div>
          ${steps.length ? '<div class="chartwrap"><canvas id="flow" aria-label="Workflow diagram"></canvas><div id="fctip"></div></div>'
                         : '<div class="empty-hint">This job has no steps. Edit it to add some.</div>'}
        </div>
        <div class="section"><div class="section-hd"><h3>Run output</h3></div><div id="runOutput"></div></div>
        <div class="section"><div class="section-hd"><h3>Run history</h3><span class="muted" id="runsCount"></span></div><div id="runs"></div></div>`;
      renderJobHeader();
      $('runOutput').innerHTML = '<span class="skel" style="width:60%"></span>';
      $('runs').innerHTML = '<span class="skel" style="width:40%"></span>';
      if(steps.length) flow.setJob(j); else flow.clear();
      loadRuns(j.id);
    }

    const ROLLBACK_LABELS = { reverse:'reverse, stop on error', best_effort:'best effort' };
    function fmtMs(ms){ const n=Number(ms); return n>=1000 && n%1000===0 ? (n/1000)+' s' : n+' ms'; }
    // Workflow-level settings shown under the job title.
    function jobChips(def){
      if(!def || typeof def!=='object') return '';
      const chips=[];
      if(ROLLBACK_LABELS[def.rollback]) chips.push(`<span class="tag" title="Completed steps are rolled back when a run fails, times out or is cancelled">${icon('undo')}Rollback: ${esc(ROLLBACK_LABELS[def.rollback])}</span>`);
      if(def.circuit_breaker===true) chips.push(`<span class="tag" title="Steps fail fast while their command's circuit is open">${icon('shield')}Circuit breaker</span>`);
      if(Number(def.timeout_ms)>0) chips.push(`<span class="tag" title="Time limit for the whole run">${icon('clock')}Timeout ${esc(fmtMs(def.timeout_ms))}</span>`);
      return chips.length ? `<div class="chips">${chips.join('')}</div>` : '';
    }

    function renderJobHeader(){
      const j=state.currentJob; if(!j || !$('jobHeader')) return;
      $('jobHeader').innerHTML = `<div class="row">
          <div style="min-width:0"><h2>${esc(j.name)} ${j.enabled?statusBadge('enabled'):statusBadge('disabled')}</h2>
            ${j.description?`<p>${esc(j.description)}</p>`:''}${jobChips(j.definition)}</div>
          <div class="acts">
            <button class="btn primary" id="runJob">${icon('play','ico')}${icon('loader','spin')}Run now</button>
            <button class="btn" id="editJob">${icon('edit')}Edit</button>
            <div class="dd" id="jobMenu"><button class="btn icon" aria-haspopup="menu" aria-expanded="false" aria-label="More actions" id="jobMenuBtn">${icon('more')}</button>
              <div class="dd-menu" role="menu">
                <button role="menuitem" id="toggleJob">${icon('power')}${j.enabled?'Disable job':'Enable job'}</button>
                <hr/><button role="menuitem" class="danger" id="delJob">${icon('trash')}Delete job</button>
              </div></div>
          </div></div>`;
      let next='—';
      if(j.schedule_cron && j.enabled){ try { const n=cronNext(j.schedule_cron,1)[0]; if(n) next=`<span title="${esc(n.toLocaleString())}">${esc(fmtAgo(n.toISOString()))}</span>`; } catch(e){ next='<span style="color:var(--err)">invalid cron</span>'; } }
      else if(j.schedule_cron) next='<span class="muted">paused</span>';
      const lr=state.jobRuns[0];
      $('jobFacts').innerHTML = `
        <div><div class="k">Schedule</div><div class="v">${j.schedule_cron?`<span class="mono">${esc(j.schedule_cron)}</span> <span class="muted">· ${esc(cronDescribe(j.schedule_cron))}</span>`:'<span class="muted">Manual only</span>'}</div></div>
        <div><div class="k">Next run</div><div class="v">${next}</div></div>
        <div><div class="k">Last run</div><div class="v">${lr?`${statusBadge(lr.status)} <span class="muted">${timeCell(runTime(lr))}</span>`:'<span class="muted">Never</span>'}</div></div>
        <div><div class="k">Updated</div><div class="v">${j.updated_at?timeCell(j.updated_at):'—'}</div></div>`;
      $('runJob').onclick = (e)=>runJob(e.currentTarget, j.id);
      $('editJob').onclick = ()=>openEditor(j);
      $('jobMenuBtn').onclick = (e)=>{ e.stopPropagation(); const m=$('jobMenu'); const o=!m.classList.contains('open'); m.classList.toggle('open',o); e.currentTarget.setAttribute('aria-expanded',o); };
      $('toggleJob').onclick = ()=>{ $('jobMenu').classList.remove('open'); toggleJob(j); };
      $('delJob').onclick = ()=>{ $('jobMenu').classList.remove('open'); delJob(j); };
    }
    document.addEventListener('click', e=>{ if(!e.target.closest('.dd')) $$('.dd.open').forEach(d=>d.classList.remove('open')); });

    async function toggleJob(j){
      if(j.enabled && j.schedule_cron){
        const ok=await confirmDialog('Disable '+j.name+'?', 'The scheduler will stop running this job. You can still run it manually.', 'Disable job');
        if(!ok) return;
      }
      const payload={ name:j.name, description:j.description||'', definition:j.definition, enabled:!j.enabled };
      if(j.schedule_cron) payload.schedule_cron=j.schedule_cron;
      try {
        await api('/api/jobs/'+encodeURIComponent(j.id),{method:'PUT', json:payload});
        toast(j.name+(j.enabled?' disabled':' enabled'),'ok');
        await loadJobsData(); renderJobList(); renderNavCounts(); await refreshCurrentJob();
      } catch(e){ if(!isAuthErr(e)) toast('Update failed: '+e.message,'err'); }
    }

    function fmtOutput(o){ if(o===undefined) return ''; try { return JSON.stringify(o, null, 2); } catch(e){ return String(o); } }

    // [badge class, badge text, summary text]
    const ROLLBACK_STATUS = {
      rolled_back:  ['rb-done',    'rolled back',         'rolled back'],
      failed:       ['rb-failed',  'rollback failed',     'failed'],
      skipped:      ['rb-skipped', 'rollback skipped',    'skipped'],
      not_supported:['rb-na',      'no rollback support', 'not supported']
    };
    const rollbackInfo = (rb) => (rb && ROLLBACK_STATUS[rb.status]) || null;
    function rollbackBadge(rb){
      const m=rollbackInfo(rb); if(!m) return '';
      return `<span class="badge ${m[0]}" title="${esc(rb.message||'')}">${esc(m[1])}</span>`;
    }
    // Header summary of rollback outcomes. "not supported" is left to the
    // per-step badges: it is the norm while few commands implement rollback.
    function rollbackSummary(steps){
      const counts={};
      steps.forEach(s=>{ const st=s.rollback && s.rollback.status; if(ROLLBACK_STATUS[st] && st!=='not_supported') counts[st]=(counts[st]||0)+1; });
      const parts=Object.keys(counts).map(st=>{
        const txt=esc(counts[st]+' '+ROLLBACK_STATUS[st][2]);
        return st==='failed' ? `<span class="bad">${txt}</span>` : txt;
      });
      return parts.length ? `<span>·</span><span class="rbsum" title="Rollback outcome of completed steps">${icon('undo')}Rollback: ${parts.join(' · ')}</span>` : '';
    }
    const failedRollbacks = (steps) => steps.filter(s=>s.rollback && s.rollback.status==='failed').length;

    function renderRunOutput(run){
      const el=$('runOutput'); if(!el) return;
      el.dataset.run = run ? run.id : ''; el.dataset.status = run ? run.status : '';
      if(!run){ el.innerHTML=emptyState('play','No runs yet','Click “Run now” to execute this workflow and inspect each step’s output.'); return; }
      const steps=Array.isArray(run.result)?run.result:[];
      const head=`<div class="runhd">${statusBadge(run.status)}<span>Run <span class="mono">${esc(run.id.slice(0,8))}</span></span>
        <span>·</span><span class="badge plain ${esc(run.trigger||'unknown')}">${esc(run.trigger||'—')}</span><span>·</span>${timeCell(runTime(run))}<span>·</span><span class="num rundur">${fmtDur(run)}</span>${Array.isArray(run.result)?rollbackSummary(run.result):''}</div>`;
      if(isLive(run.status)){
        const msg = run.status==='queued' ? 'Queued — waiting for a free runner.' : 'Running — step output appears when the run finishes.';
        // A nested run executes inside its parent's run and is cancelled with it.
        const action = run.trigger==='nested'
          ? `<span class="muted">Runs inside its parent run; cancel the parent to stop it.</span>`
          : `<button type="button" class="btn sm${cancelling.has(run.id)?' loading':''}" data-act="cancel-run" data-run="${esc(run.id)}"${cancelling.has(run.id)?' disabled':''}>${icon('x','ico')}${icon('loader','spin')}Cancel run</button>`;
        el.innerHTML=head+`<div class="alert info mid" role="status">${icon('clock')}<span>${msg}</span>${action}</div>`;
        return;
      }
      // Cancelled / skipped are outcomes, not errors; interrupted is a warning.
      const errCls = run.status==='failed' ? 'err' : (run.status==='cancelled' || run.status==='skipped') ? 'info' : '';
      const errIcon = run.status==='failed' ? 'alert' : 'info';
      const runErr = run.error ? `<div class="alert ${errCls}">${icon(errIcon)}<span>${esc(run.error)}</span></div>` : '';
      if(!steps.length){
        el.innerHTML=head+(runErr||'<div class="muted">No step output recorded for this run.</div>');
        return;
      }
      const rbFailed=failedRollbacks(steps);
      const rbAlert = rbFailed ? `<div class="alert err">${icon('undo')}<span>Rollback failed for ${rbFailed} step${rbFailed===1?'':'s'}. The changes they made may still be in place; check them before re-running.</span></div>` : '';
      const defSteps=(state.currentJob && state.currentJob.definition && state.currentJob.definition.steps) || [];
      el.innerHTML=head+runErr+rbAlert+steps.map((s,i)=>{
        const rb=rollbackInfo(s.rollback);
        // A step that never started (the run stopped earlier) has no command, error or output.
        const notRun = !s.success && !s.error_message && !s.command && (s.output===null || s.output===undefined);
        if(notRun){
          const d=defSteps[i]||{};
          return `<details class="outcard" id="out-step-${i}"><summary>${icon('chev','chev')}<span class="idx">#${i+1}</span>
            <strong>${esc(d.name||d.command||'step')}</strong>${d.name?`<span class="mono muted">${esc(d.command||'')}</span>`:''}
            <span class="sbadges"><span class="badge skipped">not run</span></span></summary><div class="muted">This step did not start: the run ended before it.</div></details>`;
        }
        const ok=!!s.success, open = !ok || steps.length<=3 || (rb && s.rollback.status==='failed');
        const out=fmtOutput(s.output);
        // The badge carries the status; the line explains it (error / skip reason).
        const rbLine = (rb && s.rollback.message) ? `<div class="rb${s.rollback.status==='failed'?' err':''}">${icon('undo')}<span>${esc(rb[1][0].toUpperCase()+rb[1].slice(1))}: ${esc(s.rollback.message)}</span></div>` : '';
        const body=(!ok && s.error_message ? `<div class="err">${esc(s.error_message)}</div>` : '')+rbLine+(out?`<pre>${esc(out)}</pre>`:'<pre class="faint">(no output)</pre>');
        return `<details class="outcard" id="out-step-${i}"${open?' open':''}><summary>${icon('chev','chev')}<span class="idx">#${i+1}</span>
          <strong>${esc(s.name||s.command||'step')}</strong>${s.name?`<span class="mono muted">${esc(s.command||'')}</span>`:''}
          <span class="sbadges">${rollbackBadge(s.rollback)}${statusBadge(ok?'success':'failed')}</span></summary>${body}</details>`;
      }).join('');
    }

    // "Run now" queues a run (202); the page then follows it until it finishes.
    async function runJob(btn, id){
      busy(btn,true);
      try { const r=await api('/api/jobs/'+encodeURIComponent(id)+'/run',{method:'POST'});
        toast('Run queued','ok');
        state.selectedRunId=r.id; flow.applyRun(r); renderRunOutput(r);
        await loadRuns(id);
        loadRunsData().then(()=>{ renderJobList(); renderOverview(); }).catch(()=>{});
      } catch(e){
        if(isAuthErr(e)) return;
        if(e.status===429){
          const secs=e.retryAfter || (e.body && e.body.retry_after_seconds);
          toast('The run queue is full'+(secs?'; try again in '+secs+' s':'; try again shortly'),'err');
        } else toast('Could not queue the run: '+e.message,'err');
      }
      finally { busy(btn,false); }
    }

    // Follow a job's live runs: while its page shows a queued/running run, poll
    // the job's run list every second (one request covers every live run),
    // update the output, history and flow chart in place, and announce each run
    // that finishes. Paused while the tab is hidden; stops after 3 failures
    // with a Retry alert.
    let watchT=null, watchedJob=null, watchFails=0;
    const cancelling = new Set();   // cancel requested here; announce how it ended
    function watchJob(jobId){
      if(watchedJob===jobId) return;
      clearTimeout(watchT); watchedJob=jobId; watchFails=0;
      const tick = async () => {
        if(watchedJob!==jobId) return;
        if(!state.currentJob || state.currentJob.id!==jobId){ watchedJob=null; return; }   // navigated away
        if(document.hidden){ watchT=setTimeout(tick, 2000); return; }
        let d;
        try { d=await api('/api/jobs/'+encodeURIComponent(jobId)+'/runs?limit=20'); }
        catch(e){
          if(isAuthErr(e)){ watchedJob=null; return; }
          if(++watchFails>=3){ watchedJob=null; state.watchErr=e.message||'request failed'; renderRunHistory(); return; }
          watchT=setTimeout(tick, 3000); return;
        }
        if(watchedJob!==jobId || !state.currentJob || state.currentJob.id!==jobId) return;
        watchFails=0;
        const before=new Map(state.jobRuns.map(r=>[r.id, r.status]));
        state.jobRuns=d.runs||[];
        updateRunViews();
        const ended=state.jobRuns.filter(r=>isLive(before.get(r.id)) && !isLive(r.status));
        ended.forEach(r=>{
          const mine=cancelling.delete(r.id);
          if(r.status==='success') toast('Run succeeded','ok');
          else if(r.status==='cancelled') toast('Run cancelled', mine?'ok':'err');
          else toast('Run '+r.status+(r.error?': '+r.error:''),'err');
        });
        if(ended.length){
          renderJobHeader();
          loadRunsData().then(()=>{ renderJobList(); renderOverview(); renderNavCounts(); }).catch(()=>{});
        }
        if(state.jobRuns.some(r=>isLive(r.status))) watchT=setTimeout(tick, 1000);
        else watchedJob=null;
      };
      watchT=setTimeout(tick, 1000);
    }
    function watchIfLive(){
      if(state.currentJob && state.jobRuns.some(r=>isLive(r.status))) watchJob(state.currentJob.id);
    }
    // Re-render after a poll without destroying what the user is working with:
    // a live run that is still in the same state only gets its elapsed time.
    function updateRunViews(){
      const sel=state.jobRuns.find(r=>r.id===state.selectedRunId);
      const el=$('runOutput');
      if(sel && el){
        if(el.dataset.run===sel.id && el.dataset.status===sel.status && isLive(sel.status)){
          const d=el.querySelector('.rundur'); if(d) d.textContent=fmtDur(sel);
        } else { flow.applyRun(sel); renderRunOutput(sel); }
      }
      renderRunHistory();
    }

    async function cancelRun(btn, runId){
      const ok=await confirmDialog('Cancel this run?',
        'A queued run is removed and never starts. A running run stops at its next cancellation point; if the workflow has rollback enabled, the steps it already completed are rolled back.',
        'Cancel run', {cancelLabel:'Keep run'});
      if(!ok) return;
      cancelling.add(runId);
      // The button may have been re-rendered while the dialog was open.
      const live=$('runOutput') && $('runOutput').querySelector('button[data-act="cancel-run"][data-run="'+CSS.escape(runId)+'"]');
      busy(live||btn,true);
      try { const r=await api('/api/runs/'+encodeURIComponent(runId)+'/cancel',{method:'POST'});
        if(!isLive(r.status)){ cancelling.delete(runId); toast('Run cancelled','ok'); }
        else toast('Cancelling run…','ok');
        const i=state.jobRuns.findIndex(x=>x.id===runId); if(i>=0) state.jobRuns[i]=r;
        if(state.selectedRunId===runId){ flow.applyRun(r); renderRunOutput(r); }
        renderRunHistory();
        watchIfLive();
        if(!isLive(r.status)) loadRunsData().then(()=>{ renderJobList(); renderOverview(); renderNavCounts(); }).catch(()=>{});
      } catch(e){
        cancelling.delete(runId);
        if(isAuthErr(e)) return;
        toast('Cancel failed: '+e.message,'err');
        if(e.status===409 && state.currentJob) loadRuns(state.currentJob.id);   // it finished meanwhile
        else if(state.selectedRunId===runId){ const r=state.jobRuns.find(x=>x.id===runId); if(r) renderRunOutput(r); }
      }
    }
    async function delJob(j){
      const ok=await confirmDialog('Delete '+j.name+'?', 'This permanently deletes the job and its schedule. Run history may be lost. This cannot be undone.', 'Delete job');
      if(!ok) return;
      try { await api('/api/jobs/'+encodeURIComponent(j.id),{method:'DELETE'});
        toast('Deleted '+j.name,'ok'); state.currentJob=null;
        await loadJobsData(); renderNavCounts(); location.hash='#/jobs'; renderJobList(); renderJobDetail();
      } catch(e){ if(!isAuthErr(e)) toast('Delete failed: '+e.message,'err'); }
    }
    async function loadRuns(id){
      try { const d=await api('/api/jobs/'+encodeURIComponent(id)+'/runs?limit=20');
        if(!state.currentJob || state.currentJob.id!==id) return; // user navigated away
        const runs=d.runs||[]; state.jobRuns=runs;
        // Auto-select the latest run (its full result is already in the list).
        if(runs.length && (!state.selectedRunId || !runs.some(r=>r.id===state.selectedRunId))){
          state.selectedRunId=runs[0].id; flow.applyRun(runs[0]); renderRunOutput(runs[0]);
        } else if(!runs.length) renderRunOutput(null);
        else { const r=runs.find(x=>x.id===state.selectedRunId); flow.applyRun(r); renderRunOutput(r); }
        state.watchErr=null;
        renderJobHeader();
        renderRunHistory();
        watchIfLive();
      } catch(e){ if(!isAuthErr(e) && $('runs')) $('runs').innerHTML=`<div class="alert err">${icon('alert')}<span>Failed to load runs: ${esc(e.message)}</span></div>`; }
    }
    function renderRunHistory(){
      const runs=state.jobRuns, host=$('runs'); if(!host) return;
      // Keep keyboard focus on the same row across re-renders.
      const fr=document.activeElement && document.activeElement.closest ? document.activeElement.closest('.runrow') : null;
      const focusId = fr && host.contains(fr) ? fr.dataset.run : null;
      $('runsCount').textContent = runs.length ? (runs.length>=20?'latest 20':runs.length+' run'+(runs.length===1?'':'s')) : '';
      const err = state.watchErr ? `<div class="alert err mid" role="alert">${icon('alert')}<span>Live updates stopped: ${esc(state.watchErr)}</span>
        <button type="button" class="btn sm" data-act="retry-watch">Retry</button></div>` : '';
      host.innerHTML = err + (runs.length ? `<div class="card" style="box-shadow:none"><div class="table-wrap"><table class="tbl">
        <thead><tr><th>Status</th><th>Run</th><th class="hide-sm">Trigger</th><th title="Start time (queue time while queued)">When</th><th class="r">Duration</th></tr></thead>
        <tbody>${runs.map(r=>`<tr class="click runrow${r.id===state.selectedRunId?' sel':''}" data-run="${esc(r.id)}" tabindex="0">
          <td>${statusBadge(r.status)}</td><td class="mono muted">${esc(r.id.slice(0,8))}</td>
          <td class="hide-sm"><span class="badge plain ${esc(r.trigger||'unknown')}">${esc(r.trigger||'—')}</span></td>
          <td class="nowrap">${timeCell(runTime(r))}</td><td class="r num muted">${fmtDur(r)}</td></tr>`).join('')}
        </tbody></table></div></div>` : '<div class="muted">No runs recorded yet.</div>');
      if(focusId){ const t=host.querySelector('.runrow[data-run="'+CSS.escape(focusId)+'"]'); if(t) t.focus(); }
    }
    function showRun(id){
      const r=state.jobRuns.find(x=>x.id===id); if(!r) return;
      state.selectedRunId=id;
      $$('.runrow').forEach(tr=>tr.classList.toggle('sel', tr.dataset.run===id));
      flow.applyRun(r); renderRunOutput(r);
      watchIfLive();
      $('runOutput').scrollIntoView({behavior:'smooth', block:'nearest'});
    }

    $('newJob').addEventListener('click', ()=>openEditor(null));

    // Delegated clicks: plugin actions/rows, job items, run rows, links in rows.
    document.addEventListener('click', ev=>{
      const pa=ev.target.closest('button[data-pa]'); if(pa){ ev.stopPropagation(); pluginAct(pa, pa.dataset.name, pa.dataset.pa); return; }
      const br=ev.target.closest('button[data-br]'); if(br){ resetBreaker(br, br.dataset.br); return; }
      const rb=ev.target.closest('button[data-act="retry-breakers"]'); if(rb){ retryBreakers(rb); return; }
      const cr=ev.target.closest('button[data-act="cancel-run"]'); if(cr){ cancelRun(cr, cr.dataset.run); return; }
      const rw=ev.target.closest('button[data-act="retry-watch"]'); if(rw){ state.watchErr=null; if(state.currentJob) loadRuns(state.currentJob.id); return; }
      if(ev.target.closest('a')) return;
      const nj=ev.target.closest('[data-act="new"]'); if(nj){ openEditor(null); return; }
      const pr=ev.target.closest('tr[data-plugin]'); if(pr){ openPluginDrawer(pr.dataset.plugin); return; }
      const ji=ev.target.closest('.jitem'); if(ji){ location.hash='#/jobs/'+encodeURIComponent(ji.dataset.job); return; }
      const rr=ev.target.closest('.runrow'); if(rr){ showRun(rr.dataset.run); return; }
      const hr=ev.target.closest('tr[data-href]'); if(hr){ location.hash=hr.dataset.href; return; }
    });
    document.addEventListener('keydown', ev=>{
      if(ev.key==='Enter' && ev.target.classList && ev.target.classList.contains('runrow')) showRun(ev.target.dataset.run);
      // Global shortcuts (ignored while typing or with an overlay open).
      if(topOverlay() || /INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName) || ev.metaKey || ev.ctrlKey || ev.altKey) return;
      if(!document.body.classList.contains('authed')) return;
      if(ev.key==='r'){ refreshAll(); }
      else if(ev.key==='/'){ ev.preventDefault(); (state.view==='plugins'?$('pluginSearch'):state.view==='jobs'?$('jobSearch'):null)?.focus(); }
      else if(ev.key==='n' && state.view==='jobs'){ ev.preventDefault(); openEditor(null); }
    });

    // ================= Job editor =================
    let editingId=null;
    let manifests=null;       // { [command_name]: pluginMeta }
    let allCommands=[];        // string[] of all registered commands
    let editorVisual=[];       // [{command, name?, params, extra}] (extra: other step fields, kept as-is)
    let editorDefExtra={};     // workflow-level fields other than steps (rollback, timeout_ms, ...), kept as-is
    let editorTab='visual';
    const DEFAULT_DEF = JSON.stringify({steps:[
      {name:"greet", command:"echo", params:{message:"Hello from Orcha"}},
      {name:"repeat", command:"echo", params:{message:"greet said: {{steps.greet.output.echoed}}"}}
    ]}, null, 2);

    function modalError(msg){ const el=$('modalErr'); el.style.display=msg?'flex':'none'; el.querySelector('span').textContent=msg||''; if(msg) el.scrollIntoView({block:'nearest'}); }
    function defWarn(msg){ const el=$('defWarn'); el.style.display=msg?'flex':'none'; el.querySelector('span').textContent=msg||''; }

    async function loadManifests(){
      if(manifests) return;
      const d = state.plugins || await api('/api/plugins');
      manifests = {};
      (d.plugins||[]).forEach(p=>{ if(p.name) manifests[p.name]=p; });
      allCommands = (d.commands||[]).slice().sort();
      $('addStepCmd').innerHTML = '<option value="">Select a command…</option>' +
        allCommands.map(c=>{
          const m=manifests[c];
          if(m) return `<option value="${esc(c)}">${esc(c)}${m.description?(' — '+esc(m.description)):''}</option>`;
          return `<option value="${esc(c)}">${esc(c)} (raw JSON params)</option>`;
        }).join('');
    }

    // Manifest helpers -------------------------------------------------------
    function paramDefault(p){
      // Manifest "default" is always a string; coerce by declared type.
      if(p.default===undefined || p.default===null) {
        if(p.type==='bool') return false;
        if(p.type==='int') return '';
        if(p.type==='object') return {};
        return '';
      }
      if(p.type==='bool') return String(p.default).toLowerCase()==='true';
      if(p.type==='int'){ const n=parseInt(p.default,10); return isNaN(n)?'':n; }
      if(p.type==='object'){ try { return JSON.parse(p.default); } catch(e){ return {}; } }
      return p.default;
    }
    function defaultParamsFor(cmd){
      const m=manifests && manifests[cmd]; const out={};
      if(!m || !m.parameters) return out;
      m.parameters.forEach(p=>{ if(p.required || p.default!==undefined) out[p.name]=paramDefault(p); });
      return out;
    }

    // Visual <-> JSON sync ---------------------------------------------------
    // Rebuilds the JSON from the visual builder. Fields the builder does not
    // edit (workflow timeout_ms, a step's parallel/timeout_ms, ...) are carried
    // over untouched so saving from the visual tab never drops them.
    function visualToJson(){
      const steps = editorVisual.map(s=>{
        const o={};
        if(s.name) o.name=s.name;
        o.command=s.command;
        o.params=s.params||{};
        return Object.assign(o, s.extra||{});
      });
      $('jDef').value = JSON.stringify(Object.assign({}, editorDefExtra, {steps}), null, 2);
    }
    const splitFields = (obj, keys) => {
      const extra={}; Object.keys(obj).forEach(k=>{ if(!keys.includes(k)) extra[k]=obj[k]; }); return extra;
    };

    // Resilience controls <-> definition. The controls mirror the definition;
    // changing one writes it into the definition (visual state or JSON text).
    let shownSettings={rollback:'none', breaker:false};   // what the controls last reflected
    function setSettingsControls(def){
      const d=(def && typeof def==='object') ? def : {};
      shownSettings={ rollback: ROLLBACK_LABELS[d.rollback] ? d.rollback : 'none', breaker: d.circuit_breaker===true };
      $('jRollback').value = shownSettings.rollback;
      $('jBreaker').checked = shownSettings.breaker;
    }
    function applySetting(def, key, value){
      if(value===undefined) delete def[key]; else def[key]=value;
    }
    const keepSettingsControls = ()=>{ shownSettings={ rollback:$('jRollback').value, breaker:$('jBreaker').checked }; };
    const undoSettingsControls = ()=>{ $('jRollback').value=shownSettings.rollback; $('jBreaker').checked=shownSettings.breaker; };
    // Writes a setting into a definition object, keeping "steps" last.
    function withSetting(def, key, value){
      applySetting(def, key, value);
      const steps=def.steps; delete def.steps;
      return Object.assign(def, steps!==undefined?{steps}:{});
    }
    const isDefObject = d => !!d && typeof d==='object' && !Array.isArray(d);
    async function onSettingChange(key, value){
      if(editorTab==='visual'){ applySetting(editorDefExtra, key, value); visualToJson(); keepSettingsControls(); return; }
      if(editorTab==='yaml'){
        // Rewrites the YAML through the server (comments are not kept).
        if(defBusy){ undoSettingsControls(); return; }
        setDefBusy(true);
        try {
          const r=await yamlToDef();
          if(!r.ok || !isDefObject(r.def)){ undoSettingsControls(); if(!r.auth) defWarn('Fix the YAML before changing this setting.'); return; }
          await defToYaml(JSON.stringify(withSetting(r.def, key, value)));
          defWarn(''); yamlStatus('', YAML_NOTE); keepSettingsControls();
        } catch(e){ undoSettingsControls(); if(!isAuthErr(e)) defWarn('Could not update the YAML: '+e.message); }
        finally { setDefBusy(false); }
        return;
      }
      let def;
      try { def=JSON.parse($('jDef').value); } catch(e){ def=null; }
      if(!isDefObject(def)){
        // Undo just this control; the JSON text still holds the old value.
        defWarn('Fix the JSON before changing this setting.');
        undoSettingsControls();
        return;
      }
      defWarn('');
      $('jDef').value = JSON.stringify(withSetting(def, key, value), null, 2);
      keepSettingsControls();
    }
    $('jRollback').addEventListener('change', e=>onSettingChange('rollback', e.target.value==='none'?undefined:e.target.value));
    $('jBreaker').addEventListener('change', e=>onSettingChange('circuit_breaker', e.target.checked?true:undefined));
    let defDeb=null;
    $('jDef').addEventListener('input', ()=>{ clearTimeout(defDeb); defDeb=setTimeout(()=>{
      try { setSettingsControls(JSON.parse($('jDef').value)); } catch(e){ /* keep controls while the JSON is mid-edit */ }
    }, 200); });
    function jsonToVisual(){
      const raw=$('jDef').value.trim();
      if(!raw){ editorVisual=[]; editorDefExtra={}; setSettingsControls(null); return {ok:true}; }
      let parsed;
      try { parsed=JSON.parse(raw); } catch(e){ return {ok:false, err:'Invalid JSON: '+e.message}; }
      if(!parsed || !Array.isArray(parsed.steps)) return {ok:false, err:'Expected an object with a "steps" array.'};
      if(parsed.steps.some(s=>!s || typeof s!=='object' || Array.isArray(s))) return {ok:false, err:'Every step must be an object.'};
      editorDefExtra = splitFields(parsed, ['steps']);
      editorVisual = parsed.steps.map(s=>({
        name: s.name || '',
        command: s.command || '',
        params: (s.params && typeof s.params==='object' && !Array.isArray(s.params)) ? s.params : {},
        extra: splitFields(s, ['name','command','params'])
      }));
      setSettingsControls(parsed);
      return {ok:true};
    }

    // Renderers --------------------------------------------------------------
    function renderVisual(){
      const host=$('defSteps');
      if(!editorVisual.length){
        host.innerHTML = '<div class="empty-hint">No steps yet. Pick a command below and click <strong>Add step</strong>, or switch to the JSON tab.</div>';
        return;
      }
      host.innerHTML = editorVisual.map((s,i)=>renderStepCard(s,i)).join('');
    }
    function renderStepCard(s, i){
      const m = manifests && manifests[s.command];
      const params = m && m.parameters ? m.parameters.map(p=>renderParamField(i, p, s.params[p.name])).join('') : renderRawParams(i, s.params);
      return `<div class="step-card" data-step="${i}">
        <div class="shd"><span class="n">${i+1}</span><span class="cmd">${esc(s.command || '(no command)')}</span>
          ${m ? (m.description?`<span class="muted" style="font-size:12px">${esc(m.description)}</span>`:'') : '<span class="badge plain unknown" title="No manifest for this command">raw</span>'}
          <span class="sp"></span>${stepExtraChips(s.extra)}
          <button type="button" class="btn ghost sm icon" data-act="up" data-i="${i}" ${i===0?'disabled':''} title="Move up" aria-label="Move step ${i+1} up">${icon('up')}</button>
          <button type="button" class="btn ghost sm icon" data-act="down" data-i="${i}" ${i===editorVisual.length-1?'disabled':''} title="Move down" aria-label="Move step ${i+1} down">${icon('down')}</button>
          <button type="button" class="btn ghost sm icon" data-act="del" data-i="${i}" title="Remove step" aria-label="Remove step ${i+1}">${icon('trash')}</button>
        </div>
        <div class="sbd">
          <div class="param"><div class="pname">name <span class="opt">optional</span></div>
            <div><input class="input" type="text" data-bind="name" data-i="${i}" value="${esc(s.name||'')}" placeholder="e.g. greet" />
              <div class="pdesc">Lets later steps reference this one as <code>{{steps.${esc(s.name||'NAME')}.output}}</code>.</div></div></div>
          ${params}
        </div>
      </div>`;
    }
    // Step-level settings the builder keeps but does not edit (edit them in JSON).
    function stepExtraChips(extra){
      if(!extra) return '';
      const chips=[];
      if(extra.parallel===true) chips.push('<span class="tag" title="Runs concurrently with the following steps (edit in JSON)">parallel</span>');
      if(Number(extra.timeout_ms)>0) chips.push(`<span class="tag" title="Step time limit (edit in JSON)">${icon('clock')}${esc(fmtMs(extra.timeout_ms))}</span>`);
      return chips.join('');
    }
    function renderParamField(i, p, value){
      const req = p.required ? '<span class="req" title="Required">*</span>' : '';
      const label = `<div class="pname">${esc(p.name)}${req}</div>`;
      const desc = p.description ? `<div class="pdesc">${esc(p.description)}</div>` : '';
      let ctrl='';
      const v = (value===undefined) ? paramDefault(p) : value;
      const common = `data-bind="param" data-pname="${esc(p.name)}" data-i="${i}"`;
      if(p.type==='bool'){
        ctrl = `<div><input type="checkbox" ${common} data-ptype="bool" ${v?'checked':''} />${desc}</div>`;
      } else if(p.type==='int'){
        ctrl = `<div><input class="input" type="number" ${common} data-ptype="int" value="${v===''?'':esc(String(v))}" placeholder="${esc(p.example||'')}" />${desc}</div>`;
      } else if(p.type==='object'){
        ctrl = `<div>${renderObjectRows(i, p.name, (v && typeof v==='object') ? v : {})}${desc}</div>`;
      } else { // string (default)
        ctrl = `<div><input class="input" type="text" ${common} data-ptype="string" value="${esc(v==null?'':String(v))}" placeholder="${esc(p.example||'')}" />${desc}</div>`;
      }
      return `<div class="param">${label}${ctrl}</div>`;
    }
    function renderObjectRows(i, pname, obj){
      const entries=Object.entries(obj||{});
      const rows = entries.map(([k,val],ri)=>`
        <div class="kv-row" data-ri="${ri}">
          <input class="input" type="text" data-bind="kvk" data-i="${i}" data-pname="${esc(pname)}" data-ri="${ri}" value="${esc(k)}" placeholder="key" />
          <input class="input" type="text" data-bind="kvv" data-i="${i}" data-pname="${esc(pname)}" data-ri="${ri}" value="${esc(val==null?'':String(val))}" placeholder="value" />
          <button type="button" class="btn ghost sm icon" data-act="kvdel" data-i="${i}" data-pname="${esc(pname)}" data-ri="${ri}" title="Remove" aria-label="Remove entry">${icon('x')}</button>
        </div>`).join('');
      return `<div class="kv-rows">${rows}
        <button type="button" class="kv-add" data-act="kvadd" data-i="${i}" data-pname="${esc(pname)}">+ Add entry</button></div>`;
    }
    function renderRawParams(i, params){
      // Unknown command: render a single raw-JSON textarea for params.
      const txt = JSON.stringify(params||{}, null, 2);
      return `<div class="param"><div class="pname">params <span class="opt">JSON</span></div>
        <div><textarea class="input" data-bind="rawparams" data-i="${i}" spellcheck="false">${esc(txt)}</textarea></div></div>`;
    }

    // Visual-tab event handling ---------------------------------------------
    function rerenderAndSync(){ renderVisual(); visualToJson(); }
    $('defSteps').addEventListener('input', ev=>{
      const t=ev.target;
      const i=parseInt(t.dataset.i,10);
      if(isNaN(i) || !editorVisual[i]) return;
      const bind=t.dataset.bind;
      if(bind==='name'){ editorVisual[i].name = t.value; }
      else if(bind==='param'){
        const pname=t.dataset.pname, ptype=t.dataset.ptype;
        if(ptype==='bool') editorVisual[i].params[pname] = t.checked;
        else if(ptype==='int'){ const n=parseInt(t.value,10); editorVisual[i].params[pname] = isNaN(n) ? '' : n; }
        else editorVisual[i].params[pname] = t.value;
      } else if(bind==='kvk' || bind==='kvv'){
        // Rebuild the object from the DOM rows of this parameter.
        const pname=t.dataset.pname;
        const rows=t.closest('.kv-rows').querySelectorAll('.kv-row');
        const next={};
        rows.forEach(row=>{
          const k=row.querySelector('input[data-bind="kvk"]').value;
          const v=row.querySelector('input[data-bind="kvv"]').value;
          if(k!=='') next[k]=v;
        });
        editorVisual[i].params[pname]=next;
      } else if(bind==='rawparams'){
        try { editorVisual[i].params = JSON.parse(t.value || '{}'); t.classList.remove('invalid'); } catch(e){ t.classList.add('invalid'); }
      }
      visualToJson();
    });
    $('defSteps').addEventListener('click', ev=>{
      const b=ev.target.closest('button[data-act]'); if(!b) return;
      const i=parseInt(b.dataset.i,10); if(isNaN(i)) return;
      const act=b.dataset.act;
      if(act==='up' && i>0){ const t=editorVisual[i-1]; editorVisual[i-1]=editorVisual[i]; editorVisual[i]=t; rerenderAndSync(); }
      else if(act==='down' && i<editorVisual.length-1){ const t=editorVisual[i+1]; editorVisual[i+1]=editorVisual[i]; editorVisual[i]=t; rerenderAndSync(); }
      else if(act==='del'){ editorVisual.splice(i,1); rerenderAndSync(); }
      else if(act==='kvadd'){
        // Objects can't hold duplicate empty keys, so at most one blank row is pending at a time.
        const pname=b.dataset.pname;
        const obj=editorVisual[i].params[pname] || (editorVisual[i].params[pname]={});
        if(obj['']===undefined) obj['']='';
        rerenderAndSync();
        const rows=$('defSteps').querySelectorAll(`.step-card[data-step="${i}"] .kv-row input[data-bind="kvk"]`);
        const blank=Array.from(rows).find(x=>x.value===''); if(blank) blank.focus();
      }
      else if(act==='kvdel'){
        const pname=b.dataset.pname, ri=parseInt(b.dataset.ri,10);
        const obj=editorVisual[i].params[pname]||{};
        const keys=Object.keys(obj);
        if(ri>=0 && ri<keys.length){ delete obj[keys[ri]]; rerenderAndSync(); }
      }
    });
    function addStep(){
      const cmd=$('addStepCmd').value;
      if(!cmd){ $('addStepCmd').focus(); toast('Select a command to add','err'); return; }
      editorVisual.push({ name:'', command:cmd, params:defaultParamsFor(cmd), extra:{} });
      $('addStepCmd').value='';
      rerenderAndSync();
      const cards=$('defSteps').querySelectorAll('.step-card'); const last=cards[cards.length-1];
      if(last){ last.scrollIntoView({block:'nearest'}); last.querySelector('input')?.focus(); }
    }
    $('addStepBtn').addEventListener('click', addStep);

    // Tab switching ----------------------------------------------------------
    function showDefTab(name){
      editorTab = name;
      $('defVisual').style.display = (name==='visual') ? '' : 'none';
      $('defJson').style.display   = (name==='json')   ? '' : 'none';
      $('defYaml').style.display   = (name==='yaml')   ? '' : 'none';
      $$('.tab[data-deftab]').forEach(t=>{ const on=t.dataset.deftab===name; t.classList.toggle('active',on); t.setAttribute('aria-selected',on); t.tabIndex = on ? 0 : -1; });
    }
    // Tab switches, setting changes and imports may wait on the server; one at a time.
    let defBusy=false;
    function setDefBusy(on){
      defBusy=on;
      $$('.tab[data-deftab]').forEach(t=>{ t.disabled=on; });
      $('defImport').disabled=on; $('jRollback').disabled=on; $('jBreaker').disabled=on;
      // The server's reply replaces the text: no typing meanwhile.
      $('jYaml').readOnly=on; $('jDef').readOnly=on;
      $('modalSave').disabled = on || $('modalSave').classList.contains('loading');
      $('defField').setAttribute('aria-busy', on);
    }
    // Brings the JSON text (#jDef) up to date from the tab being left.
    async function leaveDefTab(){
      if(editorTab==='visual'){ visualToJson(); return true; }
      if(editorTab!=='yaml') return true;
      const r=await yamlToDef();
      if(!r.ok){
        if(!r.auth){ defWarn("Fix the YAML to switch tabs."); yamlStatus('err', r.err); if(r.line) focusYamlAt(r.line, r.column); }
        return false;
      }
      $('jDef').value = r.def==null ? '' : defJsonText(r.def);
      return true;
    }
    async function setDefTab(name, opts){
      if(defBusy || name===editorTab) return;
      setDefBusy(true);
      try {
        if(!await leaveDefTab()) return;
        if(name==='visual'){
          const r = jsonToVisual();
          if(!r.ok){ defWarn("Can't switch to the visual builder: "+r.err+" Fix the definition or keep editing it here."); return; }
          renderVisual();
        } else if(name==='yaml'){
          const raw=$('jDef').value.trim();
          if(raw){
            try { JSON.parse(raw); } catch(e){ defWarn("Can't switch to YAML: the JSON is invalid ("+e.message+")."); return; }
          }
          try { await defToYaml(raw); }
          catch(e){ if(!isAuthErr(e)) defWarn("Can't switch to YAML: "+e.message); return; }
          yamlStatus('', YAML_NOTE);
        }
        defWarn('');
        showDefTab(name);
        if(!(opts && opts.auto)) store.set(localStorage, DEFTAB_KEY, name);
      } finally { setDefBusy(false); }
    }
    $$('.tab[data-deftab]').forEach(t=> t.addEventListener('click', ()=>setDefTab(t.dataset.deftab)));
    $('defField').querySelector('[role=tablist]').addEventListener('keydown', async e=>{
      const tabs=$$('.tab[data-deftab]'), i=tabs.indexOf(document.activeElement);
      if(i<0) return;
      const next = e.key==='ArrowRight' ? (i+1)%tabs.length : e.key==='ArrowLeft' ? (i+tabs.length-1)%tabs.length
                 : e.key==='Home' ? 0 : e.key==='End' ? tabs.length-1 : -1;
      if(next<0) return;
      e.preventDefault();
      await setDefTab(tabs[next].dataset.deftab);
      $$('.tab[data-deftab]').find(t=>t.classList.contains('active')).focus();
    });

    // YAML tab ---------------------------------------------------------------
    // The server converts YAML (POST /api/definitions/convert), so the editor
    // reads it exactly as the CLI and the jobs API do and needs no YAML parser.
    // #jDef (JSON) stays the editor's common format; yamlSync remembers the
    // last converted YAML text so unchanged YAML is not converted again.
    const DEFTAB_KEY='orcha_deftab';
    const YAML_NOTE='Comments and formatting are not kept: jobs are saved as JSON.';
    let yamlSync={yaml:'', json:''};
    let yamlSeq=0;
    const defJsonText = def => JSON.stringify(def, null, 2);
    const convertDef = (text, type) => api('/api/definitions/convert', {method:'POST', headers:{'Content-Type':type}, body:text});
    function yamlStatus(kind, msg){
      // A live region: only write when something changed, so it is announced once.
      const el=$('yamlStatus'), cls='hint'+(kind?' '+kind:'');
      if(el.className!==cls) el.className=cls;
      if(el.textContent!==(msg||'')) el.textContent=msg||'';
    }
    function yamlErrText(e){
      const b=e.body||{};
      const msg=String(b.error||e.message||'').replace(/^Invalid YAML body: /,'').replace(/^line \d+, column \d+: /,'');
      return b.line ? `Line ${b.line}, column ${b.column}: ${msg}` : msg;
    }
    // YAML text -> {ok, def} (def is null for empty text) or {ok:false, err, line?, column?, auth?}.
    async function yamlToDef(){
      const text=$('jYaml').value;
      if(!text.trim()) return {ok:true, def:null};
      if(text===yamlSync.yaml) return {ok:true, def:JSON.parse(yamlSync.json)};
      try {
        const r=await convertDef(text, 'application/yaml');
        yamlSync={yaml:text, json:defJsonText(r.definition)};
        return {ok:true, def:r.definition};
      } catch(e){
        if(isAuthErr(e)) return {ok:false, auth:true};
        return {ok:false, err:yamlErrText(e), line:e.body&&e.body.line, column:e.body&&e.body.column};
      }
    }
    // JSON text -> YAML tab (throws on a server error).
    async function defToYaml(jsonText){
      if(!jsonText.trim()){ $('jYaml').value=''; yamlSync={yaml:'', json:''}; return; }
      const r=await convertDef(jsonText, 'application/json');
      $('jYaml').value=r.yaml; yamlSync={yaml:r.yaml, json:defJsonText(r.definition)};
    }
    function focusYamlAt(line, column){
      const ta=$('jYaml'), lines=ta.value.split('\n');
      let off=0;
      for(let i=0; i<Math.min(line-1, lines.length); i++) off+=lines[i].length+1;
      off=Math.min(off+Math.max(0,(column||1)-1), ta.value.length);
      ta.focus(); ta.setSelectionRange(off, off);
      const lh=parseFloat(getComputedStyle(ta).lineHeight)||19;
      ta.scrollTop=Math.max(0, (line-3)*lh);
    }
    // Live check while typing; replies to older text are dropped.
    async function checkYaml(){
      const seq=++yamlSeq;
      const r=await yamlToDef();
      if(seq!==yamlSeq || editorTab!=='yaml' || r.auth) return;
      if(!r.ok){ yamlStatus('err', r.err); return; }
      defWarn(''); modalError('');
      if(r.def==null){ yamlStatus('', 'Empty definition. '+YAML_NOTE); return; }
      yamlStatus('ok', 'Valid YAML. '+YAML_NOTE);
      setSettingsControls(r.def);
    }
    let yamlDeb=null;
    $('jYaml').addEventListener('input', ()=>{ clearTimeout(yamlDeb); yamlDeb=setTimeout(checkYaml, 350); });

    // Import ----------------------------------------------------------------
    // A file holds a workflow definition ({steps: [...]}) or a whole job
    // ({name, schedule_cron, enabled, description, definition}).
    const MAX_IMPORT_BYTES=512*1024;   // the server's YAML body limit (kMaxYamlBodyBytes)
    function currentDefText(){
      if(editorTab==='visual'){ visualToJson(); return $('jDef').value; }
      if(editorTab==='yaml') return $('jYaml').value===yamlSync.yaml ? yamlSync.json : $('jYaml').value;
      return $('jDef').value;
    }
    async function importDefFile(file){
      if(!file) return;
      if(defBusy){ toast('The editor is busy. Try again in a moment.','err'); return; }
      if(file.size>MAX_IMPORT_BYTES){ toast(`${file.name} is too large to import (the limit is 512 KB).`,'err'); return; }
      setDefBusy(true);
      try {
        const text=await file.text();
        let fmt = /\.json$/i.test(file.name) ? 'json' : /\.ya?ml$/i.test(file.name) ? 'yaml' : null;
        let data;
        if(fmt!=='yaml'){
          try { data=JSON.parse(text); fmt='json'; }
          catch(e){ if(fmt==='json') throw new Error('it is not valid JSON ('+e.message+')'); fmt='yaml'; }
        }
        if(fmt==='yaml'){
          try { data=(await convertDef(text, 'application/yaml')).definition; }
          catch(e){ if(isAuthErr(e)) return; throw new Error(yamlErrText(e)); }
        }
        if(!isDefObject(data)) throw new Error('expected a workflow definition or a job.');
        const isJob = isDefObject(data.definition) && !Array.isArray(data.steps);
        const def = isJob ? data.definition : data;
        if(!Array.isArray(def.steps)) throw new Error('no "steps" list found. Expected a workflow definition, or a job with a "definition".');

        const current=currentDefText().trim();
        const typed = ['jName','jDesc','jSchedule'].some(id=>$(id).value.trim());
        const pristine = !current || (!editingId && current===DEFAULT_DEF);
        if(!pristine || (isJob && (editingId || typed))){
          const ok=await confirmDialog(isJob ? 'Replace this job with the file?' : 'Replace the current definition?',
            isJob ? `The name, schedule, settings and steps in the editor will be replaced with the ones in ${file.name}. Nothing is saved until you save the job.`
                  : `The steps in the editor will be replaced with the ones in ${file.name}. Nothing is saved until you save the job.`,
            'Replace');
          if(!ok) return;
        }

        if(isJob){
          // Every job field comes from the file; missing ones get the API's
          // defaults (no schedule = manual only, enabled). A missing name keeps
          // the typed one, since a job needs a name to be saved.
          if(typeof data.name==='string' && data.name.trim()){ $('jName').value=data.name.trim(); $('jName').classList.remove('invalid'); }
          $('jDesc').value = typeof data.description==='string' ? data.description : '';
          $('jSchedule').value = typeof data.schedule_cron==='string' ? data.schedule_cron : '';
          $('jEnabled').checked = typeof data.enabled==='boolean' ? data.enabled : true;
          syncPreset(); updateCronPreview();
        }
        const jsonText=defJsonText(def);
        $('jDef').value=jsonText;
        editorDefExtra={}; editorVisual=[];
        setSettingsControls(def);
        if(fmt==='yaml'){
          // A bare definition keeps the file's own text (and comments) while editing.
          if(isJob) await defToYaml(jsonText);
          else { $('jYaml').value=text; yamlSync={yaml:text, json:jsonText}; }
          yamlStatus('ok', 'Imported '+file.name+'. '+YAML_NOTE);
          showDefTab('yaml');
        } else {
          showDefTab('json');
        }
        defWarn(''); modalError('');
        toast(`Imported ${file.name}${isJob?' with its job settings':''}. Review it, then save.`, 'ok');
      } catch(e){
        if(!isAuthErr(e)) defWarn(`Couldn't import ${file.name}: ${e.message}`);
      } finally {
        setDefBusy(false);
        $('defFile').value='';
      }
    }
    $('defImport').addEventListener('click', ()=>$('defFile').click());
    $('defFile').addEventListener('change', e=>importDefFile(e.target.files[0]));
    const hasFiles = e => Array.from((e.dataTransfer && e.dataTransfer.types) || []).includes('Files');
    let dragDepth=0;
    $('defField').addEventListener('dragenter', e=>{ if(!hasFiles(e)) return; e.preventDefault(); dragDepth++; $('defField').classList.add('dropping'); });
    $('defField').addEventListener('dragover', e=>{ if(!hasFiles(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = defBusy ? 'none' : 'copy'; });
    $('defField').addEventListener('dragleave', e=>{ if(!hasFiles(e)) return; if(--dragDepth<=0){ dragDepth=0; $('defField').classList.remove('dropping'); } });
    $('defField').addEventListener('drop', e=>{
      if(!hasFiles(e)) return;
      e.preventDefault(); e.stopPropagation(); dragDepth=0; $('defField').classList.remove('dropping');
      const files=e.dataTransfer.files;
      if(files.length>1){ toast('Drop one file at a time.','err'); return; }
      importDefFile(files[0]);
    });
    // A file dropped elsewhere in the editor must not navigate away from the page.
    ['dragover','drop'].forEach(t=>$('modal').addEventListener(t, e=>{ if(hasFiles(e)) e.preventDefault(); }));

    // Cron helpers -----------------------------------------------------------
    // The backend scheduler evaluates cron in UTC (jobs/CronExpr.hpp), so the
    // preview matches in UTC and displays the resulting instants in local time.
    function cronField(spec, lo, hi){
      const set=new Set(); const wildcard = (spec==='*');
      spec.split(',').forEach(part=>{
        let step=1, range=part;
        const slash=part.indexOf('/');
        if(slash>=0){ step=parseInt(part.slice(slash+1),10); range=part.slice(0,slash); }
        let a, b;
        if(range==='*'){ a=lo; b=hi; }
        else if(range.includes('-')){ const [x,y]=range.split('-'); a=parseInt(x,10); b=parseInt(y,10); }
        else { a=b=parseInt(range,10); }
        if(isNaN(a)||isNaN(b)||isNaN(step)||step<=0) throw new Error('bad field "'+spec+'"');
        for(let v=a; v<=b; v+=step) if(v>=lo && v<=(hi===6?7:hi)) set.add(v);
      });
      if(lo===0 && hi===6 && set.has(7)){ set.delete(7); set.add(0); } // dow: 7→0
      return {set, wildcard};
    }
    function cronParse(expr){
      const parts=expr.trim().split(/\s+/);
      if(parts.length!==5) throw new Error('expected 5 fields (m h dom mon dow)');
      const ranges=[[0,59],[0,23],[1,31],[1,12],[0,6]];
      return parts.map((p,i)=>cronField(p, ranges[i][0], ranges[i][1]));
    }
    function cronNext(expr, n){
      const [m,h,dom,mon,dow]=cronParse(expr);
      const out=[]; const d=new Date();
      d.setUTCSeconds(0,0); d.setUTCMinutes(d.getUTCMinutes()+1);
      const limit = 366*24*60;
      for(let i=0; i<limit && out.length<n; i++){
        const dayOK = (!dom.wildcard && !dow.wildcard)
          ? (dom.set.has(d.getUTCDate()) || dow.set.has(d.getUTCDay()))
          : (!dom.wildcard ? dom.set.has(d.getUTCDate())
             : (!dow.wildcard ? dow.set.has(d.getUTCDay()) : true));
        if(m.set.has(d.getUTCMinutes()) && h.set.has(d.getUTCHours()) &&
           mon.set.has(d.getUTCMonth()+1) && dayOK){
          out.push(new Date(d));
        }
        d.setUTCMinutes(d.getUTCMinutes()+1);
      }
      return out;
    }
    function updateCronPreview(){
      const el=$('cronPreview'), inp=$('jSchedule'), expr=inp.value.trim();
      inp.classList.remove('invalid');
      if(!expr){ el.className='hint'; el.textContent='Manual only — run from the dashboard, the API, or another job via run_job.'; return true; }
      try {
        const runs=cronNext(expr, 3);
        if(!runs.length){ el.className='hint err'; el.textContent='No upcoming runs within a year.'; inp.classList.add('invalid'); return false; }
        el.className='hint';
        el.innerHTML = 'Evaluated in UTC. Next runs (your time): ' + runs.map(r=>'<strong>'+esc(r.toLocaleString(undefined,{weekday:'short',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit'}))+'</strong>').join(' · ');
        return true;
      } catch(e){
        el.className='hint err'; el.textContent='Invalid cron: '+e.message; inp.classList.add('invalid'); return false;
      }
    }
    let cronDeb=null;
    function syncPreset(){ const v=$('jSchedule').value.trim(); $('cronPreset').value = Array.from($('cronPreset').options).some(o=>o.value===v) ? v : ''; }
    $('jSchedule').addEventListener('input', ()=>{ clearTimeout(cronDeb); cronDeb=setTimeout(updateCronPreview, 150); syncPreset(); });
    $('cronPreset').addEventListener('change', e=>{ $('jSchedule').value = e.target.value; updateCronPreview(); });

    // Open / save -----------------------------------------------------------
    async function openEditor(job){
      editingId = job ? job.id : null;
      $('modalTitle').textContent = job ? 'Edit job' : 'New job';
      $('modalSave').querySelector('span').textContent = job ? 'Save changes' : 'Create job';
      $('jName').value = job ? job.name : '';
      $('jName').classList.remove('invalid');
      $('jDesc').value = job ? (job.description||'') : '';
      $('jSchedule').value = (job && job.schedule_cron) ? job.schedule_cron : '';
      syncPreset();
      $('jEnabled').checked = job ? !!job.enabled : true;
      $('jDef').value  = job ? JSON.stringify(job.definition, null, 2) : DEFAULT_DEF;
      $('jYaml').value = ''; yamlSync={yaml:'', json:''}; yamlSeq++; yamlStatus('', '');
      editorDefExtra = {}; editorVisual = [];
      setSettingsControls(job ? job.definition : null);
      // Until the definition is loaded into the builder below, a settings
      // change could serialise stale builder state.
      $('jRollback').disabled = true; $('jBreaker').disabled = true;
      modalError(''); defWarn('');
      updateCronPreview();
      $('defSteps').innerHTML = '<span class="skel" style="width:50%"></span>';
      openOverlay($('modal'), '#jName');

      // Load manifests, then try to populate visual. On parse failure, go to JSON tab.
      try { await loadManifests(); }
      catch(e){ if(!isAuthErr(e)) toast('Failed to load command list: '+e.message,'err'); }

      const r = jsonToVisual();
      if(r.ok){ showDefTab('visual'); renderVisual(); }
      else { showDefTab('json'); defWarn("This definition can't be shown in the visual builder: "+r.err+" Edit the JSON directly."); }
      $('jRollback').disabled = false; $('jBreaker').disabled = false;
      // Reopen in the text format the user last chose.
      const pref=store.get(localStorage, DEFTAB_KEY);
      if(pref==='yaml' || (pref==='json' && r.ok)) await setDefTab(pref, {auto:true});
    }
    async function saveJob(){
      const btn=$('modalSave'); if(btn.disabled || defBusy) return;
      let def;
      if(editorTab==='yaml'){
        busy(btn,true); const r=await yamlToDef(); busy(btn,false);
        if(!r.ok){ if(!r.auth){ modalError('Fix the YAML before saving.'); yamlStatus('err', r.err); if(r.line) focusYamlAt(r.line, r.column); } return; }
        if(r.def==null){ modalError('The definition is empty. Add a "steps" list.'); $('jYaml').focus(); return; }
        def=r.def;
      } else {
        if(editorTab==='visual') visualToJson();
        try { def=JSON.parse($('jDef').value); }
        catch(e){ modalError('Definition is not valid JSON: '+e.message); showDefTab('json'); return; }
      }
      const payload={ name:$('jName').value.trim(), description:$('jDesc').value.trim(),
                      definition:def, enabled:$('jEnabled').checked };
      const sched=$('jSchedule').value.trim();
      if(sched) payload.schedule_cron=sched;   // omitted => cleared (manual only)
      if(!payload.name){ modalError('Name is required.'); $('jName').classList.add('invalid'); $('jName').focus(); return; }
      if(sched && !updateCronPreview()){ modalError('Fix the cron schedule or leave it blank.'); $('jSchedule').focus(); return; }
      modalError(''); busy(btn,true);
      try {
        let saved;
        if(editingId) saved=await api('/api/jobs/'+encodeURIComponent(editingId),{method:'PUT', json:payload});
        else saved=await api('/api/jobs',{method:'POST', json:payload});
        closeOverlay($('modal')); toast(editingId?'Saved '+payload.name:'Created '+payload.name,'ok');
        await loadJobsData(); renderNavCounts(); renderJobList();
        const id = editingId || (saved && (saved.id || (saved.job&&saved.job.id)));
        if(id){ if(location.hash==='#/jobs/'+encodeURIComponent(id)) selectJob(id); else location.hash='#/jobs/'+encodeURIComponent(id); }
      } catch(e){ if(!isAuthErr(e)) modalError(e.message); }
      finally { busy(btn,false); }
    }
    $('modalSave').addEventListener('click', saveJob);
    $('modal').addEventListener('keydown', e=>{ if(e.key==='Enter' && (e.metaKey||e.ctrlKey)){ e.preventDefault(); saveJob(); } });
    $('jName').addEventListener('input', ()=>$('jName').classList.remove('invalid'));

    // ================= Flow chart (Canvas) =================
    const flow = (function(){
      const NODE_W=176, NODE_H=54, COL_GAP=64, ROW_GAP=20, PAD=24;
      let canvas=null, ctx=null, tip=null;
      let nodes=[], edges=[], job=null, runStatus=null; // runStatus: array of bool|null per step
      let hover=-1, sel=-1, drag=-1, dragDX=0, dragDY=0, dpr=1, downX=0, downY=0;
      let onSelect=null; // callback(stepIndex) on node click (not drag)

      function alpha(color, a){
        // Tokens are hex; turn into rgba for canvas tints.
        const m=/^#([0-9a-f]{6})$/i.exec(color); if(!m) return color;
        const n=parseInt(m[1],16); return `rgba(${n>>16},${(n>>8)&255},${n&255},${a})`;
      }

      function parse(def){
        const steps=(def&&def.steps)||[];
        const n=steps.length;
        const nameToIndex={};
        for(let i=0;i<n;i++){ if(steps[i].name) nameToIndex[steps[i].name]=i; }
        const deps=Array.from({length:n},()=>new Set());
        for(let i=0;i<n;i++){
          const s=JSON.stringify(steps[i].params||{}); let m;
          // positional {{stepN.output}}
          const re=/\{\{\s*step(\d+)\b/g;
          while((m=re.exec(s))){ const k=parseInt(m[1],10)-1; if(k>=0&&k<n&&k!==i) deps[i].add(k); }
          // named {{steps.<name>.output}}
          const reN=/\{\{\s*steps\.([A-Za-z_]\w*)/g;
          while((m=reN.exec(s))){ const k=nameToIndex[m[1]]; if(k!==undefined&&k!==i) deps[i].add(k); }
        }
        const level=new Array(n).fill(0);
        for(let it=0;it<n;it++) for(let i=0;i<n;i++) for(const d of deps[i]) level[i]=Math.max(level[i],level[d]+1);
        const rowOf={}; nodes=[]; edges=[];
        for(let i=0;i<n;i++){
          const L=level[i]; rowOf[L]=(rowOf[L]||0);
          const row=rowOf[L]++;
          nodes.push({ i, name:(steps[i].name||''), cmd:(steps[i].command||'?'), params:steps[i].params||{},
            x:PAD+L*(NODE_W+COL_GAP), y:PAD+row*(NODE_H+ROW_GAP) });
        }
        for(let i=0;i<n;i++) for(const d of deps[i]) edges.push([d,i]);
      }

      function resize(){
        if(!canvas) return;
        let maxX=320, maxY=110;
        nodes.forEach(nd=>{ maxX=Math.max(maxX, nd.x+NODE_W+PAD); maxY=Math.max(maxY, nd.y+NODE_H+PAD); });
        const wrap=canvas.parentElement; if(wrap) maxX=Math.max(maxX, wrap.clientWidth-2);
        dpr=window.devicePixelRatio||1;
        canvas.style.width=maxX+'px'; canvas.style.height=maxY+'px';
        canvas.width=maxX*dpr; canvas.height=maxY*dpr;
        ctx.setTransform(dpr,0,0,dpr,0,0);
        render();
      }

      function roundRect(x,y,w,h,r){ ctx.beginPath(); ctx.moveTo(x+r,y);
        ctx.arcTo(x+w,y,x+w,y+h,r); ctx.arcTo(x+w,y+h,x,y+h,r);
        ctx.arcTo(x,y+h,x,y,r); ctx.arcTo(x,y,x+w,y,r); ctx.closePath(); }

      function fit(text, max){ if(ctx.measureText(text).width<=max) return text;
        while(text.length>1 && ctx.measureText(text+'…').width>max) text=text.slice(0,-1); return text+'…'; }

      function render(){
        if(!ctx || !canvas.isConnected) return;
        ctx.clearRect(0,0,canvas.width,canvas.height);
        const fg=cssVar('--fg'), muted=cssVar('--muted'), faint=cssVar('--faint'), line=cssVar('--line-strong'),
              panel=cssVar('--panel'), accent=cssVar('--accent'), ok=cssVar('--ok'), err=cssVar('--err');
        const font=getComputedStyle(document.body).fontFamily, mono=cssVar('--mono');
        // edges
        ctx.lineWidth=1.6;
        edges.forEach(([a,b])=>{
          const na=nodes[a], nb=nodes[b];
          const x1=na.x+NODE_W, y1=na.y+NODE_H/2, x2=nb.x, y2=nb.y+NODE_H/2;
          const hl = (hover===a||hover===b||sel===a||sel===b);
          ctx.strokeStyle = hl?accent:faint; ctx.fillStyle=hl?accent:faint;
          ctx.beginPath(); ctx.moveTo(x1,y1);
          const mx=(x1+x2)/2; ctx.bezierCurveTo(mx,y1,mx,y2,x2-6,y2); ctx.stroke();
          ctx.beginPath(); ctx.moveTo(x2,y2); ctx.lineTo(x2-8,y2-4.5); ctx.lineTo(x2-8,y2+4.5); ctx.closePath(); ctx.fill();
        });
        // nodes
        nodes.forEach(nd=>{
          const st = runStatus ? runStatus[nd.i] : null;
          const tone = st===true ? ok : st===false ? err : null;
          ctx.save();
          ctx.shadowColor='rgba(0,0,0,.08)'; ctx.shadowBlur=6; ctx.shadowOffsetY=2;
          roundRect(nd.x,nd.y,NODE_W,NODE_H,9); ctx.fillStyle=panel; ctx.fill();
          ctx.restore();
          if(tone){ roundRect(nd.x,nd.y,NODE_W,NODE_H,9); ctx.fillStyle=alpha(tone,.08); ctx.fill(); }
          roundRect(nd.x,nd.y,NODE_W,NODE_H,9);
          ctx.lineWidth=(nd.i===sel||nd.i===hover)?2:1;
          ctx.strokeStyle=(nd.i===hover||nd.i===sel)?accent:(tone?alpha(tone,.55):line); ctx.stroke();
          // status rail
          if(tone){ ctx.fillStyle=tone; roundRect(nd.x+1,nd.y+8,3,NODE_H-16,1.5); ctx.fill(); }
          ctx.fillStyle=muted; ctx.font='600 10.5px '+mono;
          ctx.fillText(fit('#'+(nd.i+1)+(nd.name?('  '+nd.name):''), NODE_W-40), nd.x+14, nd.y+20);
          ctx.fillStyle=fg; ctx.font='500 13px '+font;
          ctx.fillText(fit(nd.cmd, NODE_W-28), nd.x+14, nd.y+40);
          if(tone){ // status glyph
            const cx=nd.x+NODE_W-16, cy=nd.y+16; ctx.fillStyle=tone; ctx.beginPath(); ctx.arc(cx,cy,6,0,Math.PI*2); ctx.fill();
            ctx.strokeStyle=panel; ctx.lineWidth=1.8; ctx.beginPath();
            if(st){ ctx.moveTo(cx-3,cy); ctx.lineTo(cx-1,cy+2); ctx.lineTo(cx+3,cy-2); }
            else { ctx.moveTo(cx-2.5,cy-2.5); ctx.lineTo(cx+2.5,cy+2.5); ctx.moveTo(cx+2.5,cy-2.5); ctx.lineTo(cx-2.5,cy+2.5); }
            ctx.stroke();
          }
        });
      }

      function hit(mx,my){ for(let i=nodes.length-1;i>=0;i--){ const n=nodes[i];
        if(mx>=n.x&&mx<=n.x+NODE_W&&my>=n.y&&my<=n.y+NODE_H) return i; } return -1; }

      // Canvas listeners are bound per canvas element (it is recreated with the
      // job detail); the window mouseup listener is bound exactly once.
      function bind(){
        canvas.addEventListener('mousedown', e=>{ const r=canvas.getBoundingClientRect();
          const mx=e.clientX-r.left, my=e.clientY-r.top; const i=hit(mx,my);
          sel=i; downX=e.clientX; downY=e.clientY;
          if(i>=0){ drag=i; dragDX=mx-nodes[i].x; dragDY=my-nodes[i].y; canvas.style.cursor='grabbing'; } render(); });
        canvas.addEventListener('mousemove', e=>{ const r=canvas.getBoundingClientRect();
          const mx=e.clientX-r.left, my=e.clientY-r.top;
          if(drag>=0){ nodes[drag].x=Math.max(0,mx-dragDX); nodes[drag].y=Math.max(0,my-dragDY); tip.style.display='none'; render(); return; }
          const i=hit(mx,my);
          if(i!==hover){ hover=i; canvas.style.cursor=i>=0?'pointer':'default'; render(); }
          if(i>=0){ const nd=nodes[i];
            tip.style.display='block'; tip.style.left=(nd.x+NODE_W+10)+'px'; tip.style.top=nd.y+'px';
            tip.textContent=(nd.name?nd.name+' · ':'')+nd.cmd+'\n'+JSON.stringify(nd.params,null,1).slice(0,300);
          } else tip.style.display='none';
        });
        canvas.addEventListener('mouseleave', ()=>{ hover=-1; tip.style.display='none'; render(); });
      }
      window.addEventListener('mouseup', e=>{
        if(!canvas) return;
        const moved = Math.abs(e.clientX-downX) + Math.abs(e.clientY-downY) > 4;
        if(drag>=0 && !moved && onSelect) onSelect(drag); // click (not drag) -> jump to output
        if(drag>=0){ drag=-1; canvas.style.cursor = hover>=0?'pointer':'default'; resize(); }
      });

      return {
        get job(){ return job; },
        setOnSelect(fn){ onSelect=fn; },
        setJob(j){ job=j; runStatus=null; sel=-1; hover=-1; drag=-1;
          canvas=$('flow'); ctx=canvas.getContext('2d'); tip=$('fctip');
          parse(j.definition); bind(); resize(); },
        clear(){ job=null; canvas=null; ctx=null; nodes=[]; edges=[]; },
        applyRun(run){ // color nodes from a run's step results
          if(!canvas) return;
          const arr=run && Array.isArray(run.result)?run.result:[];
          runStatus=nodes.map(nd=>{ const sr=arr[nd.i]; return sr? !!sr.success : null; });
          render();
        },
        resize, render
      };
    })();

    // Clicking a flow-chart node opens and flashes that step's output card.
    flow.setOnSelect(function(i){
      const card=document.getElementById('out-step-'+i);
      if(!card) return;
      card.open=true;
      card.scrollIntoView({behavior:'smooth', block:'center'});
      card.classList.add('flash');
      setTimeout(()=>card.classList.remove('flash'), 1200);
    });
    window.addEventListener('resize', ()=>{ if(flow.job) flow.resize(); });

    // ---------- Boot ----------
    document.body.classList.remove('booting');
    if(getAuth()) enterApp(); else showLogin();
  })();
  </script>
</body>
</html>
)HTML";

} // namespace Orcha::Agent
