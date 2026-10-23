# Phase 3 — code density, and the road to full speed

Written for whoever picks this up next. `docs/PHASE2-PROFILE.md` is the
current state and the measurements behind it; this is the plan that falls
out of them.

**Read `docs/PHASE2-PROFILE.md` first, especially "Three things that should
have worked and did not".** Three structural optimisations were built,
measured and reverted. Every one of them is implemented and flagged off in
the tree, and rebuilding any of them costs a day.

---

## Where it stands

195.8 MIPS booting *Skylanders: Spyro's Adventure* in Chrome, 800 million
instructions. Full speed is roughly 600. The budget:

```
total                         4,085 ms
  compiled execution          2,051 ms  50%   788.8M instructions at 2.6 ns
  module installation           686 ms  17%   22,862 modules, 83,842 blocks
  interpreter fallback          612 ms  15%   11.2M instructions
  dispatcher round-trips        595 ms  15%   32.5M at 18.3 ns
  translation                   170 ms   4%
```

Take the headline and the breakdown from different runs: the link counters
are behind `--jit-stats` and cost about 6%.

---

## The one result everything here rests on

`harness/wasm-core/jitspike.cpp`. A guest instruction costs **0.44 ns when
the compiled code fits in cache and 2.25 ns when there is a megabyte of
it**:

| blocks | KB of code | entry ns | ns/instruction | = MIPS |
|---|---|---|---|---|
| 1 | 0 | 1.7 | **0.44** | 2286 |
| 256 | 66 | 29.4 | 0.70 | 1428 |
| 1024 | 262 | 49.5 | 0.83 | 1202 |
| 4096 | 1050 | 76.0 | **2.25** | 445 |

The boot measures **2.6 ns per instruction**. It sits at the bottom of that
curve. The emitted code is capable of roughly 2,300 MIPS and delivers 400
because there is too much of it.

What fills the cache:

| mix | bytes of wasm per guest instruction |
|---|---|
| arithmetic | 16.4 |
| 1-in-4 memory | 34.9 |
| all memory | **93.9** |

This is also why the three reverted optimisations did nothing: **none of
them reduced bytes of code and two increased them.**

So the governing variable is code density, and the plan is to stop emitting
the same sequences inline in every block.

---

## Stage 0 — prove the ceiling before building anything

Everything below assumes a CALL to a shared helper is cheaper than the
bytes it replaces. That assumption has the same shape as the three that
already failed, so it gets tested before anything is built on it.

Extend `jitspike.cpp`: emit a variant where the link epilogue is a separate
function **in the same module** and blocks reach it with `return_call`.
Report bytes per instruction and MIPS at 256, 1024 and 4096 blocks. Same
module is enough here — it needs only the multi-function support
`ModuleBuilder` already has — and it answers the question without any of
Stage 1.

**Kill gate.** If density falls by 2× or more and MIPS does not move, stop
and go back to measuring. The density model is then wrong, and the next
thing to find out is what the 2.25 ns is actually made of — the spike
cannot currently separate instruction-cache pressure from indirect-branch
misprediction, and that would become the question.

Expect roughly half a day.

---

## Stage 1 — ModuleBuilder: function imports, and a second signature

Two capabilities, no behaviour change.

**Function imports.** Imported functions occupy the low function indices,
so every defined function shifts up by the import count. **This breaks
direct linking.** `PatchDirectCalls` writes index *k* for pending block
*k*, and it must become *k + imports*. A wrong index here is valid wasm
that calls the wrong function — it will not fail to install, it will
quietly run someone else's block.

**A second type.** All functions share one signature today, which is what
makes a direct call cheap to emit. Helpers need their own.

Assert both in `harness/wasm-core/jittest.cpp`, which is the only part of
this that CI runs on every push.

Verified by the whole suite staying byte-identical.

---

## Stage 2 — the runtime module, and the epilogue

The largest single item. At 3.02 instructions per block entry the link
epilogue — downcount, budget test, hash, table load, stored-PC compare,
tail call — is **more bytes than the block body it belongs to**.

Build one runtime module at startup exporting the helpers, and have every
block module import them. One copy process-wide, permanently hot; not one
per region.

A block exit becomes:

```wasm
local.get 0                 ;; state
i32.const <target>
i32.const <insts>
return_call $epilogue       ;; about 7 bytes, replacing about 80
```

`$epilogue` does what the inline sequence did and ends in
`return_call_indirect` into the successor. A tail call made from a
tail-called helper keeps chain depth constant, so nothing about chaining
changes.

This **composes with direct linking** rather than replacing it, and the
shape gets simpler than it is today. Push the state, then the six-byte
hole, then the two extra arguments and `return_call $epilogue`:

