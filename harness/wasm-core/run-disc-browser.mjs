// Run the real-disc test in a real browser.
//
// Node cannot do this: emscripten_fetch is built on XMLHttpRequest, which Node
// does not have. That is not a limitation worth working around -- the browser
// IS the target, and a synchronous ranged fetch from a worker is exactly what
// the shipped app will do.
//
// Two servers are needed and they are not interchangeable:
//   :8100  the page and wasm, with COOP/COEP (pthreads need SharedArrayBuffer)
//   :8101  the disc, ranges only, with CORP so a cross-origin-isolated page
//          is allowed to load it
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const profile = mkdtempSync(join(tmpdir(), 'disc-'));
// Absolute script paths: the page server runs with cwd=/tmp/discweb so it
// serves the built module, and a relative script path would be resolved
// against that cwd rather than the repo.
const repo = process.cwd();
const page = spawn(process.execPath, [join(repo, 'spikes/threading/serve.mjs'), '8100'],
                   { cwd: '/tmp/discweb', stdio: 'ignore' });
const disc = spawn(process.execPath, [join(repo, 'harness/wasm-core/serve-disc.mjs'),
                                      '/tmp/ssa.iso', '8101'], { stdio: 'ignore' });
let chrome;

const cleanup = async () => {
  for (const p of [chrome, page, disc]) { try { p?.kill(); } catch {} }
  await sleep(300);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
};

try {
  await sleep(800);
  chrome = spawn(CHROME, ['--headless', '--no-sandbox', '--remote-debugging-port=9340',
                          `--user-data-dir=${profile}`, 'http://127.0.0.1:8100/index.html'],
                 { stdio: 'ignore' });
  let targets;
  for (let i = 0; i < 60; i++) {
    try { targets = await (await fetch('http://127.0.0.1:9340/json')).json(); break; }
    catch { await sleep(250); }
  }
  const target = targets?.find(t => t.type === 'page');
  if (!target) throw new Error('no page target');
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  const send = (id, method, params) => new Promise(res => {
    const h = ev => { const m = JSON.parse(ev.data);
      if (m.id === id) { ws.removeEventListener('message', h); res(m); } };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id, method, params }));
  });
  for (let i = 0; i < 240; i++) {
    const r = await send(i + 1, 'Runtime.evaluate',
      { expression: "document.getElementById('out').textContent", returnByValue: true });
    const t = r.result?.result?.value ?? '';
    if (i === 0) {
      // One-time diagnostic: a blank result usually means the page never
      // loaded, not that the test is slow.
      const u = await send(9000, 'Runtime.evaluate',
        { expression: 'location.href + " | " + document.title + " | coi=" + crossOriginIsolated',
          returnByValue: true });
      console.log('page: ' + (u.result?.result?.value ?? '(no page)'));
    }
    if (/PASSED|FAILED|EXIT|ERROR/.test(t)) { console.log(t); break; }
    await sleep(500);
    if (i === 239) console.log('TIMEOUT\n' + t);
  }
  ws.close();
} finally { await cleanup(); }
