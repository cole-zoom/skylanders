// Node (V8) harness — same engine as Chrome, no browser overhead.
import { readFileSync } from 'node:fs';
import { runAll } from './bench-core.js';

const iters  = BigInt(process.argv[2] ?? 30_000_000);
const trials = Number(process.argv[3] ?? 7);
const bytes  = readFileSync(new URL('./spike.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(bytes, {});
const x = instance.exports;

const { seed, bench, checkCalls } = runAll(x, iters, trials, console.log);
console.log(`wasm32 node/V8 ${process.versions.v8.split('-')[0]}  iters=${iters} trials=${trials} seed=${seed}`);
bench('naive',    x.spike_run_naive);
bench('tuned',    x.spike_run_tuned);
bench('dispatch', x.spike_run_dispatched);
checkCalls(iters, trials);
