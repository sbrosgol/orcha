// States harness for the Orcha /admin dashboard: renders UI states that real
// data rarely produces, by serving fake API responses through Chrome's Fetch
// interception (only GET /api/circuit-breakers and GET /api/jobs/*/runs).
//
// Covers: every per-step rollback status (rolled_back, failed + message,
// skipped, not_supported), breakers in open / half_open / closed, HTML in step
// and command names (escaping), the breakers API failing (error state + Retry),
// an overview with only half-open circuits, the primary-tone confirm dialog,
// phone width, and an editor round-trip on the real "deploy-with-rollback" job
// (run seed.sh first). Saves PNGs to <outDir> and logs checks to stdout.
//
// usage: node states.mjs <baseUrl> <outDir> [width=1440] [height=900]
// env:   ADMIN_USER (default admin), ADMIN_PASS (default change-me),
//        CHROME (default macOS Google Chrome path), CDP_PORT (default 9333)
// exit:  0 = no JS errors, 1 = JS errors seen, 2 = harness failure
// note:  the editor round-trip changes deploy-with-rollback's definition;
//        re-run seed.sh on a fresh run dir afterwards if you need the original.
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';

const [base, out, W = '1440', H = '900'] = process.argv.slice(2);
if (!base || !out) { console.error('usage: node states.mjs <baseUrl> <outDir> [width] [height]'); process.exit(2); }
const USER = process.env.ADMIN_USER || 'admin';
const PASS = process.env.ADMIN_PASS || 'change-me';
const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const port = +(process.env.CDP_PORT || 9333);



mkdirSync(out, { recursive: true });
const profile = `${out}/.profile`;
rmSync(profile, { recursive: true, force: true });
const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
  '--no-first-run', `--window-size=${W},${H}`, 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const die = (msg) => { console.error(msg); chrome.kill(); process.exit(2); };

