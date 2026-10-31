// The browser shell: boot the emulator into the page's canvas and hand it
// the player's input.
//
// Query parameters, all optional:
//   speed=F       emulation speed limit, 1 = real time (default), 0 = unlimited
//   figure=ID:VAR the Skylander on the portal at boot (default Whirlwind, 0:0)
//   log=1         show the emulator's output under the canvas
//   audio=0       no sound
import { attachAudio } from './audio.js';
import { attachInput } from './input.js';
import { attachPortal } from './portal.js';
import { keepSaves, loadSaves, restoreSaves } from './saves.js';
import { attachStates } from './states.js';

const $ = id => document.getElementById(id);
const params = new URLSearchParams(location.search);
const status = $('status');
const logEl = $('log');
// The boot's first lines are kept whole (they say what loaded and what
// failed), then a rolling tail.
const head = [], lines = [];
// Lines for the server (see the speed readout below): the log, and a speed.
let readouts = [];

const say = (text, failed = false) => {
  status.textContent = text;
  status.hidden = !text;
  status.classList.toggle('failed', failed);
};
const log = text => {
  readouts.push(`${new Date().toISOString()} log ${text}`);
  if (head.length < 200) head.push(text);
  else lines.push(text);
  if (lines.length > 400) lines.splice(0, lines.length - 400);
  if (!logEl.hidden) logEl.textContent = [...head, '…', ...lines].join('\n');
};
logEl.hidden = params.get('log') !== '1';

// What the build needs, checked up front: a missing feature otherwise
// surfaces as an abort deep in startup.
const missing = [];
if (!crossOriginIsolated) missing.push('cross-origin isolation (serve with web/serve.mjs)');
if (!navigator.gpu) missing.push('WebGPU');
if (!('Suspending' in WebAssembly)) missing.push('JSPI (WebAssembly.Suspending)');
if (missing.length) {
  say(`This browser is missing: ${missing.join(', ')}. Use a current Chrome or Edge.`, true);
  throw new Error('unsupported browser');
}

const speed = params.get('speed') ?? '1';
const audio = params.get('audio') !== '0';
const figure = params.get('figure');
// The run never ends on its own; the step count is only a cap.
// The disc: the server's range endpoint next to the page, or the URL
// ?disc= names -- a Blob store on another origin, when the page is hosted
// where no server serves ranges (web/vercel.json).
const disc = params.get('disc') ?? new URL('disc', location.href).href;
const args = [disc, '1000000000000000000',
              '--jit', '--dual-core', '--webgpu', '--no-osd', '--live-input',
              ...(audio ? ['--audio'] : []),
              // The NAND where web/saves.js restores the game's saves.
              '--keep-nand',
              `--speed=${speed}`, figure ? `--skylander=${figure}` : '--skylander',
              // The game's pipelines, compiled at boot rather than the first
              // time each is drawn (web/make-uidcache.sh). Absent, the game
              // still runs, with a stall at each new pipeline.
              `--uidcache-url=${new URL('dist/SSPE52.uidcache', location.href).href}`];

let stagePtr = 0, secondsPtr = 0;
const readStage = () => {
  const h = Module.HEAPU8;
  let e = stagePtr;
  while (h[e]) e++;
  return new TextDecoder().decode(h.slice(stagePtr, e));
};

// The speed readout: emulated seconds per wall second, over the last second.
// Every reading is also sent to the server (web/serve.mjs writes them to a
// file), so a play session leaves a record of where it was slow.
let lastEmulated = 0, lastWall = 0, lastCompiles = 0, lastCompileMs = 0;
// keepalive (a send that outlives the page) only when the page is going:
// the browser refuses a keepalive body over 64 KB, and the boot's log is more.
const sendReadouts = (keepalive = false) => {
  if (!readouts.length) return;
  fetch('readout', { method: 'POST', body: readouts.join('\n') + '\n', keepalive })
    .catch(() => {});
  readouts = [];
};
setInterval(sendReadouts, 10000);
addEventListener('pagehide', () => sendReadouts(true));
const hud = () => {
  const stage = readStage();
  if (!stage.startsWith('stepping')) {
    say(`Booting… ${stage}`);
  } else {
    say('');
    const emulated = new Float64Array(Module.HEAPU8.buffer, secondsPtr, 1)[0];
    const wall = performance.now() / 1000;
    // A loaded save state moves the emulated clock; that second is skipped.
    const pct = (100 * (emulated - lastEmulated)) / (wall - lastWall);
    // And what was compiled in that second: a slow second spent compiling
    // is a stall, not the emulator running slowly.
    const compiles = Module._harness_compiles?.() ?? 0;
    const compileMs = Module._harness_compile_ms?.() ?? 0;
    if (lastWall && pct >= 0 && pct < 1000) {
      $('speed').textContent = `${pct.toFixed(0)}%`;
      readouts.push(`${new Date().toISOString()} emulated ${emulated.toFixed(1)} s ` +
                    `readout ${pct.toFixed(0)}% compiled ${compiles - lastCompiles} ` +
                    `in ${(compileMs - lastCompileMs).toFixed(0)} ms`);
    }
    lastEmulated = emulated;
    lastWall = wall;
    lastCompiles = compiles;
    lastCompileMs = compileMs;
  }
};

// The saves kept in this browser, read before the module starts so they
// can be put back before main() runs. Without IndexedDB the game still
// runs, from a fresh NAND each time.
const saves = await loadSaves().catch(e => (log(`! saves unavailable: ${e.message}`), null));

window.Module = {
  canvas: $('canvas'),
  preRun: [() => { if (saves) restoreSaves(Module.FS, saves.files); }],
  arguments: args,
  print: log,
  printErr: text => log('! ' + text),
  onAbort: what => say(`The emulator stopped: ${what}`, true),
  onExit: code => say(`The emulator exited (${code}).`, code !== 0),
  onRuntimeInitialized() {
    stagePtr = Module._harness_stage();
    secondsPtr = Module._harness_emulated_seconds();
    attachInput($('canvas'), Module.HEAPU8.buffer, Module._harness_live_input());
    $('portal').hidden = false;
    attachPortal(Module, $('portal'));
    if (saves) {
      $('states').hidden = false;
      attachStates(Module, $('states')).catch(e => log(`! save states unavailable: ${e.message}`));
      $('saves').hidden = false;
      keepSaves(Module.FS, saves, { status: $('saves-status'), exportButton: $('saves-export'),
                                    importButton: $('saves-import'), resetButton: $('saves-reset') });
    }
    if (audio)
      attachAudio(Module.HEAPU8.buffer, Module._harness_audio(), $('sound'))
        .catch(e => log(`! sound failed: ${e.message}`));
    else
      $('sound').hidden = true;
    setInterval(hud, 1000);
  },
};

$('fullscreen').addEventListener('click', () => $('screen').requestFullscreen?.());

say('Loading…');
const script = document.createElement('script');
script.src = 'dist/skylanders.js';
script.onerror = () => say('dist/skylanders.js is missing -- run web/build.sh first.', true);
document.body.append(script);
