// Run the real-disc test in a real browser.
//
// Node cannot do this: emscripten_fetch is built on XMLHttpRequest, which Node
// does not have. That is not a limitation worth working around -- the browser
// IS the target, and a synchronous ranged fetch from a worker is exactly what
// the shipped app will do.
//
// Two servers are needed and they are not interchangeable:
//   :8102  the page and wasm, with COOP/COEP (pthreads need SharedArrayBuffer)
//   :8101  the disc, ranges only, with CORP so a cross-origin-isolated page
//          is allowed to load it
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const profile = mkdtempSync(join(tmpdir(), 'disc-'));
// Absolute script paths: the page server runs with cwd=/tmp/bootweb so it
// serves the built module, and a relative script path would be resolved
// against that cwd rather than the repo.
const repo = process.cwd();
const page = spawn(process.execPath, [join(repo, 'spikes/threading/serve.mjs'), '8102'],
                   { cwd: '/tmp/bootweb', stdio: 'ignore' });
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
  // --enable-logging=stderr forwards console output from EVERY context,
  // workers included. That matters because PROXY_TO_PTHREAD runs main() on a
  // worker: its console.log lands in a context that page-scoped CDP
  // (Runtime.consoleAPICalled on the page target) never sees, and that the
  // page's DOM never receives. Markers written with emscripten_console_log
  // therefore arrive here and nowhere else.
  chrome = spawn(CHROME, ['--headless', '--no-sandbox', '--remote-debugging-port=9341',
                          '--enable-logging=stderr', '--v=0',
                          `--user-data-dir=${profile}`, 'http://127.0.0.1:8102/index.html'],
                 { stdio: ['ignore', 'ignore', 'pipe'] });
  chrome.stderr.setEncoding('utf8');
  chrome.stderr.on('data', chunk => {
    for (const line of chunk.split('\n')) {
      // Chrome wraps console output as [...:INFO:CONSOLE(n)] "text", source: ...
      const m = line.match(/:CONSOLE\(\d+\)\]\s+"(.*)", source:/);
      if (m && m[1]) console.log('W| ' + m[1]);
    }
  });
  let targets;
  for (let i = 0; i < 60; i++) {
    try { targets = await (await fetch('http://127.0.0.1:9341/json')).json(); break; }
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
  // Drain the page's line buffer incrementally.
  //
  // `from` is the high-water mark: each poll asks only for lines produced
  // since the last one. Re-reading the whole buffer every time is what killed
  // the page on a 20k-step run -- the trace was correct right up to the point
  // the transport collapsed, which reads as an emulation divergence and is
  // not one.
  let from = 0;
  const out = [];
  let stalled = 0;
  let badPolls = 0;
  for (let i = 0; i < 2400; i++) {
    const r = await send(i + 1, 'Runtime.evaluate', { returnByValue: true, expression: `
      (function () {
        var L = window.__lines || [];
        var stage = '?';
        try {
          if (typeof Module !== 'undefined' && Module._harness_stage && Module.HEAPU8) {
            var p = Module._harness_stage(), h = Module.HEAPU8, e = p;
            while (h[e]) e++;
            stage = String.fromCharCode.apply(null, h.subarray(p, e));
          }
        } catch (err) { stage = 'read failed: ' + err.message; }
        // Cap the chunk so one poll can never ship an unbounded payload.
        var chunk = L.slice(${from}, ${from} + 4000);
        return JSON.stringify({ n: L.length, done: !!window.__done, stage: stage, chunk: chunk });
      })()` });
    const v = r.result?.result?.value;
    if (typeof v !== 'string') {
      // One bad poll is not a dead page -- at startup the document may not
      // have run its inline script yet. Only a sustained run of failures
      // means the page is actually gone (crash, OOM, navigation), and that
      // has to be reported rather than quietly yielding a partial trace.
      const why = r.result?.exceptionDetails?.exception?.description
               ?? r.result?.exceptionDetails?.text ?? JSON.stringify(r.result ?? null);
      if (++badPolls === 1) console.log(`poll failed (${i}): ${String(why).slice(0, 200)}`);
      if (badPolls > 20) {
        console.log(`PAGE LOST after ${out.length} lines (poll ${i})`);
        break;
      }
      await sleep(500);
      continue;
    }
    badPolls = 0;
    const { n, done, stage, chunk } = JSON.parse(v);
    if (i === 0) {
      const u = await send(9000, 'Runtime.evaluate',
        { expression: 'location.href + " | " + document.title + " | coi=" + crossOriginIsolated',
          returnByValue: true });
      console.log('page: ' + (u.result?.result?.value ?? '(no page)'));
    }
    out.push(...chunk);
    from += chunk.length;
    stalled = chunk.length ? 0 : stalled + 1;
    if (done && from >= n) break;
    if (i > 0 && i % 60 === 0)
      console.log(`--- t=${i / 2}s, ${from} lines, stage=[${stage}]`);
    // Only idle when there was nothing to collect; a producing run should be
    // drained as fast as the round trip allows.
    if (!chunk.length) await sleep(500);
    if (i === 2399) console.log(`TIMEOUT (stage=[${stage}], ${from} lines)`);
  }
  console.log(out.join('\n'));
  ws.close();
} finally { await cleanup(); }
