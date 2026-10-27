// Drive the shell in headless Chrome: load the page, press keys the way a
// player would (through Chrome's input pipeline, not by writing memory), and
// report what the page shows.
//
//   node web/smoke.mjs [seconds] [out-dir] [query]
//
// Every 2 s it taps Space (A) for 0.2 s -- the same schedule as the
// harness's --input=A, which is what takes the game from boot to gameplay --
// prints the page's status and speed readout every 10 s, and writes a
// screenshot to out-dir every 30 s (SHOT_FROM, SHOT_EVERY change that).
// MOVE_FROM=N: from N s on, walk instead -- hold W, then D, 2 s each -- so
// the stick is tested too. Needs web/build.sh to have run and the disc at
// /tmp/ssa.iso.
//
// SWAP_AT=N SWAP_TO=name: at N s, take every figure off the portal and put
// on the first one the portal panel finds for `name`, through the panel's
// own buttons.
//
// STATES=120:save1,300:load1: at those seconds, press the save-state
// panel's Save or Load button for that slot, and report how it went.
//
// COPY_OUT=<path>: at the end, copy this file out of the emulator's
// filesystem into out-dir (e.g. Cache/SSPE52.uidcache, relative to the
// emulator's user directory).
//
// KEEP_PROFILE=<dir>: use (and keep) this Chrome profile, so the saves one
// run leaves in the browser are there for the next.
//
// HEADED=1: a visible window, as a player has it. At the end it prints the
// readout's distribution over the walk (or over the run without
// MOVE_FROM): one sample per readout update, the number a player sees.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const seconds = Number(process.argv[2] ?? 120);
const out = process.argv[3] ?? join(tmpdir(), 'skylanders-smoke');
const query = process.argv[4] ?? 'log=1';
const PORT = 8090, DEBUG = 9351;
mkdirSync(out, { recursive: true });

const here = dirname(fileURLToPath(import.meta.url));
// The page's readout goes to this run's own log, not a player's.
const server = spawn(process.execPath, [join(here, 'serve.mjs'), '/tmp/ssa.iso', String(PORT)],
                     { stdio: 'ignore',
                       env: { ...process.env, READOUT_LOG: join(out, 'readout.log') } });
