// Job editor flow: Visual / JSON / YAML tabs, live YAML validation, settings
// controls following the YAML, file import (whole job and bare definition,
// picker and drag-and-drop), tab keyboard support, and the remembered tab.
// Drives the real UI and reads results back through the API.
//
// usage: node editor-yaml.mjs <baseUrl> <outDir> [width=1440] [height=900]
// needs: a running agent with the echo plugin and admin auth (see SKILL.md "Run it").
// note:  saves a job named "imported-yaml-job" (deleted first if it exists).
import { writeFileSync } from 'node:fs';
import { openBrowser, sleep } from './cdp.mjs';

const [base, out, W = '1440', H = '900'] = process.argv.slice(2);
if (!base || !out) { console.error('usage: node editor-yaml.mjs <baseUrl> <outDir> [width] [height]'); process.exit(2); }
const b = await openBrowser({ out, width: +W, height: +H });
const { ev, check, val, text } = b;

// Fixtures: a whole job as YAML, and a job that leaves most fields out.
const jobFile = `${out}/job-import.yaml`, partialFile = `${out}/partial-job.json`;
writeFileSync(jobFile, `# A whole job: name, schedule and definition.
name: imported-yaml-job
description: Imported from a YAML file
schedule_cron: "30 6 * * *"
enabled: true
definition:
  rollback: best_effort
  steps:
    - name: hello
      command: echo
      params:
        message: |-
          multi
          line
`);
writeFileSync(partialFile, JSON.stringify({ name: 'partial-job', definition: { steps: [{ command: 'echo', params: { message: 'x' } }] } }));

const tab = () => ev(`document.querySelector('.tab.active').dataset.deftab`);
const setYaml = (t) => ev(`(()=>{const ta=document.getElementById('jYaml'); ta.value=${JSON.stringify(t)}; ta.dispatchEvent(new Event('input')); return 1;})()`);
const confirmShown = () => ev(`getComputedStyle(document.getElementById('confirm')).display!=='none'`);
const api = (method, path) => ev(`fetch(${JSON.stringify(path)},{method:${JSON.stringify(method)},headers:{Authorization:sessionStorage.getItem('orcha_auth')}}).then(r=>r.json())`);

await b.send('Page.navigate', { url: base + '/admin' }); await sleep(600);
await ev(`localStorage.removeItem('orcha_deftab'); 1`);
await b.login(base, '#/jobs');
for (const j of ((await api('GET', '/api/jobs')).jobs || []).filter(x => x.name === 'imported-yaml-job')) await api('DELETE', '/api/jobs/' + j.id);
await ev(`document.getElementById('newJob').click(); 1`); await sleep(1000);
check('opens in visual', await tab() === 'visual');

await ev(`document.querySelector('.tab[data-deftab=yaml]').click(); 1`); await sleep(600);
const y0 = await val('jYaml');
check('visual -> YAML', await tab() === 'yaml' && y0.includes('  - name: greet\n    command: echo') && y0.includes('{{steps.greet.output.echoed}}'), y0);
await b.shot('1-yaml-tab');

await setYaml('steps:\n  - name: a\n  - [x\n');
const st = await b.until(`(t=>/^Line /.test(t)&&t)(document.getElementById('yamlStatus').textContent)`);
check('live error with position', /^Line 4, column 1: /.test(st || ''), st);
await ev(`document.querySelector('.tab[data-deftab=visual]').click(); 1`); await sleep(600);
check('cannot leave invalid YAML', await tab() === 'yaml' && (await text('#defWarn span')) === 'Fix the YAML to switch tabs.', await text('#defWarn span'));
check('caret moved to error line', await ev(`(()=>{const t=document.getElementById('jYaml'); return t.value.slice(0,t.selectionStart).split('\\n').length;})()`) === 4);
await b.shot('2-yaml-error');

await setYaml('# my comment\nrollback: reverse\nsteps:\n  - name: one\n    command: echo\n    params:\n      message: "007"\n');
await b.until(`document.getElementById('yamlStatus').textContent.startsWith('Valid YAML')`);
check('valid status', (await text('#yamlStatus')).startsWith('Valid YAML'), await text('#yamlStatus'));
check('controls follow YAML', await val('jRollback') === 'reverse');
await ev(`document.getElementById('jBreaker').click(); 1`);
await b.until(`document.getElementById('jYaml').value.includes('circuit_breaker')`);
check('setting rewrites YAML', await val('jYaml') === 'rollback: reverse\ncircuit_breaker: true\nsteps:\n  - name: one\n    command: echo\n    params:\n      message: "007"\n', await val('jYaml'));

await ev(`document.querySelector('.tab[data-deftab=json]').click(); 1`); await sleep(600);
const j = JSON.parse(await val('jDef'));
check('YAML -> JSON keeps types and order', await tab() === 'json' && j.steps[0].params.message === '007' && Object.keys(j).join() === 'rollback,circuit_breaker,steps', j);
await ev(`document.querySelector('.tab[data-deftab=visual]').click(); 1`); await sleep(600);
check('JSON -> visual', await tab() === 'visual' && await ev(`document.querySelectorAll('.step-card').length`) === 1);

