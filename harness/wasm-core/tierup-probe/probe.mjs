// How Chrome tiers a wasm function up, measured in the real browser.
//
//   JS_FLAGS=--allow-natives-syntax node harness/wasm-core/tierup-probe/probe.mjs \
//       'index.html?pad=20&chunk=1000'
//
// Serves this directory on :8112 (spikes/threading/serve.mjs), opens the page
// in headless Chrome (CHROME=<binary> for another one) with the V8 flags in
// JS_FLAGS, polls window.__lines / window.__done and prints the lines. A
// small copy of run-boot-browser.mjs; index.html says what each query does.
// The natives syntax is needed for %IsTurboFanFunction, which is how the
// page sees the tier -- a flag for the measurement, never for a player.
//
// What it established for Chrome 156 (V8 15.6), docs/PHASE3-WEBGPU.md: the
// per-function budget is 13,000,000; a return or tail call charges the
// machine-code offset of that instruction + 60; a function that skips its
// body and leaves through its end charges its whole size per call;
// compilation hints (metadata.code.compilation_priority) are ignored with
// or without --experimental-wasm-compilation-hints; a batch of 27 warmed
// blocks is optimised within 6 ms.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const CHROME = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const PORT = 8112, DBG = 9351;
const path = process.argv[2] ?? 'index.html';
const profile = mkdtempSync(join(tmpdir(), 'probe-'));
const page = spawn(process.execPath, [join(repo, 'spikes/threading/serve.mjs'), String(PORT)],
                   { cwd: here, stdio: 'ignore' });
let chrome;
const cleanup = async () => {
  for (const p of [chrome, page]) { try { p?.kill(); } catch {} }
  await sleep(200);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
};
try {
  await sleep(500);
  chrome = spawn(CHROME, ['--headless', '--no-sandbox', `--remote-debugging-port=${DBG}`,
                          '--enable-logging=stderr', '--v=0',
                          ...(process.env.JS_FLAGS ? [`--js-flags=${process.env.JS_FLAGS}`] : []),
                          `--user-data-dir=${profile}`, `http://127.0.0.1:${PORT}/${path}`],
                 { stdio: ['ignore', 'ignore', 'pipe'] });
  chrome.stderr.setEncoding('utf8');
  chrome.stderr.on('data', chunk => {
    for (const line of chunk.split('\n')) {
      const m = line.match(/:CONSOLE\(\d+\)\]\s+"(.*)", source:/);
      if (m && m[1] && !/^(W\||probe:)/.test(m[1])) console.log('C| ' + m[1]);
    }
  });
  let target;
  for (let i = 0; i < 60 && !target; i++) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${DBG}/json`)).json();
      target = targets.find(t => t.type === 'page' && t.url.includes(`:${PORT}/`));
    } catch {}
    if (!target) await sleep(250);
  }
  if (!target) throw new Error('no page target');
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let id = 0;
  const evaluate = expr => new Promise(res => {
    const my = ++id;
    const h = ev => { const m = JSON.parse(ev.data); if (m.id === my) { ws.removeEventListener('message', h); res(m); } };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id: my, method: 'Runtime.evaluate', params: { expression: expr, returnByValue: true } }));
  });
  let from = 0;
  const deadline = Date.now() + (Number(process.env.PROBE_TIMEOUT_S ?? 600)) * 1000;
  while (Date.now() < deadline) {
    const r = await evaluate(`JSON.stringify({n: (window.__lines||[]).length, done: !!window.__done, chunk: (window.__lines||[]).slice(${from})})`);
    const v = r?.result?.result?.value;
    if (typeof v === 'string') {
      const { done, chunk } = JSON.parse(v);
      for (const l of chunk) console.log(l);
      from += chunk.length;
      if (done) break;
    }
    await sleep(200);
  }
  ws.close();
} finally { await cleanup(); }
