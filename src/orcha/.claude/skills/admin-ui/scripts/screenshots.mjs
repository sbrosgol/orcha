// Headless-Chrome screenshot + JS-error harness for the Orcha /admin dashboard.
// Signs in through the real login form, walks every view in light and dark
// themes, saves PNGs and prints any uncaught exceptions / console.error calls.
//
// usage: node screenshots.mjs <baseUrl> <outDir> [width=1440] [height=900]
// env:   ADMIN_USER (default admin), ADMIN_PASS (default change-me),
//        CHROME (default macOS Google Chrome path), CDP_PORT (default 9333)
// exit:  0 = no JS errors, 1 = JS errors seen, 2 = harness failure
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';

const [base, out, W = '1440', H = '900'] = process.argv.slice(2);
if (!base || !out) { console.error('usage: node screenshots.mjs <baseUrl> <outDir> [width] [height]'); process.exit(2); }
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
await shot('00-login');
await ev(`document.getElementById('user').value=${JSON.stringify(USER)}; document.getElementById('pass').value=${JSON.stringify(PASS)}; document.getElementById('signin').click(); 1`);
await sleep(1500);
if (!(await ev(`document.body.classList.contains('authed')`))) die('Login failed — check ADMIN_USER / ADMIN_PASS and the agent config.');

for (const theme of ['light', 'dark']) {
  await click(`.theme-switch button[data-theme=${theme}]`);
  await ev(`location.hash='#/overview'; 1`); await sleep(700); await shot(`${theme}-1-overview`);
  await ev(`location.hash='#/plugins'; 1`); await sleep(500); await shot(`${theme}-2-plugins`);
  if (await click('tr[data-plugin]')) { await sleep(500); await shot(`${theme}-3-plugin-drawer`); await click('#drawer [data-close]'); }
  await ev(`location.hash='#/jobs'; 1`); await sleep(500);
  if (await click('.jitem')) {
    await sleep(1000); await shot(`${theme}-4-job`);
    if (await click('#editJob')) { await sleep(800); await shot(`${theme}-5-editor`); await click('#modal [data-close]'); await sleep(200); }
    if (await click('#jobMenuBtn') && await click('#delJob')) { await sleep(400); await shot(`${theme}-6-confirm`); await click('#cfCancel'); await sleep(200); }
  } else { await shot(`${theme}-4-jobs-empty`); }
}
console.log('JS ERRORS:', errors.length ? JSON.stringify(errors, null, 2) : 'none');
ws.close();
// Wait for Chrome to exit before deleting its profile, or it races still-open files.
await new Promise(r => { chrome.once('exit', r); chrome.kill(); setTimeout(r, 3000); });
try { rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch {}
process.exit(errors.length ? 1 : 0);
