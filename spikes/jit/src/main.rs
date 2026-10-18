use spike_a_throughput::{spike_install_blocks, Guest, GUEST_INSNS_PER_ITER};
use std::time::Instant;

fn median(mut v: Vec<f64>) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let n = v.len();
    if n % 2 == 1 { v[n / 2] } else { (v[n / 2 - 1] + v[n / 2]) / 2.0 }
}

fn bench(name: &str, f: impl Fn(&mut Guest, u64) -> u32, iters: u64, trials: usize) {
    let mut g = Guest::new();
    std::hint::black_box(f(&mut g, iters / 10)); // warm-up
    let mut mips = Vec::new();
    let mut ck = 0u32;
    for _ in 0..trials {
        let t = Instant::now();
        ck = std::hint::black_box(f(&mut g, iters));
        mips.push((iters * GUEST_INSNS_PER_ITER) as f64 / t.elapsed().as_secs_f64() / 1e6);
    }
    let lo = mips.iter().cloned().fold(f64::INFINITY, f64::min);
    let hi = mips.iter().cloned().fold(0.0, f64::max);
    println!("{:<9} {:>8.1} MIPS  (min {:>7.1}, max {:>7.1})  checksum={:08x}",
             name, median(mips), lo, hi, ck);
}

fn main() {
    let iters: u64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(30_000_000);
    let trials: usize = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(7);
    // Fixed seed so checksums are comparable across targets. Devirtualization
    // is prevented by the table being filled through an exported function,
    // not by the value being unpredictable.
    let seed: u32 = 0x5EED_0001;
    spike_install_blocks(seed);
    println!("native aarch64   iters={} trials={} insns/iter={} seed={}",
             iters, trials, GUEST_INSNS_PER_ITER, seed);
    bench("naive",    |g, n| g.run_naive(n), iters, trials);
    bench("tuned",    |g, n| g.run_tuned(n), iters, trials);
    bench("dispatch", |g, n| g.run_dispatched(n), iters, trials);
    // Semantic check: the dispatch loop must have made exactly one indirect
    // call per iteration, warm-up included.
    let expect = iters / 10 + iters * trials as u64;
    let got = spike_a_throughput::spike_block_calls();
    println!("block calls: {} expected {}  {}", got, expect,
             if got == expect { "OK" } else { "MISMATCH -- dispatch did not run as modelled" });
}
