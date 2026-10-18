//! Spike A — PowerPC → WebAssembly throughput.
//!
//! QUESTION: can recompiled Broadway code sustain enough guest instructions
//! per second in a browser to run Skylanders at full speed?
//!
//! Gate (docs/PLAN.md): >= 600 MIPS sustained. Broadway is 729 MHz and retires
//! close to 1 IPC on game code, so ~600 MIPS is roughly 100% speed with a
//! little headroom.
//!
//! WHAT THIS MEASURES
//!
//! Not an emulator. This is the *shape* of what a recompiler emits for a hot
//! basic block, hand-written in Rust so it compiles to roughly the WebAssembly
//! a good `ppc2wasm` backend would produce. We run the identical source
//! natively (aarch64) and as wasm32 under V8, and compare.
//!
//! The workload is a vertex-transform-ish kernel, chosen because it is the
//! instruction mix Skylanders actually spends its CPU budget on: a tight loop
//! of guest loads, paired-single float math, integer address arithmetic,
//! compares and branches.
//!
//! THE TWO VARIANTS exist to price the single biggest unknown in the plan —
//! how much the lack of host flags and the lack of fastmem actually cost.
//!
//!   naive  — eager condition-register computation, full bounds-check per
//!            guest memory access. What you get if you translate literally.
//!   tuned  — lazy CR (record operands, resolve on read) and power-of-two
//!            address masking instead of compare-and-branch.
//!
//! The delta between them is the value of two specific design decisions, and
//! `tuned` is the number that should be compared against the gate.

/// 24 MB MEM1. Power of two so the tuned path can mask instead of branch.
pub const MEM1_SIZE: usize = 32 * 1024 * 1024;
pub const MEM1_MASK: u32 = (MEM1_SIZE as u32) - 1;
/// Wii maps MEM1 at 0x8000_0000 (cached).
pub const MEM1_BASE: u32 = 0x8000_0000;

/// Guest instructions retired per iteration of the kernel loop body.
///
/// Counted by hand from `kernel_body` below, one guest instruction per
/// modelled operation. This is the number MIPS is derived from, so it is
/// deliberately conservative — where a Rust expression could plausibly stand
/// for two PowerPC instructions, it is counted as one.
///
///   4  lwz/lfs   guest loads
///   6  fmuls/fadds/fmadds  paired-single arithmetic
///   2  stw/stfs  guest stores
///   4  add/rlwinm/or  integer address and index arithmetic
///   2  cmpw + bc  compare and conditional branch
///  ---
///  18
pub const GUEST_INSNS_PER_ITER: u64 = 18;

/// Lazily-evaluated condition register.
///
/// Most PowerPC arithmetic sets CR0 and most of those results are never read.
/// Storing the operands and resolving on demand moves the cost to the ~15% of
/// sites that actually branch on it.
#[derive(Clone, Copy)]
pub struct LazyCr {
    lhs: i32,
    rhs: i32,
}

impl LazyCr {
    #[inline(always)]
    fn record(&mut self, lhs: i32, rhs: i32) {
        self.lhs = lhs;
        self.rhs = rhs;
    }
    /// Resolve to the 4-bit CR field: LT, GT, EQ, SO.
    #[inline(always)]
    fn resolve(&self) -> u32 {
        let (l, r) = (self.lhs, self.rhs);
        if l < r {
            0b1000
        } else if l > r {
            0b0100
        } else {
            0b0010
        }
    }
    #[inline(always)]
    fn lt(&self) -> bool {
        self.lhs < self.rhs
    }
}

pub struct Guest {
    pub mem: Vec<u8>,
    pub gpr: [u32; 32],
    pub cr: u32,
    pub lazy_cr: LazyCr,
}

impl Guest {
    pub fn new() -> Self {
        let mut mem = vec![0u8; MEM1_SIZE];
        // Deterministic fill so both builds compute the same checksum.
        let mut s: u32 = 0x1234_5678;
        for c in mem.chunks_mut(4) {
            s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            c.copy_from_slice(&s.to_be_bytes());
        }
        Guest { mem, gpr: [0; 32], cr: 0, lazy_cr: LazyCr { lhs: 0, rhs: 0 } }
    }

