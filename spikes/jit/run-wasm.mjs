// Spike A wasm harness. Runs under Node (V8) — same engine as Chrome.
import { readFileSync } from 'node:fs';

const iters = BigInt(process.argv[2] ?? 20_000_000);
const bytes = readFileSync(new URL('./spike.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(bytes, {});
const x = instance.exports;

const insns = x.spike_insns_per_iter();
console.log(`wasm32 (V8 ${process.versions.v8.split('-')[0]})  iters=${iters} insns/iter=${insns}`);

function bench(name, fn) {
  const g = x.spike_new();
  fn(g, iters / 10n);                      // warm-up
  const t = process.hrtime.bigint();
  const r = fn(g, iters);
  const secs = Number(process.hrtime.bigint() - t) / 1e9;
  const mips = Number(iters * insns) / secs / 1e6;
  console.log(
    `${name.padEnd(8)} ${mips.toFixed(1).padStart(9)} MIPS   ` +
    `${secs.toFixed(3).padStart(7)}s   checksum=${(r >>> 0).toString(16).padStart(8, '0')}`
  );
}

bench('naive', x.spike_run_naive);
bench('tuned', x.spike_run_tuned);
bench('dispatch', x.spike_run_dispatched);