let ws, id = 0; const pending = new Map(); const errors = [];
for (let i = 0; i < 50 && !ws; i++) {
  try { const t = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    const pg = t.find(x => x.type === 'page'); if (pg) ws = new WebSocket(pg.webSocketDebuggerUrl); } catch {}
  if (!ws) await sleep(200);
}
if (!ws) die('Could not connect to headless Chrome (is CHROME correct?)');
await new Promise(r => ws.onopen = r);
ws.onmessage = (m) => { const d = JSON.parse(m.data);
  if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
  if (d.method === 'Runtime.exceptionThrown') errors.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
  if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') errors.push(d.params.args.map(a => a.value || a.description).join(' '));
};
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(`${out}/${name}.png`, Buffer.from(r.result.data, 'base64')); console.log('shot', `${out}/${name}.png`); };
// Click helper that tolerates missing elements (e.g. no jobs yet) and reports them.
const click = (sel) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return 'missing'; e.click(); return 'ok'; })()`)
  .then(r => { if (r === 'missing') console.log('  (skip: no element', sel + ')'); return r === 'ok'; });

await send('Runtime.enable'); await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: +W, height: +H, deviceScaleFactor: 1, mobile: +W < 600 });
await send('Page.navigate', { url: base + '/admin' }); await sleep(800);
await sleep(1); await ev(`document.getElementById('user').value=${JSON.stringify(USER)}; document.getElementById('pass').value=${JSON.stringify(PASS)}; document.getElementById('signin').click(); 1`);
await sleep(1500);
if (!(await ev(`document.body.classList.contains('authed')`))) die('Login failed');

const nowIso=new Date().toISOString(), hourAgo=new Date(Date.now()-3600e3).toISOString();
const run = {id:'abcdef1234567890', job_id:'x', trigger:'manual', status:'failed', started_at:nowIso, finished_at:nowIso, error:'Command exited with code 2',
 result:[
  {name:'prepare', command:'echo', success:true, error_message:'', output:{a:1}, rollback:{status:'rolled_back'}},
  {name:'migrate', command:'db_migrate', success:true, error_message:'', output:{b:2}, rollback:{status:'failed', message:'Down migration 0042 failed: relation "users_tmp" does not exist'}},
  {name:'notify', command:'slack', success:true, error_message:'', output:null, rollback:{status:'skipped', message:'Stopped after an earlier rollback failure'}},
  {name:'<img src=x onerror=console.error("XSS")>', command:'echo', success:true, error_message:'', output:{}, rollback:{status:'not_supported', message:'"><b>x</b>'}},
  {name:'deploy', command:'shell_exec', success:false, error_message:'Command exited with code 2', output:{}}]};
const brs = [{command:'shell_exec',state:'open',consecutive_failures:5,total_calls:5,failed_calls:5,rejected_calls:1,last_failure:nowIso},
 {command:'http_get',state:'half_open',consecutive_failures:0,total_calls:40,failed_calls:7,rejected_calls:12,last_failure:hourAgo},
 {command:'echo',state:'closed',consecutive_failures:0,total_calls:1200,failed_calls:0,rejected_calls:0,last_failure:null},
 {command:'"><img src=x onerror=console.error("XSS2")>',state:'open',consecutive_failures:1,total_calls:1,failed_calls:1,rejected_calls:0,last_failure:null}];

let FAKE = false; let MODE = 'normal'; // normal | fail | halfopen
await send('Fetch.enable', { patterns: [{ urlPattern: '*/api/circuit-breakers*' }, { urlPattern: '*/api/jobs/*/runs*' }] });
const b64 = (o) => Buffer.from(JSON.stringify(o)).toString('base64');
ws.addEventListener('message', async (m) => { const d = JSON.parse(m.data); if (d.method !== 'Fetch.requestPaused') return;
  const { requestId, request } = d.params;
  if (!FAKE || request.method !== 'GET') { send('Fetch.continueRequest', { requestId }); return; }
  let body;
  if (request.url.includes('/api/circuit-breakers')) {
    if (MODE === 'fail') { send('Fetch.fulfillRequest', { requestId, responseCode: 500, responseHeaders: [{ name: 'Content-Type', value: 'application/json' }], body: b64({ error: 'registry unavailable' }) }); return; }
    const list = MODE === 'halfopen' ? brs.filter(b => b.state !== 'open') : brs;
    body = { circuit_breakers: list, count: list.length };
  }
  else { const jid = decodeURIComponent(request.url.split('/api/jobs/')[1].split('/')[0]); body = { runs: [Object.assign({}, run, { job_id: jid })] }; }
  send('Fetch.fulfillRequest', { requestId, responseCode: 200, responseHeaders: [{ name: 'Content-Type', value: 'application/json' }], body: b64(body) });
});
FAKE = true; await ev(`document.getElementById('refreshBtn').click(); 1`); await sleep(800);
for (const theme of ['light','dark']) {
  await click(`.theme-switch button[data-theme=${theme}]`);
  await ev(`location.hash='#/jobs'; 1`); await sleep(600);
  await ev(`(() => { const e=[...document.querySelectorAll('.jitem')].find(x=>x.textContent.includes('deploy-with-rollback')); e.click(); return 1; })()`); await sleep(1500);
  await ev(`document.getElementById('runOutput').scrollIntoView(); 1`); await sleep(300);
  await shot(`${theme}-p1-rollback-statuses`);
  await ev(`location.hash='#/breakers'; 1`); await sleep(500); await shot(`${theme}-p2-breakers-states`);
  await ev(`location.hash='#/overview'; 1`); await sleep(600); await shot(`${theme}-p3-overview-multi`);
}
await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
await ev(`location.hash='#/breakers'; 1`); await sleep(500); await shot(`m-p2-breakers-states`);
await ev(`location.hash='#/jobs'; 1`); await sleep(600);
await ev(`(() => { const e=[...document.querySelectorAll('.jitem')].find(x=>x.textContent.includes('deploy-with-rollback')); e.click(); return 1; })()`); await sleep(1500);
await ev(`document.getElementById('runOutput').scrollIntoView(); 1`); await sleep(300); await shot(`m-p1-rollback-statuses`);
await send('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
// Error state + retry, half-open-only overview, primary-tone confirm.
MODE = 'fail'; await ev(`document.getElementById('refreshBtn').click(); 1`); await sleep(800);
await ev(`location.hash='#/breakers'; 1`); await sleep(400); await shot('p7-breakers-error');
await ev(`location.hash='#/overview'; 1`); await sleep(400); await shot('p8-overview-breakers-unavailable');
console.log('overview notice (fail):', await ev(`document.getElementById('ovAlerts').textContent.trim().slice(0,80)`));
MODE = 'halfopen'; await ev(`location.hash='#/breakers'; 1`); await sleep(300);
console.log('retry click', await click('button[data-act="retry-breakers"]')); await sleep(800);
console.log('rows after retry:', await ev(`document.querySelectorAll('#breakerRows tr').length`));
await ev(`location.hash='#/overview'; 1`); await sleep(400); await shot('p9-overview-halfopen');
console.log('overview notice (halfopen):', await ev(`document.getElementById('ovAlerts').innerHTML.includes('alert err') ? 'err-tone' : 'warn-tone'`));
await ev(`location.hash='#/breakers'; 1`); await sleep(300); await click('button[data-br]'); await sleep(300);
console.log('confirm ok class:', await ev(`document.getElementById('cfOk').className`)); await shot('p10-confirm-primary');
await click('#cfCancel'); await sleep(200);
MODE = 'normal'; await ev(`document.getElementById('refreshBtn').click(); 1`); await sleep(800);
// Editor round-trip on the real job
console.log('edit click', await click('#editJob')); await sleep(1000);
console.log('modal open', await ev(`document.getElementById('modal').classList.contains('show')`));
console.log('rollback ctl', await ev(`document.getElementById('jRollback').value`), 'breaker', await ev(`document.getElementById('jBreaker').checked`));
await ev(`document.getElementById('jBreaker').click(); 1`); await sleep(100);
console.log('after breaker toggle (visual):', await ev(`document.getElementById('jDef').value`));
await shot('p5-editor-visual');
await ev(`document.querySelector('.tab[data-deftab=json]').click(); 1`); await sleep(100);
await ev(`(()=>{const d=document.getElementById('jDef'); d.value=d.value.replace('"reverse"','"none"'); d.dispatchEvent(new Event('input',{bubbles:true})); return 1;})()`); await sleep(400);
console.log('ctl after JSON rollback="none":', await ev(`document.getElementById('jRollback').value`));
await ev(`(()=>{const d=document.getElementById('jDef'); d.value=d.value.replace('"circuit_breaker": true','"circuit_breaker": "yes"'); d.dispatchEvent(new Event('input',{bubbles:true})); return 1;})()`); await sleep(400);
console.log('ctl after circuit_breaker:"yes":', await ev(`document.getElementById('jBreaker').checked`));
await ev(`(()=>{const d=document.getElementById('jDef'); d.value='{"rollback":"reverse", broken'; d.dispatchEvent(new Event('input',{bubbles:true})); return 1;})()`); await sleep(400);
console.log('before toggle on broken JSON: rollback', await ev(`document.getElementById('jRollback').value`));
await ev(`document.getElementById('jBreaker').click(); 1`); await sleep(100);
console.log('after toggle on broken JSON: breaker', await ev(`document.getElementById('jBreaker').checked`), 'rollback', await ev(`document.getElementById('jRollback').value`));
await shot('p6-editor-brokenjson');
console.log('JS ERRORS:', errors.length ? JSON.stringify(errors, null, 2) : 'none');
chrome.kill(); process.exit(errors.length ? 1 : 0);
