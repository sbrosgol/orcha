// Shared headless-Chrome plumbing for the admin UI flow harnesses
// (editor-yaml.mjs, upload-flow.mjs). Launches Chrome with a throwaway
// profile, speaks CDP over its WebSocket, collects uncaught JS errors, and
// gives checks/screenshots/login helpers.
//
// Gotchas these harnesses learned the hard way:
//  - el.click() from Runtime.evaluate does NOT focus the element (real clicks
//    do in Chrome, not in Safari): app code must not rely on the clicked
//    button being document.activeElement; use mouse() for a real click.
//  - The page's `state`/`up` objects live in the app script's scope and are
//    not reachable from evaluate(); assert on the DOM, location.hash and API.
//  - Never sleep a fixed time for a request: wait for the UI to settle with
//    until() (e.g. the button leaves its .loading state).
//
// env: CHROME (default macOS Google Chrome), CDP_PORT (default 9340),
//      ADMIN_USER / ADMIN_PASS (default admin / change-me)
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync, rmSync } from 'node:fs';

export const sleep = (ms) => new Promise(r => setTimeout(r, ms));

export async function openBrowser({ out, width = 1440, height = 900, port = +(process.env.CDP_PORT || 9340) }) {
  const CHROME = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  mkdirSync(out, { recursive: true });
  const profile = `${out}/.profile-${port}`;
  rmSync(profile, { recursive: true, force: true });
  const chrome = spawn(CHROME, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    '--no-first-run', `--window-size=${width},${height}`, 'about:blank'], { stdio: 'ignore' });
  let ws;
  for (let i = 0; i < 50 && !ws; i++) {
    try {
      const t = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const pg = t.find(x => x.type === 'page');
      if (pg) ws = new WebSocket(pg.webSocketDebuggerUrl);
    } catch {}
    if (!ws) await sleep(200);
  }
  if (!ws) { chrome.kill(); throw new Error('Could not connect to headless Chrome (is CHROME correct?)'); }
  await new Promise(r => ws.onopen = r);

  let id = 0;
  const pending = new Map(), errors = [], fails = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
    if (d.method === 'Runtime.exceptionThrown') errors.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
    if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') errors.push(d.params.args.map(a => a.value || a.description).join(' '));
  };
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await send('Runtime.enable'); await send('Page.enable'); await send('DOM.enable');
  await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: width < 600 });

  const b = {
    send, ev, errors, fails, width, phone: width < 600,
    tag: width < 600 ? 'phone' : 'desk',
    async shot(name) {
      const r = await send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(`${out}/${b.tag}-${name}.png`, Buffer.from(r.result.data, 'base64'));
    },
    check(name, cond, info) {
      console.log((cond ? 'ok   ' : 'FAIL ') + name + (cond ? '' : '  -> ' + JSON.stringify(info)));
      if (!cond) fails.push(name);
    },
    text: (sel) => ev(`(document.querySelector(${JSON.stringify(sel)})||{}).textContent||''`),
    val: (id) => ev(`document.getElementById(${JSON.stringify(id)}).value`),
    /** Poll @p expr until truthy (or @p ms passes); returns the last value. */
    async until(expr, ms = 5000) {
      let v;
      for (const t0 = Date.now(); Date.now() - t0 < ms; await sleep(100)) { if ((v = await ev(expr))) return v; }
      return v;
    },
    /** A real mouse click at the centre of @p sel (focuses buttons like a user's click). */
    async mouse(sel) {
      const p = await ev(`(()=>{ const r=document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return {x:r.left+r.width/2, y:r.top+r.height/2}; })()`);
      for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent', { type, x: p.x, y: p.y, button: 'left', clickCount: 1 });
    },
    async key(key, code = key, keyCode = 0) {
      await send('Input.dispatchKeyEvent', { type: 'keyDown', key, code, windowsVirtualKeyCode: keyCode });
      await send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: keyCode });
    },
    /** Give a file input (by CSS selector) these files, as the file picker would. */
    async setFiles(sel, files) {
      const doc = await send('DOM.getDocument', {});
      const n = await send('DOM.querySelector', { nodeId: doc.result.root.nodeId, selector: sel });
      await send('DOM.setFileInputFiles', { nodeId: n.result.nodeId, files });
    },
    /** Sign in through the real form and go to @p hash. */
    async login(base, hash = '#/overview') {
      await send('Page.navigate', { url: base + '/admin' }); await sleep(900);
      const user = process.env.ADMIN_USER || 'admin', pass = process.env.ADMIN_PASS || 'change-me';
      await ev(`document.getElementById('user').value=${JSON.stringify(user)}; document.getElementById('pass').value=${JSON.stringify(pass)}; document.getElementById('signin').click(); 1`);
      if (!(await b.until(`document.body.classList.contains('authed')`))) throw new Error('Login failed: check ADMIN_USER / ADMIN_PASS');
      await ev(`location.hash=${JSON.stringify(hash)}; 1`); await sleep(600);
    },
    /** Print the summary, close Chrome and exit (1 on failed checks or JS errors). */
    async finish() {
      console.log('JS ERRORS:', errors.length ? JSON.stringify(errors, null, 2) : 'none');
      console.log(fails.length ? 'FAILED: ' + fails.join(', ') : 'ALL CHECKS PASSED');
      ws.close();
      await new Promise(r => { chrome.once('exit', r); chrome.kill(); setTimeout(r, 3000); });
      try { rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch {}
      process.exit(errors.length || fails.length ? 1 : 0);
    },
  };
  return b;
}