const profile = process.env.KEEP_PROFILE ?? mkdtempSync(join(tmpdir(), 'shell-'));
mkdirSync(profile, { recursive: true });
let chrome;
try {
  await sleep(500);
  // A visible Chrome with a fresh profile opens its sign-in page first.
  const headless = process.env.HEADED ? ['--no-first-run', '--no-default-browser-check']
                                      : ['--headless'];
  chrome = spawn(CHROME, [...headless, '--no-sandbox', `--remote-debugging-port=${DEBUG}`,
                          '--enable-unsafe-webgpu', '--window-size=1100,900',
                          `--user-data-dir=${profile}`,
                          `http://127.0.0.1:${PORT}/?${query}`], { stdio: 'ignore' });
  const ours = t => t.type === 'page' && t.url.includes(`:${PORT}/`);
  let targets;
  for (let i = 0; i < 60 && !targets?.some(ours); i++) {
    try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json`)).json(); }
    catch {}
    await sleep(250);
  }
  const page = targets.find(ours);
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => (ws.onopen = r));
  let id = 0;
  const send = (method, params = {}) => new Promise(res => {
    const me = ++id;
    const timer = setTimeout(() => res(null), 15000);
    const h = ev => {
      const m = JSON.parse(ev.data);
      if (m.id === me) { clearTimeout(timer); ws.removeEventListener('message', h); res(m.result); }
    };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id: me, method, params }));
  });
  const evaluate = async expr =>
    (await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }))
      ?.result?.value;
  const KEYS = { Space: [' ', 32], KeyW: ['w', 87], KeyD: ['d', 68] };
  const key = (type, code = 'Space') => send('Input.dispatchKeyEvent',
    { type, code, key: KEYS[code][0], windowsVirtualKeyCode: KEYS[code][1] });
  const moveFrom = Number(process.env.MOVE_FROM ?? Infinity);
  let swapAt = Number(process.env.SWAP_AT ?? Infinity);
  const stateActions = (process.env.STATES ?? '').split(',').filter(Boolean).map(a => {
    const [at, what] = a.split(':');
    return { at: Number(at), op: what.replace(/\d+$/, ''), slot: Number(what.match(/\d+$/)[0]) };
  });
  let walks = 0;
  // Every readout the page shows, as it shows it.
  const watch = from => evaluate(`(() => {
    if (!document.getElementById('speed')) return false;
    window.__readouts = [];
    new MutationObserver(() => {
      const v = parseFloat(document.getElementById('speed').textContent);
      if (!isNaN(v) && performance.now() / 1000 >= ${from}) window.__readouts.push(v);
    }).observe(document.getElementById('speed'), { childList: true, characterData: true, subtree: true });
    return true;
  })()`);
  const tPage = (await evaluate('performance.now() / 1000')) ?? 0;
  while (!(await watch(tPage + (Number.isFinite(moveFrom) ? moveFrom : 0)))) await sleep(100);

  const t0 = Date.now();
  let nextReport = 0, nextShot = Number(process.env.SHOT_FROM ?? 30), shot = 0;
  while ((Date.now() - t0) / 1000 < seconds) {
    const t = (Date.now() - t0) / 1000;
    if (t >= nextReport) {
      const s = await evaluate(`JSON.stringify({
        status: document.getElementById('status').hidden ? '' : document.getElementById('status').textContent,
        speed: document.getElementById('speed').textContent,
        sound: document.getElementById('sound').textContent,
        saves: document.getElementById('saves-status').textContent,
        // The sound ring (bootgame.cpp's AudioRing): frames written, read,
        // the worklet's short quanta, and the loudest sample in it now.
        audio: (() => {
          if (!Module._harness_audio) return null;
          const a = Module._harness_audio(), b = Module.HEAPU8.buffer;
          const ix = new Uint32Array(b, a, 4), d = new Int16Array(b, a + 16, 2 * ix[2]);
          let peak = 0;
          for (const v of d) peak = Math.max(peak, Math.abs(v));
          return { written: ix[0], read: ix[1], underruns: ix[3], peak };
        })() })`);
      console.log(`t=${t.toFixed(0)}s ${s}`);
      nextReport += 10;
    }
    if (stateActions.length && t >= stateActions[0].at) {
      const { op, slot } = stateActions.shift();
      const r = await evaluate(`(async () => {
        const root = document.getElementById('states');
        const li = root.querySelectorAll('li')[${slot - 1}];
        const b = [...li.querySelectorAll('button')].find(b => b.textContent === ${JSON.stringify(op === 'save' ? 'Save' : 'Load')});
        b.click();
        const st = root.querySelector('.state-status');
        for (let i = 0; i < 600 && (!st.textContent || st.textContent.endsWith('…')); i++)
          await new Promise(r => setTimeout(r, 100));
        return st.textContent;
      })()`);
      console.log(`t=${t.toFixed(0)}s state ${op} ${slot}: ${r}`);
    }
    if (t >= swapAt) {
      swapAt = Infinity;
      const r = await evaluate(`(async () => {
        const p = document.getElementById('portal');
        const slots = () => [...p.querySelectorAll('.slots li')].map(li => li.textContent).join(' | ');
        const before = slots();
        for (const b of p.querySelectorAll('.slots button')) b.click();
        await new Promise(r => setTimeout(r, 1500));
        const s = p.querySelector('input');
        s.value = ${JSON.stringify(process.env.SWAP_TO ?? '')};
        s.dispatchEvent(new Event('input'));
        p.querySelector('.figures button')?.click();
        await new Promise(r => setTimeout(r, 1500));
        return before + ' -> ' + slots();
      })()`);
      console.log(`t=${t.toFixed(0)}s portal: ${r}`);
    }
    if (t >= nextShot) {
      const r = await send('Page.captureScreenshot', { format: 'png' });
      if (r?.data) writeFileSync(join(out, `shot-${String(++shot).padStart(2, '0')}.png`),
                                 Buffer.from(r.data, 'base64'));
      nextShot += Number(process.env.SHOT_EVERY ?? 30);
    }
    if (t >= moveFrom) {
      const code = walks++ % 2 ? 'KeyD' : 'KeyW';
      await key('keyDown', code);
      await sleep(2000);
      await key('keyUp', code);
      continue;
    }
    await key('keyDown');
    await sleep(200);
    await key('keyUp');
    await sleep(1800);
  }
  const r = (await evaluate('window.__readouts')) ?? [];
  if (r.length) {
    const v = [...r].sort((a, b) => a - b);
    const at = p => v[Math.min(v.length - 1, Math.floor(p * v.length))];
    console.log(`readout over ${v.length} s${Number.isFinite(moveFrom) ? ' of walking' : ''}: ` +
                `median ${at(0.5)}%, p25 ${at(0.25)}%, p10 ${at(0.1)}%, worst ${v[0]}%; ` +
                `${v.filter(x => x < 90).length} below 90%`);
  }
  if (process.env.COPY_OUT) {
    const b64 = await evaluate(`(() => {
      const d = Module.FS.readFile('/tmp/dolphin-boot-user/' + ${JSON.stringify(process.env.COPY_OUT)});
      let s = ''; for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode(...d.subarray(i, i + 0x8000));
      return btoa(s); })()`);
    if (b64) writeFileSync(join(out, process.env.COPY_OUT.split('/').pop()), Buffer.from(b64, 'base64'));
    console.log(`${process.env.COPY_OUT}: ${b64 ? 'copied' : 'not found'}`);
  }
  const log = await evaluate(`document.getElementById('log').textContent`);
  writeFileSync(join(out, 'log.txt'), log ?? '');
  console.log(`screenshots and log.txt in ${out}`);
  ws.close();
} finally {
  for (const p of [chrome, server]) { try { p?.kill(); } catch {} }
  await sleep(300);
  if (!process.env.KEEP_PROFILE)
    try { rmSync(profile, { recursive: true, force: true }); } catch {}
}
