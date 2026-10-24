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

// Refuse to start while anything holds the ports. A Chrome or a server left
// over from a killed run keeps them, the new ones fail to bind with their
// stdio ignored, and this runner then attaches to the OLD page and reports
// what it printed -- the same output for every run, whatever was built.
// That cost two misleading A/Bs before this check.
{
  const { createServer } = await import('node:net');
  for (const port of [8101, 8102, 9341]) {
    const free = await new Promise(res => {
      const srv = createServer().once('error', () => res(false))
                                .once('listening', () => srv.close(() => res(true)));
      srv.listen(port, '127.0.0.1');
    });
    if (!free) {
      console.log(`port ${port} is in use -- a process from an earlier run? ` +
                  `lsof -nP -iTCP:${port} -sTCP:LISTEN`);
      process.exit(2);
    }
  }
}
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
                          // WebGPU in headless Chrome (the WebGPU backend,
                          // docs/PHASE3-WEBGPU.md); without it a WEBGPU=1
                          // build's canvas setup failed at startup.
                          '--enable-unsafe-webgpu',
                          // Room for the whole 640x528 canvas: the default
                          // headless viewport is shorter, and a screenshot
                          // of what lies below it comes back white.
                          '--window-size=1024,768',
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
  // Every CDP call races a timeout.
  //
  // Without one this awaits a reply FOREVER: if a response is dropped or the
  // socket hiccups, the promise never settles, the poll loop stops iterating,
  // and the run hangs with the process alive and Chrome healthy. That
  // happened -- a capture sat wedged for an hour, and because the loop was no
  // longer iterating it never reached its own iteration cap to report a
  // timeout. A hang that cannot even time out is the worst version of this.
  //
  // 15s is enormous for a round trip that normally takes single-digit
  // milliseconds, so it only fires when something is genuinely wrong, and a
  // null return feeds the existing bad-poll tolerance rather than aborting.
  const CDP_TIMEOUT_MS = 15000;
  const send = (id, method, params) => new Promise(res => {
    let done = false;
    const finish = v => {
      if (done) return;
      done = true;
      ws.removeEventListener('message', h);
      clearTimeout(timer);
      res(v);
    };
    const h = ev => { const m = JSON.parse(ev.data); if (m.id === id) finish(m); };
    const timer = setTimeout(() => finish(null), CDP_TIMEOUT_MS);
    ws.addEventListener('message', h);
    try { ws.send(JSON.stringify({ id, method, params })); } catch { finish(null); }
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
  // Stall watchdog.
  //
  // Deliberately NOT a wall-clock cap on the run. The stepping phase is
  // silent for many minutes by design -- a 256 KB capture is about 4 billion
  // interpreted instructions -- so "no output for N minutes" would kill a
  // perfectly healthy run. What it watches instead is PROGRESS: new output
  // lines, or the stage marker, which bootgame advances every ~4M steps
  // precisely so that a working-but-silent run still looks alive.
  //
  // 5 minutes of neither moving means something is actually wrong.
  const STALL_MS = 5 * 60 * 1000;
  let lastProgress = Date.now();
  let lastStage = '';
  // ~2 hours of idle polls. A boot-and-compare run finishes in minutes, but
  // capturing 256 KB of command stream takes 4 BILLION instructions, which the
  // wasm interpreter does far slower than native. The old 20-minute cap timed
  // such a run out while it was still making progress, which is the worst
  // failure mode: it looks like a hang and is not one.
  for (let i = 0; i < 14400; i++) {
    const r = await send(i + 1, 'Runtime.evaluate', { returnByValue: true, expression: `
      (function () {
        var L = window.__lines || [];
        var stage = '?';
        try {
          // Read through the pointer the page captured in
          // onRuntimeInitialized -- never by calling the export from here.
          // Calling it before the runtime is up ABORTS the module, so a poll
          // arriving a moment too early kills the run it is observing. That
          // happened. Module.calledRun is not a usable guard either: under
          // PROXY_TO_PTHREAD main() runs on a worker and it never becomes
          // true on this thread.
          if (typeof Module !== 'undefined' && window.__stagePtr && Module.HEAPU8) {
            var p = window.__stagePtr, h = Module.HEAPU8, e = p;
            while (h[e]) e++;
            stage = String.fromCharCode.apply(null, h.subarray(p, e));
          }
        } catch (err) { stage = 'read failed: ' + err.message; }
        // Cap the chunk so one poll can never ship an unbounded payload.
        var chunk = L.slice(${from}, ${from} + 4000);
        return JSON.stringify({ n: L.length, done: !!window.__done, stage: stage, chunk: chunk });
      })()` });
    // A timed-out call resolves to null (see send()); that is a bad poll
    // like any other, not a reason for the runner itself to die.
    const v = r?.result?.result?.value;
    if (typeof v !== 'string') {
      // One bad poll is not a dead page -- at startup the document may not
      // have run its inline script yet. Only a sustained run of failures
      // means the page is actually gone (crash, OOM, navigation), and that
      // has to be reported rather than quietly yielding a partial trace.
      const why = r?.result?.exceptionDetails?.exception?.description
               ?? r?.result?.exceptionDetails?.text ?? JSON.stringify(r?.result ?? null);
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
    // Completion is detected from the OUTPUT, not only from window.__done.
    //
    // bootgame ends with _exit() rather than returning -- a booted emulator
    // leaves threads and device state live, and unwinding them terminates in
    // libc++abi after the result has printed. But _exit() bypasses
    // Emscripten's onExit, so window.__done never becomes true, and a run that
    // finished perfectly looks identical to one that hung: output stops, and
    // the stage marker (only written inside the step loop) stops too.
    //
    // That is not hypothetical -- a completed capture was reported STALLED
    // for exactly this reason, with all 4,600 lines of its payload already
    // drained.
    const finished = out.length && /^(PASSED|FAILED)\b/.test(out[out.length - 1]);
    if ((done || finished) && from >= n) break;

    if (chunk.length || stage !== lastStage) {
      lastStage = stage;
      lastProgress = Date.now();
    } else if (Date.now() - lastProgress > STALL_MS) {
      console.log(`STALLED — no output and no stage change for ` +
                  `${Math.round(STALL_MS / 60000)} min ` +
                  `(stage=[${stage}], ${from} lines). Giving up.`);
      break;
    }
    if (i > 0 && i % 60 === 0)
      console.log(`--- t=${i / 2}s, ${from} lines, stage=[${stage}]`);
    // Only idle when there was nothing to collect; a producing run should be
    // drained as fast as the round trip allows.
    if (!chunk.length) await sleep(500);
    if (i === 14399) console.log(`TIMEOUT (stage=[${stage}], ${from} lines)`);
  }
  console.log(out.join('\n'));
  // SCREENSHOT=<file.png>: what the canvas shows at the end of the run --
  // the last frame the WebGPU backend presented.
  if (process.env.SCREENSHOT) {
    const box = await send(9002, 'Runtime.evaluate', { returnByValue: true,
      expression: 'JSON.stringify(document.getElementById("canvas").getBoundingClientRect())' });
    console.log(`canvas box ${box?.result?.result?.value}`);
    const shot = await send(9001, 'Page.captureScreenshot',
      { format: 'png', clip: { x: 0, y: 0, width: 640, height: 528, scale: 1 } });
    if (shot?.result?.data) {
      const { writeFileSync } = await import('node:fs');
      writeFileSync(process.env.SCREENSHOT, Buffer.from(shot.result.data, 'base64'));
      console.log(`screenshot -> ${process.env.SCREENSHOT}`);
    } else {
      console.log('screenshot failed');
    }
  }
  ws.close();
} finally { await cleanup(); }