- patched — the direct sibling call fires and everything after is
  unreachable, which WebAssembly validates against a polymorphic stack;
- unpatched — the hole is six `nop`s and all three arguments are on the
  stack for the epilogue.

The current `drop`-based fallback and its counter adjustments disappear.

---

## Stage 3 — memory accesses, at 94 bytes each

The obstacle is the guard. A helper cannot bail out of its *caller*, and a
refused access has to return from the block with the bail marker so the
interpreter re-runs that instruction.

Use a multi-value return: `load32(state, ea) -> (value, ok)`, and the block
does `if (!ok) return bail_pc | 1`. Call site about 12 bytes against 94.

**This is the stage most likely to fail.** Memory is roughly a quarter of
the instruction stream, so a call per access may cost more than the density
buys. Measure it on its own and revert it on its own.

If it fails, the fallback worth trying is narrower: keep the guard inline,
where it is cheap and well predicted, and out-of-line only the address
translation and the four-byte big-endian reassembly.

---

## Stage 4 — the CR0 update

About 26 wasm instructions at every dot form, all identical. Same pattern,
smaller prize, and worth doing only if Stage 3 showed the call overhead is
acceptable.

---

## Independent of density, and worth interleaving

**Installation, 17%.** Regions average 3.7 blocks against a measured
ceiling of about 1.4 µs per block at sixteen (`harness/wasm-core/jitbatch.cpp`).
The blocker is 13,743 regions that find exactly one block. Walking forward
in memory to fill them was measured and is **net negative** — a block costs
about 2 µs to translate whether or not it ever runs. What has not been
tried is deferring singletons across misses and accepting that such a block
is interpreted until its batch flushes.

**Interpreter fallback, 15%.** 11.2M instructions, and it concentrates.
Run with `--jit-stats` for the current ranking; at the last reading `dcbz`
was 31%, `addic.` 11%, `subfic` 6%, `mftb` 6%, `adde` 5%, `dcbst` 3%.
`dcbz` needs HID0.DCE and the low-memory hack checked at runtime; the carry
forms need XER[CA], which nothing writes yet.

---

## What this adds up to, and what it does not

| | saves |
|---|---|
| density 3× — compiled execution 2,051 → ~710 ms | ~1,340 ms |
| installation batching | ~500 ms |
| interpreter fallback | ~360 ms |
| cheaper dispatch, longer chains | ~200 ms |

About **475 MIPS**. That is 2.4× today and it is **not 600**.

There is no measured route to 600 and this document is not going to invent
one. The candidate worth spiking is a change of representation: stop
emitting one wasm function per block, and move toward a small,
permanently-hot interpreter loop in wasm over a compact encoded form, so
that "code" becomes data and the instruction-cache problem disappears by
construction. It trades per-instruction speed for cache behaviour, which is
the right direction given the curve above. It is a different architecture
and it needs its own Stage 0 before anyone commits to it.

---

## How to work on this

```sh
harness/wasm-core/run-jitdiff.sh                          # all four differentials, ~15s
harness/wasm-core/run-jit.sh 800000000 --jit              # the number
harness/wasm-core/run-jit.sh 800000000 --jit --jit-stats  # the breakdowns, ~6% slower
node /tmp/jitspike.js                                     # the density curve
g++ -std=c++23 -O1 harness/wasm-core/jittest.cpp -o /tmp/jittest && /tmp/jittest
```

Run the differentials **and** `jittest.cpp` before every push. They cover
different things — one runs emitted code, the other checks the decoder and
the encoder — and only the second is gated by CI on push. A change to which
instructions `CompileBlock` accepts needs both. The nightly workflow runs
the differentials unattended, which means a regression is caught the next
morning rather than in the pull request that caused it.

Measure at several step counts. Installation is a one-off, so a short run
reports a speed dominated by compiling: the same build does 54.7 MIPS over
200M instructions and 195.8 over 800M.

The boot's final PC is **not** a correctness signal. It differs between
every configuration including the interpreter. Correctness evidence is the
differentials.

### Specific to this work

A bug in a shared helper is a bug in **every block at once**. That is an
argument for the differentials before each push, not after, and for
mutation-testing each helper the way the existing ones were — break it,
confirm the suite goes red, revert.

**Anything that increases code size is now suspect by default.** That is
the opposite of the instinct that produced the last three failures, and it
applies to this plan as much as to anything else: a helper that is called
from one place is just a slower inline sequence.

The differentials generate blocks of up to 41 instructions because 11 was
not enough to catch a real bug. If a stage here makes blocks longer, that
number should grow with them.
