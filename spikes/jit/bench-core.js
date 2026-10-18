// Shared benchmark driver. Used by both the Node harness and the browser page.
export function runAll(x, iters, trials, log) {
  const insns = x.spike_insns_per_iter();
  // Fixed seed so checksums are comparable across targets. Devirtualization
  // is prevented by the table being filled through an exported function, not
  // by the value being unpredictable.
  const seed = 0x5EED0001;
  x.spike_install_blocks(seed);

  const median = (a) => {
    const s = [...a].sort((p, q) => p - q);
    const n = s.length;
    return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
  };

  const bench = (name, fn) => {
    const g = x.spike_new();
    fn(g, iters / 10n);                       // warm-up
    const mips = [];
    let ck = 0;
    for (let i = 0; i < trials; i++) {
      const t = performance.now();
      ck = fn(g, iters);
      mips.push(Number(iters * insns) / ((performance.now() - t) / 1000) / 1e6);
    }
    log(
      `${name.padEnd(9)} ${median(mips).toFixed(1).padStart(8)} MIPS  ` +
      `(min ${Math.min(...mips).toFixed(1).padStart(7)}, max ${Math.max(...mips).toFixed(1).padStart(7)})  ` +
      `checksum=${(ck >>> 0).toString(16).padStart(8, '0')}`
    );
  };

  const checkCalls = (iters, trials) => {
    const expect = iters / 10n + iters * BigInt(trials);
    const got = x.spike_block_calls();
    log(`block calls: ${got} expected ${expect}  ` +
        (got === expect ? 'OK' : 'MISMATCH -- dispatch did not run as modelled'));
  };
  return { insns, seed, bench, checkCalls, x };
}