    // ── naive memory path ──
    // Models what a recompiler emits for a literal translation: an explicit
    // range compare with a branch to the MMU slow path, then a raw load.
    // Raw pointers deliberately: Rust's own bounds check is not part of the
    // thing being measured, and leaving it in measures Vec, not the MMU.
    #[inline(always)]
    fn read_u32_checked(&self, ea: u32) -> u32 {
        let off = ea.wrapping_sub(MEM1_BASE) as usize;
        if off + 4 > MEM1_SIZE {
            return 0; // slow path
        }
        unsafe { u32::from_be(core::ptr::read_unaligned(self.mem.as_ptr().add(off) as *const u32)) }
    }
    #[inline(always)]
    fn write_u32_checked(&mut self, ea: u32, v: u32) {
        let off = ea.wrapping_sub(MEM1_BASE) as usize;
        if off + 4 > MEM1_SIZE {
            return;
        }
        unsafe { core::ptr::write_unaligned(self.mem.as_mut_ptr().add(off) as *mut u32, v.to_be()) }
    }

    // ── tuned memory path ──
    // Mask into a power-of-two arena, then the same raw load. No branch.
    #[inline(always)]
    fn read_u32_masked(&self, ea: u32) -> u32 {
        let off = (ea & MEM1_MASK & !3) as usize;
        unsafe { u32::from_be(core::ptr::read_unaligned(self.mem.as_ptr().add(off) as *const u32)) }
    }
    #[inline(always)]
    fn write_u32_masked(&mut self, ea: u32, v: u32) {
        let off = (ea & MEM1_MASK & !3) as usize;
        unsafe { core::ptr::write_unaligned(self.mem.as_mut_ptr().add(off) as *mut u32, v.to_be()) }
    }

    /// Eager-CR + checked-memory variant.
    pub fn run_naive(&mut self, iters: u64) -> u32 {
        let mut acc: u32 = 0;
        let mut idx: u32 = 0;
        for _ in 0..iters {
            let base = MEM1_BASE.wrapping_add(idx & 0x00FF_FFF0);

            // 4 guest loads
            let a = self.read_u32_checked(base);
            let b = self.read_u32_checked(base.wrapping_add(4));
            let c = self.read_u32_checked(base.wrapping_add(8));
            let d = self.read_u32_checked(base.wrapping_add(12));

            // 6 paired-single ops, modelled in f32
            let (fa, fb) = (f32::from_bits(a), f32::from_bits(b));
            let (fc, fd) = (f32::from_bits(c), f32::from_bits(d));
            let x = fa * fc + fb * fd;
            let y = fa * fd - fb * fc;
            let z = x + y;

            // 2 guest stores
            self.write_u32_checked(base, x.to_bits());
            self.write_u32_checked(base.wrapping_add(4), y.to_bits());

            // 4 integer ops
            idx = idx.wrapping_add(16).wrapping_mul(1_103_515_245) >> 3;
            acc = acc.wrapping_add(z.to_bits() ^ idx);

            // 2: cmpw + bc, with CR computed eagerly whether or not it is read
            self.cr = {
                let (l, r) = (acc as i32, 0x4000_0000i32);
                if l < r {
                    0b1000
                } else if l > r {
                    0b0100
                } else {
                    0b0010
                }
            };
            if self.cr & 0b1000 == 0 {
                idx = idx.wrapping_add(1);
            }
        }
        acc
    }

