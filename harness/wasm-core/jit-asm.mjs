// The machine code V8's optimizing tier makes of a compiled block.
//
//   node harness/wasm-core/jit-asm.mjs <run output> [out dir]
//
// A run with --jit-dump=PC[,PC...] prints the module holding each of those
// blocks ("JITMOD <pc> f<k> <bytes>", then base64 "F " lines). This writes
// each module to <out dir>/<pc>.wasm and re-runs itself under
// `node --no-liftoff --print-wasm-code`, which compiles every function with
// TurboFan when the module is created -- no instantiation, so the imports
// (the game's memory, table and hatch) need not exist -- and keeps only the
// listing of the block asked for. Node's V8 is older than Chrome's, so read
// it for the shape of the code, not for exact instruction counts. Node
// needs the branch-hinting flag for the hints (StateLayout::branch_hints) to
// place the cold code as Chrome does.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.argv[2] === '--compile') {
  new WebAssembly.Module(readFileSync(process.argv[3]));
  process.exit(0);
}

const [log, outDir = '/tmp/jit-asm'] = process.argv.slice(2);
if (!log) {
  console.error('usage: node harness/wasm-core/jit-asm.mjs <run output> [out dir]');
  process.exit(2);
}
mkdirSync(outDir, { recursive: true });
const mods = [];
let cur = null;
for (const line of readFileSync(log, 'utf8').split('\n')) {
  const head = /^JITMOD ([0-9a-f]{8}) f(\d+) (\d+)$/.exec(line);
  if (head) {
    cur = { pc: head[1], k: Number(head[2]), size: Number(head[3]), b64: '' };
    mods.push(cur);
    continue;
  }
  if (cur && line.startsWith('F ')) cur.b64 += line.slice(2);
  else cur = null;
}
const self = fileURLToPath(import.meta.url);
for (const m of mods) {
  const bytes = Buffer.from(m.b64, 'base64');
  if (bytes.length !== m.size) {
    console.error(`${m.pc}: got ${bytes.length} of ${m.size} bytes, skipped`);
    continue;
  }
  const file = join(outDir, `${m.pc}.wasm`);
  writeFileSync(file, bytes);
  const out = execFileSync(process.execPath,
    ['--no-liftoff', '--no-wasm-lazy-compilation', '--experimental-wasm-branch-hinting', '--print-wasm-code', self, '--compile', file],
    { encoding: 'utf8', maxBuffer: 1 << 28 });
  // Function k of the module follows the imports; its listing is the one
  // named wasm-function[k + imports] (or b_<pc> with --jit-names).
  const imports = WebAssembly.Module.imports(new WebAssembly.Module(bytes))
    .filter(i => i.kind === 'function').length;
  const want = new RegExp(`^name: (wasm-function\\[${m.k + imports}\\]|b_${m.pc})$`);
  const listings = out.split('--- WebAssembly code ---');
  const mine = listings.find(t => t.split('\n').some(l => want.test(l)));
  const text = mine ? mine.split('--- End code ---')[0] : '(no listing found)\n';
  writeFileSync(join(outDir, `${m.pc}.s`), text);
  const n = (text.match(/^0x[0-9a-f]+ /gm) || []).length;
  console.log(`${m.pc}: ${bytes.length} module bytes, ${n} machine instructions -> ${join(outDir, m.pc)}.s`);
}