// Import a whole job (file picker); the edited definition is replaced only after a confirm.
await b.setFiles('#defFile', [jobFile]); await sleep(800);
check('confirm before replacing edits', await confirmShown());
await b.shot('3-import-confirm');
await ev(`document.getElementById('cfOk').click(); 1`); await sleep(900);
const y2 = await val('jYaml');
check('job fields imported', await val('jName') === 'imported-yaml-job' && await val('jSchedule') === '30 6 * * *' && await val('jDesc') === 'Imported from a YAML file');
check('definition imported to YAML tab', await tab() === 'yaml' && y2.startsWith('rollback: best_effort\nsteps:') && y2.includes('      message: |-\n        multi\n        line\n'), y2);
check('rollback control from file', await val('jRollback') === 'best_effort');
await ev(`document.getElementById('modalSave').click(); 1`);
await b.until(`getComputedStyle(document.getElementById('modal')).display==='none'`);
const saved = ((await api('GET', '/api/jobs')).jobs || []).find(x => x.name === 'imported-yaml-job');
check('saved from YAML tab', saved && saved.schedule_cron === '30 6 * * *' && saved.definition.steps[0].params.message === 'multi\nline' && saved.definition.rollback === 'best_effort', saved);

// A partial job file after a full one: missing fields reset (no stale schedule).
await ev(`document.getElementById('newJob').click(); 1`); await sleep(1200);
await b.setFiles('#defFile', [jobFile]); await sleep(900);
await b.setFiles('#defFile', [partialFile]); await sleep(700);
check('job import over typed fields asks first', await confirmShown());
await ev(`document.getElementById('cfOk').click(); 1`); await sleep(700);
check('partial job resets missing fields', await val('jName') === 'partial-job' && await val('jSchedule') === '' && await val('jDesc') === '' && await ev(`document.getElementById('jEnabled').checked`) === true);

// Tabs: arrow keys and roving tabindex.
await ev(`document.querySelector('.tab.active').focus(); 1`);
await b.key('ArrowRight', 'ArrowRight', 39); await sleep(700);
check('ArrowRight moves to next tab', await tab() === 'yaml' && await ev(`document.activeElement.dataset.deftab`) === 'yaml', await tab());
check('roving tabindex', await ev(`[...document.querySelectorAll('.tab[data-deftab]')].map(t=>t.tabIndex).join()`) === '-1,-1,0');
// The text is read-only while a server round-trip is in flight.
await ev(`(()=>{ window.__ro=[]; const ta=document.getElementById('jYaml'); new MutationObserver(()=>window.__ro.push(ta.readOnly)).observe(ta,{attributes:true}); document.getElementById('jBreaker').click(); return 1; })()`); await sleep(800);
check('YAML locked during round-trip', await ev(`window.__ro.includes(true) && document.getElementById('jYaml').readOnly===false`), await ev(`window.__ro`));
await ev(`document.querySelector('#modal [data-close]').click(); 1`); await sleep(300);

// Reopen: the tab chosen (by arrow key) is remembered; then drop a JSON file.
await ev(`document.getElementById('newJob').click(); 1`); await sleep(1200);
check('reopens in last chosen tab', await tab() === 'yaml', await tab());
const dropJs = (type) => `(()=>{const dt=new DataTransfer(); dt.items.add(new File([JSON.stringify({steps:[{name:'dropped',command:'echo',params:{message:'hi'}}]})], 'flow.json', {type:'application/json'}));
  document.getElementById('defField').dispatchEvent(new DragEvent('${type}',{dataTransfer:dt,bubbles:true,cancelable:true})); return 1;})()`;
await ev(dropJs('dragenter')); await sleep(200);
check('drop hint shown', await ev(`document.getElementById('defField').classList.contains('dropping')`));
await b.shot('4-drop-hint');
await ev(dropJs('drop')); await sleep(900);
check('pristine sample replaced without confirm', !(await confirmShown()));
check('dropped JSON opens JSON tab', await tab() === 'json' && JSON.parse(await val('jDef')).steps[0].name === 'dropped', await val('jDef'));
check('drop hint cleared', !(await ev(`document.getElementById('defField').classList.contains('dropping')`)));

// Dark theme shots of the YAML tab.
await ev(`document.querySelector('.tab[data-deftab=yaml]').click(); 1`); await sleep(700);
await ev(`(document.querySelector('.theme-switch button[data-theme=dark]')||{click(){}}).click(); 1`); await sleep(300);
await b.shot('5-dark-yaml-tab');
await setYaml('steps:\n  - [x\n'); await sleep(900);
await b.shot('6-dark-yaml-error');
await b.finish();