    /// Lazy-CR + masked-memory variant.
    pub fn run_tuned(&mut self, iters: u64) -> u32 {
        let mut acc: u32 = 0;
        let mut idx: u32 = 0;
        for _ in 0..iters {
            let base = MEM1_BASE.wrapping_add(idx & 0x00FF_FFF0);

            let a = self.read_u32_masked(base);
            let b = self.read_u32_masked(base.wrapping_add(4));
            let c = self.read_u32_masked(base.wrapping_add(8));
            let d = self.read_u32_masked(base.wrapping_add(12));

            let (fa, fb) = (f32::from_bits(a), f32::from_bits(b));
            let (fc, fd) = (f32::from_bits(c), f32::from_bits(d));
            let x = fa * fc + fb * fd;
            let y = fa * fd - fb * fc;
            let z = x + y;

            self.write_u32_masked(base, x.to_bits());
            self.write_u32_masked(base.wrapping_add(4), y.to_bits());

            idx = idx.wrapping_add(16).wrapping_mul(1_103_515_245) >> 3;
            acc = acc.wrapping_add(z.to_bits() ^ idx);

            // Record operands only; resolve just the bit the branch reads.
            self.lazy_cr.record(acc as i32, 0x4000_0000i32);
            if !self.lazy_cr.lt() {
                idx = idx.wrapping_add(1);
            }
        }
        // One resolve at block exit, standing in for an interrupt boundary.
        self.cr = self.lazy_cr.resolve();
        acc
    }
}

/// Realistic variant: guest state lives in memory, and each block is a
/// separate function reached through a function pointer — which lowers to
/// `call_indirect` on wasm32, exactly as a real dispatcher would.
///
/// This is the number that actually predicts the emulator, because it pays the
/// two costs `run_tuned` hides: no cross-block register allocation, and an
/// indirect call per block.
pub type BlockFn = fn(&mut Guest) -> bool;

#[inline(never)]
pub fn block_body(g: &mut Guest) -> bool {
    let idx = g.gpr[3];
    let base = MEM1_BASE.wrapping_add(idx & 0x00FF_FFF0);

    let a = g.read_u32_masked(base);
    let b = g.read_u32_masked(base.wrapping_add(4));
    let c = g.read_u32_masked(base.wrapping_add(8));
    let d = g.read_u32_masked(base.wrapping_add(12));

    let (fa, fb) = (f32::from_bits(a), f32::from_bits(b));
    let (fc, fd) = (f32::from_bits(c), f32::from_bits(d));
    let x = fa * fc + fb * fd;
    let y = fa * fd - fb * fc;
    let z = x + y;

    g.write_u32_masked(base, x.to_bits());
    g.write_u32_masked(base.wrapping_add(4), y.to_bits());

    let nidx = idx.wrapping_add(16).wrapping_mul(1_103_515_245) >> 3;
    let acc = g.gpr[4].wrapping_add(z.to_bits() ^ nidx);
    g.gpr[3] = nidx;
    g.gpr[4] = acc;

    g.lazy_cr.record(acc as i32, 0x4000_0000i32);
    !g.lazy_cr.lt()
}

impl Guest {
    /// Dispatch loop: fetch block pointer from a table, indirect-call it.
    pub fn run_dispatched(&mut self, iters: u64) -> u32 {
        let table: [BlockFn; 2] = [block_body, block_body];
        self.gpr[3] = 0;
        self.gpr[4] = 0;
        let mut sel = 0usize;
        for _ in 0..iters {
            let f = core::hint::black_box(table[sel & 1]);
            if f(self) {
                self.gpr[3] = self.gpr[3].wrapping_add(1);
            }
            sel = sel.wrapping_add(1);
        }
        self.gpr[4]
    }
}

#[no_mangle]
pub extern "C" fn spike_run_dispatched(g: *mut Guest, iters: u64) -> u32 {
    unsafe { (*g).run_dispatched(iters) }
}

// ── wasm32 entry points, called from the Node harness ──

#[no_mangle]
pub extern "C" fn spike_new() -> *mut Guest {
    Box::into_raw(Box::new(Guest::new()))
}

#[no_mangle]
pub extern "C" fn spike_run_naive(g: *mut Guest, iters: u64) -> u32 {
    unsafe { (*g).run_naive(iters) }
}

#[no_mangle]
pub extern "C" fn spike_run_tuned(g: *mut Guest, iters: u64) -> u32 {
    unsafe { (*g).run_tuned(iters) }
}

#[no_mangle]
pub extern "C" fn spike_insns_per_iter() -> u64 {
    GUEST_INSNS_PER_ITER
}
