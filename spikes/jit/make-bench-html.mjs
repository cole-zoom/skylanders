// Generate a self-contained bench.html: wasm inlined as base64 and
// instantiated SYNCHRONOUSLY, so the whole benchmark completes during parse
// and `chrome --headless --dump-dom` captures finished results. No server,
// no automation dependency.
import { readFileSync, writeFileSync } from 'node:fs';

const b64  = readFileSync('spike.wasm').toString('base64');
const core = readFileSync('bench-core.js', 'utf8').replace('export function', 'function');
const iters  = process.argv[2] ?? '30000000';
const trials = process.argv[3] ?? '7';

writeFileSync('bench.html', `<!doctype html><meta charset=utf8>
<title>Spike A</title><pre id=out>running...</pre>
<script>
${core}
const lines = [];
const log = (s) => lines.push(s);
try {
  const bin = Uint8Array.from(atob("${b64}"), c => c.charCodeAt(0));
  // Synchronous instantiation: no await, so results exist before load fires.
  const inst = new WebAssembly.Instance(new WebAssembly.Module(bin), {});
  const x = inst.exports;
  const iters = ${iters}n, trials = ${trials};
  const { seed, bench, checkCalls } = runAll(x, iters, trials, log);
  log("wasm32 chrome  iters=" + iters + " trials=" + trials + " seed=" + seed);
  bench('naive',    x.spike_run_naive);
  bench('tuned',    x.spike_run_tuned);
  bench('dispatch', x.spike_run_dispatched);\n  checkCalls(iters, trials);
} catch (e) { log("ERROR " + e); }
document.getElementById('out').textContent = lines.join("\\n");
</script>`);
console.log('bench.html written (' + (b64.length / 1024).toFixed(0) + ' KB base64)');
