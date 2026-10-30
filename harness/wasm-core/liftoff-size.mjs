// Liftoff machine-code size per wasm body byte, for the blocks a run dumped.
//
//   node harness/wasm-core/liftoff-size.mjs <run output with JITMOD lines> [out dir]
//
// Why: Chrome optimises a function once its budget of machine-code bytes
// executed is spent (docs/PHASE3-WEBGPU.md, "warming"), and the dispatcher
// spends it with --jit-warm using an assumed bytes-per-wasm-byte ratio;
// this is where that ratio was measured (1.25-2.9 over this game's hot
// blocks, 1.9 in the middle). The .wasm files it writes are also what
// tierup-probe/index.html's mod= and queue= modes load.
//
// Writes each dumped module to <out dir>/<pc>.wasm, compiles it in node with
// Liftoff only (--liftoff --no-wasm-tier-up --no-wasm-lazy-compilation
// --print-wasm-code), and prints per function: wasm body bytes, Liftoff
// instructions bytes, the ratio. Node's V8 (12.x) is older than Chrome's
// 15.6, so this is a calibration of the ratio, not an exact size.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.argv[2] === '--compile') {
  new WebAssembly.Module(readFileSync(process.argv[3]));
  process.exit(0);
}
const [log, outDir = '/tmp/liftoff-size'] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
const mods = [];
let cur = null;
for (const line of readFileSync(log, 'utf8').split('\n')) {
  const head = /^JITMOD ([0-9a-f]{8}) f(\d+) (\d+)$/.exec(line);
  if (head) { cur = { pc: head[1], k: Number(head[2]), size: Number(head[3]), b64: '' }; mods.push(cur); continue; }
  if (cur && line.startsWith('F ')) cur.b64 += line.slice(2);
  else cur = null;
}
// wasm body sizes from the code section
function bodySizes(bytes) {
  let p = 8;
  const u32 = () => { let r = 0, s = 0, b; do { b = bytes[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
  while (p < bytes.length) {
    const id = bytes[p++]; const len = u32(); const end = p + len;
    if (id === 10) { const n = u32(); const out = []; for (let i = 0; i < n; i++) { const sz = u32(); out.push(sz); p += sz; } return out; }
    p = end;
  }
  return [];
}
const self = fileURLToPath(import.meta.url);
const rows = [];
const seen = new Set();
for (const m of mods) {
  if (seen.has(m.pc)) continue;
  seen.add(m.pc);
  const bytes = Buffer.from(m.b64, 'base64');
  if (bytes.length !== m.size) { console.error(`${m.pc}: got ${bytes.length} of ${m.size} bytes, skipped`); continue; }
  const file = join(outDir, `${m.pc}.wasm`);
  writeFileSync(file, bytes);
  const out = execFileSync(process.execPath,
    ['--liftoff', '--no-wasm-tier-up', '--no-wasm-dynamic-tiering', '--no-wasm-lazy-compilation', '--print-wasm-code', self, '--compile', file],
    { encoding: 'utf8', maxBuffer: 1 << 28 });
  const imports = WebAssembly.Module.imports(new WebAssembly.Module(bytes)).filter(i => i.kind === 'function').length;
  const sizes = bodySizes(bytes);
  for (const listing of out.split('--- WebAssembly code ---').slice(1)) {
    const idx = /^index: (\d+)/m.exec(listing);
    const ins = /^Instructions \(size = (\d+)\)/m.exec(listing);
    const comp = /^compiler: (\w+)/m.exec(listing);
    if (!idx || !ins) continue;
    const k = Number(idx[1]) - imports;
    if (k < 0 || k >= sizes.length) continue;
    rows.push({ mod: m.pc, k, body: sizes[k], liftoff: Number(ins[1]), compiler: comp?.[1] });
  }
}
rows.sort((a, b) => a.body - b.body);
for (const r of rows) console.log(`${r.mod} f${r.k} ${r.compiler} body ${r.body} liftoff ${r.liftoff} ratio ${(r.liftoff / r.body).toFixed(2)}`);
const ratios = rows.map(r => r.liftoff / r.body).sort((a, b) => a - b);
const q = f => ratios[Math.min(ratios.length - 1, Math.floor(f * ratios.length))];
console.log(`${rows.length} functions: ratio min ${q(0).toFixed(2)} p10 ${q(0.1).toFixed(2)} median ${q(0.5).toFixed(2)} p90 ${q(0.9).toFixed(2)} max ${q(0.999).toFixed(2)}; body bytes median ${rows[Math.floor(rows.length / 2)]?.body}, liftoff median ${[...rows].sort((a, b) => a.liftoff - b.liftoff)[Math.floor(rows.length / 2)]?.liftoff}`);
