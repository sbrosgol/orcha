// Plugin upload flow, end to end in the real UI: picker (file slots, live
// summary, focus, Tab trap), upload -> review -> install (upgrade), the
// provenance in the plugin drawer, a downgrade (needs its checkbox), the upload
// that crashed the server ("Install anyway", load failure kept in the dialog),
// keep for later / discard from the card, a wrong file type, Esc, dropping a
// file on the page, and the disabled state on a server with uploads off.
//
// usage: bash upload-env.sh start <work>     # prepares both servers + fixtures
//        node upload-flow.mjs <onUrl> <offUrl> <work>/fixtures <outDir> [width=1440] [height=900]
//        bash upload-env.sh stop <work>
// The flow changes server state: run upload-env.sh start again before re-running.
import { readFileSync } from 'node:fs';
import { openBrowser, sleep } from './cdp.mjs';

const [base, offBase, fx, out, W = '1440', H = '900'] = process.argv.slice(2);
if (!base || !offBase || !fx || !out) {
  console.error('usage: node upload-flow.mjs <onUrl> <offUrl> <fixturesDir> <outDir> [width] [height]'); process.exit(2);
}
const LIB = readFileSync(`${fx}/lib-name`, 'utf8').trim();          // libEchoCommand.<ext>
const EXT = LIB.slice(LIB.lastIndexOf('.'));
const KEY = readFileSync(`${fx}/key-id`, 'utf8').trim();
const files = (v, all = true) => all ? [`${fx}/${v}/${LIB}`, `${fx}/${v}/${LIB}.sig`, `${fx}/${v}/manifest.json`] : [`${fx}/${v}/${LIB}`];

const b = await openBrowser({ out, width: +W, height: +H });
const { ev, check, text } = b;
const shown = () => ev(`document.getElementById('upload').classList.contains('show')`);
const inDialog = () => ev(`document.getElementById('upload').contains(document.activeElement)`);
const active = () => ev(`document.activeElement.id||document.activeElement.tagName`);
const drop = (target, name) => ev(`(()=>{const dt=new DataTransfer(); dt.items.add(new File(['x'],${JSON.stringify(name)}));
  document.getElementById(${JSON.stringify(target)}).dispatchEvent(new DragEvent('drop',{dataTransfer:dt,bubbles:true,cancelable:true})); return 1;})()`);
const idle = () => b.until(`!document.querySelector('#upload .btn.loading')`);

// --- Uploads on ---
await b.login(base, '#/plugins');
await b.shot('1-plugins');
check('crashed upload flagged in the card', (await text('#stagedUploads')).includes('crashed the server'));
await b.mouse('#uploadPluginBtn'); await sleep(700);
check('dialog opens with picker', await shown() && (await text('#upTitle')) === 'Upload plugin' && await ev(`!!document.getElementById('upDrop')`));
check('focus moved into the dialog', await inDialog(), await active());
check('upload disabled until library + signature', await ev(`document.getElementById('upSend').disabled`));
check('platform and trusted key shown', (await text('#upBody')).includes(EXT) && (await text('#upBody')).includes(KEY));
await b.shot('2-picker');
await b.setFiles('#upFile', files('v13')); await sleep(400);
check('files sorted into slots', (await text('.upfiles')).includes(`${LIB}.sig`) && (await text('.upfiles')).includes('manifest.json') && !(await ev(`document.getElementById('upSend').disabled`)));
check('live summary', (await text('#upLive')).includes('Ready to upload'));
await ev(`document.querySelector('[data-upx="manifest"]').focus(); document.querySelector('[data-upx="manifest"]').click(); 1`); await sleep(200);
check('focus stays in dialog after removing a file', await inDialog(), await active());
await b.setFiles('#upFile', [`${fx}/v13/manifest.json`]); await sleep(300);
await ev(`document.getElementById('upSend').focus(); 1`);
await b.key('Tab', 'Tab', 9); await sleep(100);
check('Tab stays in dialog', await inDialog());
await b.shot('3-picker-filled');

await ev(`document.getElementById('upSend').click(); 1`);
await b.until(`document.getElementById('upTitle').textContent==='Review plugin'`);
const review = await text('#upBody');
check('review after upload', review.includes('upgrade from 1.2.0') && review.includes('Verified') && review.includes('loads on this server'), review.slice(0, 200));
check('install button says upgrade', (await text('#upInstall')).includes('Upgrade to 1.3.0'));
check('focus on install', await active() === 'upInstall');
await b.shot('4-review');
await ev(`document.getElementById('upInstall').click(); 1`);
await b.until(`!document.getElementById('upload').classList.contains('show')`); await sleep(400);
check('installed: dialog closed, toast', !(await shown()) && (await text('#toasts')).includes('echo: 1.2.0 → 1.3.0'), await text('#toasts'));
check('focus back on opener', await active() === 'uploadPluginBtn', await active());
check('table shows 1.3.0 as uploaded', await ev(`[...document.querySelectorAll('tr[data-plugin="echo"] td')].some(td=>td.textContent.trim()==='1.3.0')`) &&
  (await text('tr[data-plugin="echo"]')).includes('uploaded'));
