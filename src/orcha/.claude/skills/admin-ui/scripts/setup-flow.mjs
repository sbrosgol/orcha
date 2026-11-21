// First-run setup flow: a server with no admin password shows "Create the
// admin account" instead of sign-in; validation; creating the account signs
// you in; sign-out shows sign-in (not setup) and the new login works; and when
// someone else claims the account first, the form falls back to sign-in.
//
// usage: node setup-flow.mjs <freshUrl> <raceUrl> <outDir> [width=1440] [height=900]
// needs: two agents with NO admin.password and a fresh admin.credentials_file
//        each (see SKILL.md "First-run setup"). Both are claimed by the run:
//        restart them with a new credentials file to run it again.
import { openBrowser, sleep } from './cdp.mjs';

const [fresh, race, out, W = '1440', H = '900'] = process.argv.slice(2);
if (!fresh || !race || !out) { console.error('usage: node setup-flow.mjs <freshUrl> <raceUrl> <outDir> [width] [height]'); process.exit(2); }
const b = await openBrowser({ out, width: +W, height: +H });
const { ev, check, text } = b;
const USER = 'ops.admin', PASS = 'correct horse battery';

const mode = () => ev(`document.getElementById('login').dataset.mode`);
const shown = (id) => ev(`(()=>{const e=document.getElementById(${JSON.stringify(id)}); return !!e && e.offsetParent!==null;})()`);
const fill = (fields) => ev(`(()=>{ for (const [id,v] of Object.entries(${JSON.stringify(fields)})) document.getElementById(id).value=v; return 1; })()`);
const submitSetup = async () => { await b.mouse('#setupBtn'); await b.until(`!document.getElementById('setupBtn').classList.contains('loading')`); };
const theme = (t) => ev(`document.documentElement.dataset.theme=${JSON.stringify(t)}; 1`);
const setupState = async (base) => (await (await fetch(base + '/api/setup')).json()).setup_required;

// ---- Fresh server: setup view, validation, create, sign out, sign in ----
check('server starts in setup', await setupState(fresh) === true);
await b.send('Page.navigate', { url: fresh + '/admin' });
check('setup view shown', (await b.until(`document.getElementById('login').dataset.mode==='setup'`)) === true, await mode());
check('setup form visible, sign-in hidden', (await shown('setupForm')) && !(await shown('loginForm')));
check('warning about the open claim', (await text('#setupNote')).includes('anyone who can open this page'));
check('focus on the password (username prefilled)', await b.until(`document.activeElement && document.activeElement.id==='suPass'`) === true,
      await ev(`document.activeElement && document.activeElement.id`));
await theme('light'); await sleep(150); await b.shot('setup-light'); await theme('dark'); await sleep(150); await b.shot('setup-dark'); await theme('light');

await fill({ suUser: USER, suPass: 'short', suPass2: 'short' }); await submitSetup();
check('short password refused', (await text('#setupErr')).includes('12 characters'));
await fill({ suPass: PASS, suPass2: PASS + 'x' }); await submitSetup();
check('mismatch refused', (await text('#setupErr')).includes('match'));
await fill({ suUser: 'bad user', suPass2: PASS }); await submitSetup();
check('bad username refused', (await text('#setupErr')).includes('letters, digits'));
check('still in setup after client-side errors', await setupState(fresh) === true);
await b.shot('setup-error');

await fill({ suUser: USER, suPass: PASS, suPass2: PASS }); await submitSetup();
check('signed in after creating', (await b.until(`document.body.classList.contains('authed')`)) === true);
check('header shows the new user', (await text('#who')) === USER);
check('server left setup', await setupState(fresh) === false);
const jobs = await fetch(fresh + '/api/jobs', { headers: { Authorization: 'Basic ' + Buffer.from(`${USER}:${PASS}`).toString('base64') } });
check('new login works on the API', jobs.status === 200, jobs.status);
check('setup password not kept in the form', (await ev(`document.getElementById('suPass').value`)) === '');
await sleep(300); await b.shot('after-setup');

// Not a mouse click: the success toast covers it on desktop, and on a phone it
// sits in the closed nav drawer.
await ev(`document.getElementById('logout').click(); 1`);
check('sign-out shows sign-in, not setup', (await b.until(`document.getElementById('login').dataset.mode==='login'`)) === true, await mode());
await fill({ user: USER, pass: PASS }); await b.mouse('#signin');
check('signs in again with the created account', (await b.until(`document.body.classList.contains('authed')`)) === true);

// ---- Race: someone claims the account while the form is open ----
await ev(`sessionStorage.clear(); 1`);
await b.send('Page.navigate', { url: race + '/admin' });
check('race server in setup', (await b.until(`document.getElementById('login').dataset.mode==='setup'`)) === true);
const claim = await fetch(race + '/api/setup', { method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ username: 'first', password: 'someone else was faster' }) });
check('other client claims first (201)', claim.status === 201, claim.status);
await fill({ suUser: USER, suPass: PASS, suPass2: PASS }); await submitSetup();
check('late claim falls back to sign-in', (await b.until(`document.getElementById('login').dataset.mode==='login'`)) === true, await mode());
check('explains why', (await text('#loginErr')).includes('already exists'));
check('not signed in as the late claimant', !(await ev(`document.body.classList.contains('authed')`)));
await b.shot('race-lost');

await b.finish();
