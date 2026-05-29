#pragma once

namespace Orcha::Agent {

// Embedded admin dashboard served at /admin. Vanilla HTML/JS, no build step,
// no external assets. Talks to the /api/plugins and /api/jobs JSON APIs.
//
// Features: custom login view, Light/Dark/System themes, Plugins admin, Jobs
// (CRUD + run + history), and an interactive HTML Canvas workflow flow chart.
inline constexpr const char kDashboardHtml[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Orcha Admin</title>
  <script>document.documentElement.dataset.theme = localStorage.getItem('orcha_theme') || 'system';</script>
  <style>
    :root {
      --bg:#f6f8fa; --panel:#ffffff; --line:#d0d7de; --fg:#1f2328;
      --muted:#656d76; --accent:#0969da; --on-accent:#ffffff;
      --ok:#1a7f37; --warn:#9a6700; --err:#cf222e;
      --shadow:0 1px 3px rgba(27,31,36,.12), 0 8px 24px rgba(27,31,36,.08);
    }
    :root[data-theme="dark"] {
      --bg:#0f1419; --panel:#1a2029; --line:#2a323d; --fg:#e6edf3;
      --muted:#8b97a5; --accent:#4493f8; --on-accent:#0b1220;
      --ok:#3fb950; --warn:#d29922; --err:#f85149;
      --shadow:0 1px 3px rgba(0,0,0,.5), 0 8px 24px rgba(0,0,0,.4);
    }
    @media (prefers-color-scheme: dark) {
      :root[data-theme="system"] {
        --bg:#0f1419; --panel:#1a2029; --line:#2a323d; --fg:#e6edf3;
        --muted:#8b97a5; --accent:#4493f8; --on-accent:#0b1220;
        --ok:#3fb950; --warn:#d29922; --err:#f85149;
        --shadow:0 1px 3px rgba(0,0,0,.5), 0 8px 24px rgba(0,0,0,.4);
      }
    }
    * { box-sizing: border-box; }
    body { margin:0; font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;
           background:var(--bg); color:var(--fg); }
    header { padding:14px 24px; border-bottom:1px solid var(--line);
             display:flex; align-items:center; gap:18px; }
    header h1 { font-size:17px; margin:0; font-weight:600; }
    nav { display:flex; gap:4px; }
    nav button { background:none; border:none; color:var(--muted); padding:6px 12px;
                 border-radius:6px; cursor:pointer; font-size:14px; }
    nav button.active { color:var(--fg); background:var(--bg); font-weight:600; }
    .spacer { flex:1; }
    main { padding:24px; max-width:1180px; margin:0 auto; }
    button.btn { background:var(--panel); color:var(--fg); border:1px solid var(--line);
                 border-radius:6px; padding:6px 12px; cursor:pointer; font-size:13px; }
    button.btn:hover { border-color:var(--accent); }
    button.btn.primary { background:var(--accent); color:var(--on-accent); border-color:var(--accent); }
    button.btn.danger:hover { border-color:var(--err); color:var(--err); }
    select, input[type=text], input[type=password], textarea {
      background:var(--bg); color:var(--fg); border:1px solid var(--line);
      border-radius:6px; padding:8px 10px; font-size:13px; font-family:inherit; }
    textarea { width:100%; min-height:200px; font-family:ui-monospace,SFMono-Regular,Menlo,monospace; }
    input:focus, textarea:focus, select:focus { outline:none; border-color:var(--accent); }
    table { width:100%; border-collapse:collapse; background:var(--panel);
            border:1px solid var(--line); border-radius:8px; overflow:hidden; }
    th,td { text-align:left; padding:10px 14px; border-bottom:1px solid var(--line); vertical-align:top; }
    th { color:var(--muted); font-weight:500; font-size:12px; text-transform:uppercase; }
    tr:last-child td { border-bottom:none; }
    .badge { display:inline-block; padding:2px 8px; border-radius:999px; font-size:11px; font-weight:600; }
    .badge.loaded,.badge.success { background:color-mix(in srgb,var(--ok) 18%,transparent); color:var(--ok); }
    .badge.available { background:color-mix(in srgb,var(--warn) 18%,transparent); color:var(--warn); }
    .badge.failed { background:color-mix(in srgb,var(--err) 18%,transparent); color:var(--err); }
    .tags span { background:var(--bg); border:1px solid var(--line); border-radius:4px;
                 padding:1px 6px; margin-right:4px; font-size:11px; color:var(--muted); }
    .actions { display:flex; gap:6px; flex-wrap:wrap; }
    .muted { color:var(--muted); }
    .toolbar { display:flex; align-items:center; gap:12px; margin-bottom:16px; }
    .grid { display:grid; grid-template-columns: 320px 1fr; gap:20px; }
    .panel { background:var(--panel); border:1px solid var(--line); border-radius:8px; }
    .panel h3 { margin:0; padding:12px 14px; border-bottom:1px solid var(--line); font-size:13px;
                text-transform:uppercase; color:var(--muted); letter-spacing:.03em; }
    .joblist .item { padding:10px 14px; border-bottom:1px solid var(--line); cursor:pointer; }
    .joblist .item:last-child { border-bottom:none; }
    .joblist .item.active { background:var(--bg); }
    .joblist .item .nm { font-weight:600; }
    .pad { padding:14px; }
    #toast { position:fixed; bottom:20px; right:20px; padding:10px 16px; border-radius:6px;
             background:var(--panel); border:1px solid var(--line); display:none; max-width:380px;
             box-shadow:var(--shadow); z-index:80; }
    #toast.err { border-color:var(--err); color:var(--err); }
    #toast.ok { border-color:var(--ok); color:var(--ok); }
    .theme-ctl { display:flex; align-items:center; gap:6px; }
    .theme-ctl > span { font-size:11px; color:var(--muted); }

