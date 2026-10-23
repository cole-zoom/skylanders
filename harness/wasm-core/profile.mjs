// CPU-profile a bootgame run in headless Chrome.
//
// Attaches V8's sampling profiler to EVERY worker -- under PROXY_TO_PTHREAD
// main() runs on one, and under --dual-core the GPU loop runs on another --
// starts it when the stage marker reaches "stepping" (plus PROF_DELAY_MS, so
// a late phase can be profiled on its own), stops when the run prints
// PASSED or FAILED, and writes to the directory it is given:
//
//   run.txt              everything the run printed
//   profile-N.cpuprofile one per worker, loadable in Chrome's performance panel
//   summary.txt          self time by function over every worker
//   threads.txt          each busy thread by category (profile-threads.mjs)
//
// run-jit.sh runs this when PROFILE=<dir> is set, and links with the
// function names kept so the frames read as C++ rather than f4117. Pass
// --jit-names to the harness so compiled blocks appear as b_<pc>.
//
// The sampling interval is 200 us: across fifteen workers the profiles are
// a few megabytes each for a ten-minute run.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { report } from './profile-threads.mjs';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const OUTDIR = process.argv[2] ?? '.';
mkdirSync(OUTDIR, { recursive: true });
const DELAY_MS = Number(process.env.PROF_DELAY_MS || 0);
const profile = mkdtempSync(join(tmpdir(), 'prof-'));
// Same two servers and ports as run-boot-browser.mjs: the page from
// /tmp/bootweb with COOP/COEP, the disc with ranges and CORP.
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
  chrome = spawn(CHROME, ['--headless', '--no-sandbox', '--remote-debugging-port=9341', '--v=0',
                          `--user-data-dir=${profile}`, 'http://127.0.0.1:8102/index.html'],
                 { stdio: 'ignore' });
  let ver;
  for (let i = 0; i < 60; i++) {
    try { ver = await (await fetch('http://127.0.0.1:9341/json/version')).json(); break; }
    catch { await sleep(250); }
  }
  if (!ver) throw new Error('Chrome did not come up on :9341');
  // One browser-level socket, flattened sessions: the page and every worker
  // are addressed by sessionId over the same connection.
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let nextId = 1;
  const pending = new Map();
  // A closed socket must be fatal, not silent. send() on a closed WebSocket
  // drops the message without throwing, so without this every call simply
  // timed out and the poll loop ran on for an hour after the run had
  // printed PASSED. Print the close code and reason: they are the only
  // evidence of why Chrome hung up.
  let closed = null;
  ws.onclose = ev => { closed = `code ${ev.code} reason "${ev.reason}"`; console.log(`debugger socket closed: ${closed}`); };
  ws.onerror = ev => { console.log(`debugger socket error: ${ev.message ?? ev.type}`); };
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  };
  // Every call races a timeout, for the reason run-boot-browser.mjs gives: a
  // dropped reply must not wedge the poll loop forever. Worker sessions are
  // the slow ones: a pool worker blocked in Atomics.wait answers nothing
  // until its wait returns, which can be seconds. So the per-worker
  // profiler calls below are sent to every worker at once and awaited
  // together -- sent one at a time, sixteen workers at fifteen seconds each
  // once took twelve minutes to "start", and the run was over before
  // sampling began.
  const send = (method, params = {}, sessionId, timeoutMs = 15000) => new Promise((res, rej) => {
    if (closed) return rej(new Error(`debugger socket is closed (${closed}); cannot send ${method}`));
    const id = nextId++;
    const timer = setTimeout(() => { if (pending.delete(id)) res(null); }, timeoutMs);
    pending.set(id, m => { clearTimeout(timer); res(m); });
    ws.send(JSON.stringify({ id, method, params, sessionId }));
  });

  const { result: { targetInfos } } = await send('Target.getTargets');
  const pageT = targetInfos.find(t => t.type === 'page');
  const { result: { sessionId: pageS } } =
    await send('Target.attachToTarget', { targetId: pageT.targetId, flatten: true });
  // Auto-attach from the page session as well. Without it the worker
  // sessions attached from the browser level below never answer a Profiler
  // command -- observed, not understood: fourteen workers, zero replies in
  // thirty seconds, and the same code with this line profiles all of them.
  await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, pageS);
  await send('Runtime.enable', {}, pageS);

  // Workers appear as the pthread pool spins them up; scan for new ones on
  // every poll and attach, so a thread created late (the GPU thread) is
  // still profiled from the moment sampling starts.
  const workers = new Map();   // sessionId -> { started }
  const known = new Set();
  const scanWorkers = async () => {
    const r = await send('Target.getTargets');
    for (const t of r?.result?.targetInfos ?? []) {
      if (t.type !== 'worker' || known.has(t.targetId)) continue;
      known.add(t.targetId);
      const a = await send('Target.attachToTarget', { targetId: t.targetId, flatten: true });
      if (a?.result?.sessionId) workers.set(a.result.sessionId, { started: false });
    }
  };
  const evalPage = async expr => {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true }, pageS);
    return r?.result?.result?.value;
  };
  const stageExpr = `(function(){ try { if (typeof Module !== 'undefined' && window.__stagePtr && Module.HEAPU8) { var p = window.__stagePtr, h = Module.HEAPU8, e = p; while (h[e]) e++; return String.fromCharCode.apply(null, h.subarray(p, e)); } } catch (err) {} return '?'; })()`;
  const lastLineExpr = `(function(){ var L = window.__lines || []; return JSON.stringify({ n: L.length, last: L[L.length-1] || '' }); })()`;

  const startAll = async () => {
    const fresh = [...workers].filter(([, w]) => !w.started);
    for (const [, w] of fresh) w.started = true;
    await Promise.all(fresh.map(async ([sid]) => {
      await send('Profiler.enable', {}, sid);
      await send('Profiler.setSamplingInterval', { interval: 200 }, sid);
      await send('Profiler.start', {}, sid);
    }));
  };
  let profiling = false, t0 = 0, stepT0 = 0;
  const results = [];
  // 40,000 polls at 100 ms is over an hour; a gameplay run is ten minutes.
  for (let i = 0; i < 40000; i++) {
    if (closed) throw new Error(`debugger socket closed during the run: ${closed}`);
    await scanWorkers();
    const stage = await evalPage(stageExpr);
    if (!profiling && typeof stage === 'string' && stage.startsWith('stepping')) {
      if (!stepT0) stepT0 = Date.now();
      if (Date.now() - stepT0 >= DELAY_MS) {
        await startAll();
        profiling = true; t0 = Date.now();
        console.log(`profiling started at stage [${stage}] with ${workers.size} workers`);
      }
    } else if (profiling) {
      await startAll();
    }
    const ll = JSON.parse(await evalPage(lastLineExpr) ?? '{"n":0,"last":""}');
    if (/^(PASSED|FAILED)\b/.test(ll.last)) {
      console.log(`run finished after ${Date.now() - t0} ms of profiling`);
      // Stop every worker at once; one that never answers was idle for the
      // whole run and its profile would have been all waits anyway.
      const stopped = await Promise.all([...workers].filter(([, w]) => w.started)
        .map(([sid]) => send('Profiler.stop', {}, sid, 30000)));
      for (const r of stopped) if (r?.result?.profile) results.push(r.result.profile);
      console.log(`${results.length} of ${stopped.length} profiled workers answered`);
      break;
    }
    await sleep(100);
  }
  const lines = await evalPage('JSON.stringify(window.__lines || [])');
  writeFileSync(join(OUTDIR, 'run.txt'), JSON.parse(lines ?? '[]').join('\n'));

  // Self time by function over every worker. Frames in wasm:// modules are
  // either the main module (named, with PROFILING=1) or a compiled block
  // (b_<pc> with --jit-names, else f<k>).
  const agg = new Map();
  let total = 0;
  let pi = 0;
  for (const prof of results) {
    writeFileSync(join(OUTDIR, `profile-${pi++}.cpuprofile`), JSON.stringify(prof));
    const byId = new Map(prof.nodes.map(n => [n.id, n]));
    for (let k = 0; k < prof.samples.length; k++) {
      const n = byId.get(prof.samples[k]);
      const us = prof.timeDeltas[k] ?? 0;
      const name = n.callFrame.functionName || '(anonymous)';
      agg.set(name, (agg.get(name) || 0) + us);
      total += us;
    }
  }
  let out = `total sampled: ${(total / 1000).toFixed(0)} ms over ${results.length} worker profiles\n`;
  out += 'self time by function, every worker (idle pool workers included -- see threads.txt):\n';
  for (const [k, us] of [...agg.entries()].sort((a, b) => b[1] - a[1]).slice(0, 80))
    out += `  ${(us / 1000).toFixed(0).padStart(8)} ms  ${(100 * us / total).toFixed(1).padStart(5)}%  ${k}\n`;
  writeFileSync(join(OUTDIR, 'summary.txt'), out);
  const th = report(OUTDIR);
  writeFileSync(join(OUTDIR, 'threads.txt'), th);
  console.log(th);
  ws.close();
} finally { await cleanup(); }
