use spike_a_throughput::{Guest, GUEST_INSNS_PER_ITER};
use std::time::Instant;

fn bench(name: &str, f: impl Fn(&mut Guest, u64) -> u32, iters: u64) {
    let mut g = Guest::new();
    // warm-up
    std::hint::black_box(f(&mut g, iters / 10));
    let t = Instant::now();
    let r = std::hint::black_box(f(&mut g, iters));
    let secs = t.elapsed().as_secs_f64();
    let mips = (iters * GUEST_INSNS_PER_ITER) as f64 / secs / 1e6;
    println!("{:<8} {:>9.1} MIPS   {:>7.3}s   checksum={:08x}", name, mips, secs, r);
}

fn main() {
    let iters: u64 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(20_000_000);
    println!("native (aarch64)  iters={} insns/iter={}", iters, GUEST_INSNS_PER_ITER);
    bench("naive", |g, n| g.run_naive(n), iters);
    bench("tuned", |g, n| g.run_tuned(n), iters);
    bench("dispatch", |g, n| g.run_dispatched(n), iters);
}