    /* Login */
    #login { position:fixed; inset:0; z-index:50; background:var(--bg);
             display:flex; align-items:center; justify-content:center; padding:20px; }
    .card { background:var(--panel); border:1px solid var(--line); border-radius:12px;
            padding:30px 28px; width:340px; box-shadow:var(--shadow); }
    .card .logo { width:38px; height:38px; border-radius:9px; background:var(--accent);
                  display:flex; align-items:center; justify-content:center; color:var(--on-accent);
                  font-weight:700; font-size:18px; margin-bottom:16px; }
    .card h2 { margin:0 0 4px; font-size:19px; }
    .card .sub { color:var(--muted); margin:0 0 20px; font-size:13px; }
    .field { margin-bottom:14px; }
    .field label { display:block; font-size:12px; color:var(--muted); margin-bottom:6px; }
    .field input { width:100%; }
    .btn-primary { width:100%; background:var(--accent); color:var(--on-accent); border:none;
      padding:10px; border-radius:6px; font-size:14px; cursor:pointer; font-weight:600; margin-top:4px; }
    .login-err { color:var(--err); font-size:13px; margin-top:12px; min-height:18px; }

    body:not(.authed) #app { display:none; }
    body.authed #login { display:none; }
    #logout { display:none; } body.authed #logout { display:inline-block; }
    .view { display:none; } .view.active { display:block; }

    /* Flow chart */
    .chartwrap { position:relative; overflow:auto; background:var(--bg);
                 border:1px solid var(--line); border-radius:8px; }
    #flow { display:block; }
    #fctip { position:absolute; pointer-events:none; display:none; max-width:280px;
             background:var(--panel); border:1px solid var(--line); border-radius:6px;
             padding:8px 10px; font-size:12px; box-shadow:var(--shadow); z-index:5; white-space:pre-wrap; }

    /* Modal */
    #modal { position:fixed; inset:0; z-index:70; background:rgba(0,0,0,.45);
             display:none; align-items:center; justify-content:center; padding:20px; }
    #modal.show { display:flex; }
    .modal-card { background:var(--panel); border:1px solid var(--line); border-radius:12px;
                  width:640px; max-width:100%; max-height:90vh; overflow:auto; box-shadow:var(--shadow); }
    .modal-card .hd { padding:16px 20px; border-bottom:1px solid var(--line); font-weight:600; }
    .modal-card .bd { padding:20px; } .modal-card .ft { padding:16px 20px; border-top:1px solid var(--line);
                  display:flex; gap:8px; justify-content:flex-end; }
    .runrow { cursor:pointer; }
    .mono { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:12px;
            white-space:pre-wrap; word-break:break-word; }
  </style>