await ev(`document.querySelector('tr[data-plugin="echo"]').click(); 1`); await sleep(500);
const drawer = await text('#drBody');
check('drawer shows source and provenance', drawer.includes('overrides the bundled') && drawer.includes('Installed from') && drawer.includes(KEY), drawer.slice(0, 300));
await b.shot('5-drawer-provenance');
await b.key('Escape', 'Escape', 27); await sleep(300);

// A downgrade needs its checkbox.
await ev(`document.getElementById('uploadPluginBtn').click(); 1`); await sleep(600);
await b.setFiles('#upFile', files('v12')); await sleep(300);
await ev(`document.getElementById('upSend').click(); 1`);
await b.until(`!!document.getElementById('upInstall')`);
check('downgrade labelled', (await text('#upBody')).includes('downgrade from 1.3.0') && (await text('#upBody')).includes('older than the installed 1.3.0') && (await text('#upInstall')).includes('Downgrade to 1.2.0'));
check('downgrade needs the box ticked', await ev(`document.getElementById('upInstall').disabled`) && await ev(`!!document.getElementById('upConfirm')`));
await ev(`document.getElementById('upConfirm').click(); 1`); await sleep(100);
check('ticking enables it', !(await ev(`document.getElementById('upInstall').disabled`)));
await b.shot('6-downgrade');
await ev(`document.getElementById('upConfirm').click(); 1`); await sleep(100);
check('unticking disables it again', await ev(`document.getElementById('upInstall').disabled`));
await ev(`document.getElementById('upDiscard').click(); 1`); await sleep(400);
await ev(`document.getElementById('cfOk').click(); 1`);
await b.until(`!document.getElementById('upload').classList.contains('show')`); await sleep(300);
check('discard from review closes dialog, focus restored', !(await shown()) && await active() !== 'BODY', await active());

// The upload that crashed the server: warned, "Install anyway"; it also
// clashes ("echo" is taken), so the load failure stays in the dialog.
await ev(`[...document.querySelectorAll('[data-up="review"]')][0].click(); 1`); await sleep(500);
check('crash warning in review', (await text('#upBody .alert.err')).includes('The server stopped while installing') && (await text('#upInstall')).includes('anyway'));
await b.shot('7-crashed-review');
await ev(`document.getElementById('upInstall').click(); 1`); await sleep(200); await idle();
check('load failure shown in dialog', await shown() && (await text('#upBody')).includes("Command 'echo' already registered"), await text('#upBody'));
await ev(`[...document.querySelectorAll('#upFt [data-close]')][0].click(); 1`); await sleep(400);
check('kept for later: staged card', (await text('#stagedUploads')).includes('echo-dup'));
await ev(`document.querySelector('[data-up="discard"]').click(); 1`); await sleep(400);
check('discard asks first', (await text('#cfTitle')) === 'Discard this upload?');
await ev(`document.getElementById('cfOk').click(); 1`);
await b.until(`document.getElementById('stagedUploads').textContent===''`); await sleep(200);
check('discarded; focus not lost', (await text('#stagedUploads')) === '' && await active() === 'uploadPluginBtn', await active());

// Wrong file type, then a valid one: the old error goes away. Esc closes.
await ev(`document.getElementById('uploadPluginBtn').click(); 1`); await sleep(600);
await drop('upload', 'notes.txt'); await sleep(300);
check('wrong type explained', (await text('#upBody .alert.err')).includes(`notes.txt is not a ${EXT} library`));
await ev(`document.querySelector('.theme-switch button[data-theme=dark]').click(); 1`); await sleep(300);
await b.shot('8-dark-picker-error');
await b.setFiles('#upFile', files('v13', false)); await sleep(300);
check('stale error cleared', (await text('#upBody .alert.err')) === '');
await b.key('Escape', 'Escape', 27); await sleep(300);
check('Esc closes', !(await shown()));
await drop('view-plugins', `${LIB}.sig`); await sleep(900);
check('page drop opens dialog with the file', await shown() && (await text('.upfiles')).includes(`${LIB}.sig`));
await b.key('Escape', 'Escape', 27); await sleep(300);

// --- Uploads off ---
await b.login(offBase, '#/plugins');
await ev(`document.getElementById('uploadPluginBtn').click(); 1`);
await b.until(`document.getElementById('upBody').textContent.includes('plugins.upload')`);
check('disabled explained', (await text('#upBody')).includes('plugins.upload.enabled') && !(await ev(`!!document.getElementById('upDrop')`)), await text('#upBody'));
await b.shot('9-dark-disabled');
await b.finish();