</head>
<body>
  <div id="login">
    <form class="card" id="loginForm" autocomplete="on">
      <div class="logo">O</div>
      <h2>Orcha Admin</h2>
      <p class="sub">Sign in to manage plugins and jobs.</p>
      <div class="field"><label for="user">Username</label>
        <input id="user" type="text" name="username" autocomplete="username" required /></div>
      <div class="field"><label for="pass">Password</label>
        <input id="pass" type="password" name="current-password" autocomplete="current-password" required /></div>
      <button class="btn-primary" type="submit" id="signin">Sign in</button>
      <div class="login-err" id="loginErr"></div>
      <div class="theme-ctl" style="margin-top:18px; justify-content:center;">
        <span>Theme</span>
        <select class="theme-select" aria-label="Theme">
          <option value="system">System</option><option value="light">Light</option><option value="dark">Dark</option>
        </select>
      </div>
    </form>
  </div>

  <div id="app">
    <header>
      <h1>Orcha Admin</h1>
      <nav>
        <button data-view="plugins" class="active">Plugins</button>
        <button data-view="jobs">Jobs</button>
      </nav>
      <span class="spacer"></span>
      <label class="theme-ctl"><span>Theme</span>
        <select class="theme-select" aria-label="Theme">
          <option value="system">System</option><option value="light">Light</option><option value="dark">Dark</option>
        </select></label>
      <span class="muted" id="who"></span>
      <button class="btn" id="logout">Sign out</button>
    </header>
    <main>
      <!-- Plugins view -->
      <section class="view active" id="view-plugins">
        <div class="toolbar">
          <button class="btn" id="refresh">Refresh</button>
          <span class="muted" id="count"></span>
          <label style="margin-left:auto; display:flex; align-items:center; gap:8px;">
            Watch directory <input type="checkbox" id="watch" /></label>
        </div>
        <table>
          <thead><tr><th>Plugin</th><th>Version</th><th>Status</th><th>Dependencies</th><th>Tags</th><th>Actions</th></tr></thead>
          <tbody id="rows"></tbody>
        </table>
      </section>

      <!-- Jobs view -->
      <section class="view" id="view-jobs">
        <div class="toolbar">
          <button class="btn primary" id="newJob">New job</button>
          <button class="btn" id="refreshJobs">Refresh</button>
          <span class="muted" id="jobsCount"></span>
        </div>
        <div class="grid">
          <div class="panel joblist"><h3>Jobs</h3><div id="jobItems"></div></div>
          <div class="panel"><h3>Detail</h3><div class="pad" id="jobDetail">
            <span class="muted">Select a job to view its workflow and run history.</span></div></div>
        </div>
      </section>
    </main>
  </div>

  <!-- Editor modal -->
  <div id="modal">
    <div class="modal-card">
      <div class="hd" id="modalTitle">New job</div>
      <div class="bd">
        <div class="field"><label>Name</label><input id="jName" type="text" style="width:100%" /></div>
        <div class="field"><label>Description</label><input id="jDesc" type="text" style="width:100%" /></div>
        <div class="field"><label>Schedule (cron, optional &mdash; "m h dom mon dow")</label>
          <input id="jSchedule" type="text" placeholder="e.g. 0 9 * * 1-5  (blank = manual only)" style="width:100%" /></div>
        <div class="field"><label style="display:flex; align-items:center; gap:8px; color:var(--fg)">
          <input id="jEnabled" type="checkbox" /> Enabled (the scheduler runs this job when due)</label></div>
        <div class="field"><label>Definition (JSON with a "steps" array)</label>
          <textarea id="jDef" spellcheck="false"></textarea></div>
        <div class="login-err" id="modalErr"></div>
      </div>
      <div class="ft">
        <button class="btn" id="modalCancel">Cancel</button>
        <button class="btn primary" id="modalSave">Save</button>
      </div>
    </div>
  </div>

  <div id="toast"></div>

  <script>
    const $ = (id) => document.getElementById(id);
    const esc = (s) => String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

    // ---------- Theme (multiple synced selectors: header + login) ----------
    const THEME_KEY='orcha_theme';
    function setTheme(t){
      document.documentElement.dataset.theme=t;
      localStorage.setItem(THEME_KEY, t);
      document.querySelectorAll('.theme-select').forEach(s=>{ s.value=t; });
      if (flow.job) flow.render(); // recolor for the new theme
    }
    (function(){
      const t=localStorage.getItem(THEME_KEY)||'system';
      document.documentElement.dataset.theme=t;
      document.querySelectorAll('.theme-select').forEach(s=>{
        s.value=t;
        s.addEventListener('change', e=>setTheme(e.target.value));
      });
    })();

    // ---------- Toast ----------
    function toast(msg, kind){ const t=$('toast'); t.textContent=msg; t.className=kind||'';
      t.style.display='block'; clearTimeout(toast._t); toast._t=setTimeout(()=>t.style.display='none',4000); }

    // ---------- Auth ----------
    const AUTH_KEY='orcha_auth';
    const getAuth=()=>sessionStorage.getItem(AUTH_KEY);
    const setAuth=(v)=>sessionStorage.setItem(AUTH_KEY,v);
    const clearAuth=()=>sessionStorage.removeItem(AUTH_KEY);
    const b64=(s)=>btoa(unescape(encodeURIComponent(s)));

    async function api(path, opts){
      opts=opts||{}; const headers=Object.assign({}, opts.headers||{});
      const auth=getAuth(); if(auth) headers['Authorization']=auth;
      const res=await fetch(path, Object.assign({}, opts, {headers}));
      if(res.status===401){ clearAuth(); showLogin('Your session is invalid or has expired.'); throw new Error('Unauthorized'); }
      let body=null; try{ body=await res.json(); }catch(e){}
      if(!res.ok) throw new Error((body&&body.error)||('HTTP '+res.status));
      return body;
    }
    function showLogin(msg){ document.body.classList.remove('authed'); $('loginErr').textContent=msg||'';
      $('pass').value=''; setTimeout(()=>$('user').focus(),0); }
    function enterApp(){ document.body.classList.add('authed'); loadPlugins(); loadJobs(); }

    $('loginForm').addEventListener('submit', async e => {
      e.preventDefault();
      const user=$('user').value.trim(), pass=$('pass').value, btn=$('signin');
      btn.disabled=true; btn.textContent='Signing in...'; $('loginErr').textContent='';
      try {
        const token='Basic '+b64(user+':'+pass);
        const res=await fetch('/api/plugins',{headers:{Authorization:token}});
        if(res.status===401){ $('loginErr').textContent='Invalid username or password.'; return; }
        if(!res.ok){ $('loginErr').textContent='Server error (HTTP '+res.status+').'; return; }
        setAuth(token); $('who').textContent=user; enterApp();
      } catch(err){ $('loginErr').textContent='Could not reach the server.'; }
      finally { btn.disabled=false; btn.textContent='Sign in'; }
    });
    $('logout').addEventListener('click', ()=>{ clearAuth(); $('rows').innerHTML=''; $('jobItems').innerHTML='';
      showLogin('Signed out.'); });

    // ---------- View tabs ----------
    document.querySelectorAll('nav button').forEach(b => b.addEventListener('click', ()=>{
      document.querySelectorAll('nav button').forEach(x=>x.classList.remove('active'));
      b.classList.add('active');
      document.querySelectorAll('.view').forEach(v=>v.classList.remove('active'));
      $('view-'+b.dataset.view).classList.add('active');
      if (b.dataset.view==='jobs' && flow.job) flow.resize();
    }));

    // ================= Plugins =================
    function pRow(p){
      const deps=(p.dependencies||[]).map(esc).join(', ')||'<span class="muted">-</span>';
      const tags=(p.tags||[]).map(t=>'<span>'+esc(t)+'</span>').join('')||'<span class="muted">-</span>';
      const actions = p.status==='loaded'
        ? `<button class="btn" data-pa="reload" data-name="${esc(p.name)}">Reload</button>
           <button class="btn" data-pa="disable" data-name="${esc(p.name)}">Disable</button>`
        : `<button class="btn" data-pa="enable" data-name="${esc(p.name)}">Enable</button>`;
      return `<tr><td><strong>${esc(p.name)}</strong><br><span class="muted">${esc(p.description||'')}</span></td>
        <td>${esc(p.version||'')}</td><td><span class="badge ${esc(p.status)}">${esc(p.status)}</span></td>
        <td>${deps}</td><td class="tags">${tags}</td><td class="actions">${actions}</td></tr>`;
    }
    async function loadPlugins(){
      try { const d=await api('/api/plugins');
        $('count').textContent=(d.count||0)+' plugin(s), '+((d.commands||[]).length)+' command(s)';
        $('watch').checked=!!d.watching;
        $('rows').innerHTML=(d.plugins||[]).map(pRow).join('')||'<tr><td colspan="6" class="muted">No plugins found.</td></tr>';
      } catch(e){ if(e.message!=='Unauthorized') toast('Failed to load plugins: '+e.message,'err'); }
    }
    async function pluginAct(name, action){
      try { const r=await api('/api/plugins/'+encodeURIComponent(name)+'/'+action,{method:'POST'});
        toast((r&&r.message)||(action+' ok'),'ok'); await loadPlugins();
      } catch(e){ if(e.message!=='Unauthorized') toast(action+' failed: '+e.message,'err'); }
    }
    $('refresh').addEventListener('click', loadPlugins);
    $('watch').addEventListener('change', async e=>{
      try { await api('/api/plugins/_watch',{method:'PUT',headers:{'Content-Type':'application/json'},
              body:JSON.stringify({enabled:e.target.checked})});
        toast('Watcher '+(e.target.checked?'enabled':'disabled'),'ok');
      } catch(err){ if(err.message!=='Unauthorized') toast('Watcher toggle failed: '+err.message,'err');
        e.target.checked=!e.target.checked; }
    });

    // ================= Jobs =================
    let jobs=[], currentJob=null;

    async function loadJobs(){
      try { const d=await api('/api/jobs'); jobs=d.jobs||[];
        $('jobsCount').textContent=jobs.length+' job(s)';
        $('jobItems').innerHTML = jobs.length ? jobs.map(j =>
          `<div class="item ${currentJob&&currentJob.id===j.id?'active':''}" data-job="${esc(j.id)}">
             <div class="nm">${esc(j.name)}${j.enabled?'':' <span class="muted">(disabled)</span>'}${j.schedule_cron?' <span class="muted">&#9201;</span>':''}</div>
             <div class="muted">${esc(j.description||'')}</div></div>`).join('')
          : '<div class="pad muted">No jobs yet. Click "New job".</div>';
      } catch(e){ if(e.message!=='Unauthorized') toast('Failed to load jobs: '+e.message,'err'); }
    }

    async function selectJob(id){
      try {
        currentJob = await api('/api/jobs/'+encodeURIComponent(id));
        document.querySelectorAll('.joblist .item').forEach(x =>
          x.classList.toggle('active', x.dataset.job===id));
        renderJobDetail();
      } catch(e){ if(e.message!=='Unauthorized') toast(e.message,'err'); }
    }

    function renderJobDetail(){
      const j=currentJob; if(!j){ return; }
      const sched = j.schedule_cron
        ? `<code>${esc(j.schedule_cron)}</code>` : '<span class="muted">manual only</span>';
      const enBadge = j.enabled
        ? '<span class="badge success">enabled</span>'
        : '<span class="badge failed">disabled</span>';
      $('jobDetail').innerHTML = `
        <div class="toolbar">
          <strong style="font-size:15px">${esc(j.name)}</strong>
          <span class="muted">${esc(j.description||'')}</span>
          <span style="margin-left:auto"></span>
          <button class="btn primary" id="runJob">Run now</button>
          <button class="btn" id="toggleJob">${j.enabled ? 'Disable' : 'Enable'}</button>
          <button class="btn" id="editJob">Edit</button>
          <button class="btn danger" id="delJob">Delete</button>
        </div>
        <div class="muted" style="margin:-6px 0 12px; display:flex; gap:14px; align-items:center">
          <span>Schedule: ${sched}</span> ${enBadge}
        </div>
        <div class="chartwrap"><canvas id="flow"></canvas><div id="fctip"></div></div>
        <h3 style="margin:18px 0 8px; font-size:12px; text-transform:uppercase; color:var(--muted)">Run history</h3>
        <div id="runs" class="muted">Loading runs...</div>`;
      $('runJob').onclick = ()=>runJob(j.id);
      $('toggleJob').onclick = ()=>toggleJob(j);
      $('editJob').onclick = ()=>openEditor(j);
      $('delJob').onclick = ()=>delJob(j);
      flow.setJob(j);
      loadRuns(j.id);
    }

    async function toggleJob(j){
      const payload = { name:j.name, description:j.description||'',
                        definition:j.definition, enabled:!j.enabled };
      if(j.schedule_cron) payload.schedule_cron = j.schedule_cron;
      try {
        await api('/api/jobs/'+encodeURIComponent(j.id),
          {method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)});
        toast(j.enabled ? 'Disabled' : 'Enabled', 'ok');
        await loadJobs();
        await selectJob(j.id);
      } catch(e){ if(e.message!=='Unauthorized') toast('Toggle failed: '+e.message,'err'); }
    }

    async function runJob(id){
      try { const r=await api('/api/jobs/'+encodeURIComponent(id)+'/run',{method:'POST'});
        toast('Run '+r.status,(r.status==='success')?'ok':'err');
        flow.applyRun(r); loadRuns(id);
      } catch(e){ if(e.message!=='Unauthorized') toast('Run failed: '+e.message,'err'); }
    }
    async function delJob(j){
      if(!confirm('Delete job "'+j.name+'"?')) return;
      try { await api('/api/jobs/'+encodeURIComponent(j.id),{method:'DELETE'});
        toast('Deleted','ok'); currentJob=null;
        $('jobDetail').innerHTML='<span class="muted">Select a job to view its workflow and run history.</span>';
        loadJobs();
      } catch(e){ if(e.message!=='Unauthorized') toast('Delete failed: '+e.message,'err'); }
    }
    async function loadRuns(id){
      try { const d=await api('/api/jobs/'+encodeURIComponent(id)+'/runs?limit=20');
        const runs=d.runs||[];
        $('runs').innerHTML = runs.length ? `<table><thead><tr><th>Started</th><th>Trigger</th><th>Status</th></tr></thead>
          <tbody>${runs.map(r=>`<tr class="runrow" data-run="${esc(r.id)}"><td>${esc(r.started_at)}</td>
            <td>${esc(r.trigger)}</td><td><span class="badge ${esc(r.status)}">${esc(r.status)}</span></td></tr>`).join('')}
          </tbody></table>` : '<span class="muted">No runs yet.</span>';
      } catch(e){ if(e.message!=='Unauthorized') $('runs').textContent='Failed to load runs.'; }
    }
    async function showRun(id){
      try { const r=await api('/api/runs/'+encodeURIComponent(id));
        flow.applyRun(r);
        toast('Loaded run '+r.id.slice(0,8)+' ('+r.status+')',(r.status==='success')?'ok':'err');
      } catch(e){ if(e.message!=='Unauthorized') toast(e.message,'err'); }
    }

    $('newJob').addEventListener('click', ()=>openEditor(null));
    $('refreshJobs').addEventListener('click', loadJobs);

    // Delegated clicks for plugin actions, job selection, run rows.
    document.addEventListener('click', ev=>{
      const pa=ev.target.closest('button[data-pa]'); if(pa){ pluginAct(pa.dataset.name, pa.dataset.pa); return; }
      const ji=ev.target.closest('.joblist .item'); if(ji){ selectJob(ji.dataset.job); return; }
      const rr=ev.target.closest('.runrow'); if(rr){ showRun(rr.dataset.run); return; }
    });

    // ---------- Editor modal ----------
    let editingId=null;
    const DEFAULT_DEF = JSON.stringify({steps:[
      {command:"echo", params:{message:"Hello from Orcha"}},
      {command:"echo", params:{message:"step 1 said: {{step1.output.echoed}}"}}
    ]}, null, 2);
    function openEditor(job){
      editingId = job ? job.id : null;
      $('modalTitle').textContent = job ? 'Edit job' : 'New job';
      $('jName').value = job ? job.name : '';
      $('jDesc').value = job ? (job.description||'') : '';
      $('jSchedule').value = (job && job.schedule_cron) ? job.schedule_cron : '';
      $('jEnabled').checked = job ? !!job.enabled : true;
      $('jDef').value  = job ? JSON.stringify(job.definition, null, 2) : DEFAULT_DEF;
      $('modalErr').textContent='';
      $('modal').classList.add('show');
    }
    $('modalCancel').addEventListener('click', ()=>$('modal').classList.remove('show'));
    $('modalSave').addEventListener('click', async ()=>{
      let def;
      try { def=JSON.parse($('jDef').value); }
      catch(e){ $('modalErr').textContent='Definition is not valid JSON: '+e.message; return; }
      const payload={ name:$('jName').value.trim(), description:$('jDesc').value.trim(),
                      definition:def, enabled:$('jEnabled').checked };
      const sched=$('jSchedule').value.trim();
      if(sched) payload.schedule_cron=sched;   // omitted => cleared (manual only)
      if(!payload.name){ $('modalErr').textContent='Name is required.'; return; }
      try {
        if(editingId){ await api('/api/jobs/'+encodeURIComponent(editingId),
            {method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}); }
        else { await api('/api/jobs',
            {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}); }
        $('modal').classList.remove('show'); toast('Saved','ok');
        await loadJobs();
        if(editingId) await selectJob(editingId);
      } catch(e){ if(e.message!=='Unauthorized') $('modalErr').textContent=e.message; }
    });

    // ================= Flow chart (Canvas) =================
    const flow = (function(){
      const NODE_W=158, NODE_H=46, COL_GAP=66, ROW_GAP=22, PAD=20;
      let canvas=null, ctx=null, tip=null;
      let nodes=[], edges=[], job=null, runStatus=null; // runStatus: array of bool|null per step
      let hover=-1, sel=-1, drag=-1, dragDX=0, dragDY=0, dpr=1;

      function cssVar(n){ return getComputedStyle(document.documentElement).getPropertyValue(n).trim(); }

      function parse(def){
        const steps=(def&&def.steps)||[];
        const n=steps.length;
        const deps=Array.from({length:n},()=>new Set());
        for(let i=0;i<n;i++){
          const s=JSON.stringify(steps[i].params||{});
          const re=/\{\{\s*step(\d+)/g; let m;
          while((m=re.exec(s))){ const k=parseInt(m[1],10)-1; if(k>=0&&k<n&&k!==i) deps[i].add(k); }
        }
        const level=new Array(n).fill(0);
        for(let it=0;it<n;it++) for(let i=0;i<n;i++) for(const d of deps[i]) level[i]=Math.max(level[i],level[d]+1);
        const rowOf={}; nodes=[]; edges=[];
        for(let i=0;i<n;i++){
          const L=level[i]; rowOf[L]=(rowOf[L]||0);
          const row=rowOf[L]++;
          nodes.push({ i, cmd:(steps[i].command||'?'), params:steps[i].params||{},
            x:PAD+L*(NODE_W+COL_GAP), y:PAD+row*(NODE_H+ROW_GAP) });
        }
        for(let i=0;i<n;i++) for(const d of deps[i]) edges.push([d,i]);
      }

      function resize(){
        if(!canvas) return;
        let maxX=300, maxY=120;
        nodes.forEach(nd=>{ maxX=Math.max(maxX, nd.x+NODE_W+PAD); maxY=Math.max(maxY, nd.y+NODE_H+PAD); });
        dpr=window.devicePixelRatio||1;
        canvas.style.width=maxX+'px'; canvas.style.height=maxY+'px';
        canvas.width=maxX*dpr; canvas.height=maxY*dpr;
        ctx.setTransform(dpr,0,0,dpr,0,0);
        render();
      }

      function nodeColor(nd){
        if(runStatus){ const st=runStatus[nd.i];
          if(st===true) return {bg:'color-mix', stroke:cssVar('--ok')};
          if(st===false) return {bg:'color-mix', stroke:cssVar('--err')}; }
        return {bg:null, stroke: (nd.i===sel)?cssVar('--accent'):cssVar('--line')};
      }

      function roundRect(x,y,w,h,r){ ctx.beginPath(); ctx.moveTo(x+r,y);
        ctx.arcTo(x+w,y,x+w,y+h,r); ctx.arcTo(x+w,y+h,x,y+h,r);
        ctx.arcTo(x,y+h,x,y,r); ctx.arcTo(x,y,x+w,y,r); ctx.closePath(); }

      function render(){
        if(!ctx) return;
        ctx.clearRect(0,0,canvas.width,canvas.height);
        const line=cssVar('--line'), fg=cssVar('--fg'), muted=cssVar('--muted'),
              panel=cssVar('--panel'), accent=cssVar('--accent');
        // edges
        ctx.lineWidth=1.5;
        edges.forEach(([a,b])=>{
          const na=nodes[a], nb=nodes[b];
          const x1=na.x+NODE_W, y1=na.y+NODE_H/2, x2=nb.x, y2=nb.y+NODE_H/2;
          const hl = (hover===a||hover===b||sel===a||sel===b);
          ctx.strokeStyle = hl?accent:line;
          ctx.beginPath(); ctx.moveTo(x1,y1);
          const mx=(x1+x2)/2; ctx.bezierCurveTo(mx,y1,mx,y2,x2,y2); ctx.stroke();
          // arrowhead (points left into the target node's left edge)
          ctx.fillStyle=hl?accent:line;
          ctx.beginPath(); ctx.moveTo(x2,y2); ctx.lineTo(x2-7,y2-4); ctx.lineTo(x2-7,y2+4); ctx.closePath(); ctx.fill();
        });
        // nodes
        nodes.forEach(nd=>{
          const c=nodeColor(nd);
          roundRect(nd.x,nd.y,NODE_W,NODE_H,8);
          ctx.fillStyle=panel; ctx.fill();
          if(runStatus && runStatus[nd.i]!==null && runStatus[nd.i]!==undefined){
            roundRect(nd.x,nd.y,NODE_W,NODE_H,8);
            ctx.fillStyle = (runStatus[nd.i]? 'rgba(63,185,80,.14)':'rgba(248,81,73,.14)'); ctx.fill();
          }
          ctx.lineWidth=(nd.i===sel||nd.i===hover)?2:1.2;
          ctx.strokeStyle=(nd.i===hover)?accent:c.stroke; ctx.stroke();
          ctx.fillStyle=muted; ctx.font='600 11px -apple-system,sans-serif';
          ctx.fillText('STEP '+(nd.i+1), nd.x+12, nd.y+17);
          ctx.fillStyle=fg; ctx.font='13px -apple-system,sans-serif';
          let label=nd.cmd; if(label.length>18) label=label.slice(0,17)+'…';
          ctx.fillText(label, nd.x+12, nd.y+34);
        });
      }

      function hit(mx,my){ for(let i=nodes.length-1;i>=0;i--){ const n=nodes[i];
        if(mx>=n.x&&mx<=n.x+NODE_W&&my>=n.y&&my<=n.y+NODE_H) return i; } return -1; }

      function bind(){
        canvas.addEventListener('mousedown', e=>{ const r=canvas.getBoundingClientRect();
          const mx=e.clientX-r.left, my=e.clientY-r.top; const i=hit(mx,my);
          sel=i; if(i>=0){ drag=i; dragDX=mx-nodes[i].x; dragDY=my-nodes[i].y; } render(); });
        canvas.addEventListener('mousemove', e=>{ const r=canvas.getBoundingClientRect();
          const mx=e.clientX-r.left, my=e.clientY-r.top;
          if(drag>=0){ nodes[drag].x=mx-dragDX; nodes[drag].y=my-dragDY; render(); return; }
          const i=hit(mx,my);
          if(i!==hover){ hover=i; canvas.style.cursor=i>=0?'grab':'default'; render(); }
          if(i>=0){ const nd=nodes[i];
            tip.style.display='block'; tip.style.left=(nd.x+NODE_W+8)+'px'; tip.style.top=nd.y+'px';
            tip.textContent='step'+(i+1)+' · '+nd.cmd+'\n'+JSON.stringify(nd.params,null,1).slice(0,260);
          } else tip.style.display='none';
        });
        window.addEventListener('mouseup', ()=>{ drag=-1; });
        canvas.addEventListener('mouseleave', ()=>{ hover=-1; tip.style.display='none'; render(); });
      }

      return {
        get job(){ return job; },
        setJob(j){ job=j; runStatus=null; sel=-1; hover=-1;
          canvas=$('flow'); ctx=canvas.getContext('2d'); tip=$('fctip');
          parse(j.definition); bind(); resize(); },
        applyRun(run){ // color nodes from a run's step results
          const arr=Array.isArray(run.result)?run.result:[];
          runStatus=nodes.map(nd=>{ const sr=arr[nd.i]; return sr? !!sr.success : null; });
          render();
        },
        resize, render
      };
    })();

    // ---------- Boot ----------
    if(getAuth()) enterApp(); else showLogin();
  </script>
</body>
</html>
)HTML";

} // namespace Orcha::Agent
