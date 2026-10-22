// Can a running WebAssembly program generate and call new WebAssembly code?
//
// Everything in Phase 2 depends on the answer. WebAssembly has no
// writable-executable memory, so the classic JIT -- emit machine code into a
// buffer, jump to it -- is impossible. The alternative is to build a wasm
// MODULE at runtime, let the browser compile it, and install its exports into
// the indirect function table the program already dispatches through.
//
// If that works, a recompiler is possible and Spike A's 863.9 MIPS is
// reachable. If it does not, Phase 2 as planned is dead and the honest answer
// is a faster interpreter.
//
// Three things have to hold, and each is checked separately because they fail
// for different reasons:
//
//   1. a runtime-built module COMPILES          (the encoder is correct)
//   2. it can IMPORT the host's shared memory   (generated code can reach
//                                                guest state, not just do
//                                                arithmetic on arguments)
//   3. its exports can be INSTALLED in the table and called as a function
//      pointer from C++  (dispatch works without a JS round trip per call)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../ppc2wasm/src/block.h"
#include "../../ppc2wasm/src/wasm_emit.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

#if defined(__EMSCRIPTEN__)
// Compile a module and install its export in the indirect function table,
// returning the table index -- which is exactly what a C function pointer is
// in Emscripten, so the result can be called directly from C++.
//
// Returns -1 on failure, with the reason printed. A thrown exception here is
// the normal way a malformed module reports itself, and the message is the
// most useful diagnostic the browser gives.
EM_JS(int, InstallModule, (const uint8_t* ptr, int len), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    // Synchronous compilation. Allowed here because this runs on a worker --
    // Chrome forbids sync compilation of modules over 4 KB on the main
    // thread, which is one more reason PROXY_TO_PTHREAD is not optional.
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory } });
    const fn = inst.exports.f;
    const idx = wasmTable.length;
    wasmTable.grow(1);
    wasmTable.set(idx, fn);
    return idx;
  } catch (e) {
    out('  module install failed: ' + e);
    return -1;
  }
});

EM_JS(int, MemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });
EM_JS(int, MemoryIsShared, (), {
  return (typeof SharedArrayBuffer !== 'undefined' &&
          wasmMemory.buffer instanceof SharedArrayBuffer) ? 1 : 0;
});
#endif

using AddFn = int (*)(int, int);
using LoadFn = int (*)(int);

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

#if !defined(__EMSCRIPTEN__)
  std::printf("target: native -- the encoder, checked against the spec\n\n");

  // LEB128 is where a hand-written encoder goes wrong quietly. These vectors
  // come from the WebAssembly spec's integer encoding; a module whose lengths
  // are off by one byte is rejected outright, but a mis-signed i32.const
  // VALIDATES and computes the wrong number.
  auto enc_u = [](uint32_t v) {
    ppc2wasm::Bytes b;
    ppc2wasm::PutU32(b, v);
    return b;
  };
  auto enc_s = [](int32_t v) {
    ppc2wasm::Bytes b;
    ppc2wasm::PutI32(b, v);
    return b;
  };
  check(enc_u(0) == ppc2wasm::Bytes({0x00}), "u32 0");
  check(enc_u(1) == ppc2wasm::Bytes({0x01}), "u32 1");
  check(enc_u(127) == ppc2wasm::Bytes({0x7F}), "u32 127 is one byte");
  check(enc_u(128) == ppc2wasm::Bytes({0x80, 0x01}), "u32 128 spills to two bytes");
  check(enc_u(624485) == ppc2wasm::Bytes({0xE5, 0x8E, 0x26}), "u32 624485 (spec example)");
  check(enc_s(0) == ppc2wasm::Bytes({0x00}), "s32 0");
  check(enc_s(-1) == ppc2wasm::Bytes({0x7F}), "s32 -1 is 0x7F, not 0xFF");
  check(enc_s(63) == ppc2wasm::Bytes({0x3F}), "s32 63");
  check(enc_s(64) == ppc2wasm::Bytes({0xC0, 0x00}), "s32 64 needs a second byte for the sign");
  check(enc_s(-64) == ppc2wasm::Bytes({0x40}), "s32 -64");
  check(enc_s(-65) == ppc2wasm::Bytes({0xBF, 0x7F}), "s32 -65");
  check(enc_s(-123456) == ppc2wasm::Bytes({0xC0, 0xBB, 0x78}), "s32 -123456 (spec example)");

  // A whole module, byte for byte. f(a) = a, the smallest thing with every
  // section a compiled block needs.
  ppc2wasm::ModuleBuilder mb;
  ppc2wasm::Bytes body;
  body.push_back(ppc2wasm::kLocalGet);
  ppc2wasm::PutU32(body, 0);
  mb.SetFunction({ppc2wasm::kI32}, {ppc2wasm::kI32}, body, "f");
  const auto bytes = mb.Build();
  const ppc2wasm::Bytes expect{
      0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,  // magic, version
      0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,  // type:  (i32) -> i32
      0x03, 0x02, 0x01, 0x00,                          // func:  one, type 0
      0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,        // export: "f" = func 0
      0x0A, 0x06, 0x01, 0x04, 0x00, 0x20, 0x00, 0x0B,  // code:  local.get 0; end
  };
  check(bytes == expect, "a complete module encodes byte for byte as expected");

  // Several functions in ONE module. Checked by structure rather than byte
  // for byte: what matters is that the counts in the function, export and
  // code sections agree with each other, because a module whose section
  // counts disagree is rejected outright and the message says nothing
  // useful about which count was wrong.
  {
    ppc2wasm::ModuleBuilder many;
    many.SetSignature({ppc2wasm::kI32}, {ppc2wasm::kI32});
    ppc2wasm::Bytes b0;
    b0.push_back(ppc2wasm::kLocalGet);
    ppc2wasm::PutU32(b0, 0);
    check(many.AddFunction(b0, "f0") == 0, "the first added function is index 0");
    check(many.AddFunction(b0, "f1") == 1, "the second is index 1");
    check(many.AddFunction(b0, "f2", 3) == 2, "the third is index 2");
    check(many.FunctionCount() == 3, "the builder counts three functions");
    const auto m = many.Build();
    // Section 3 (function) must list three type indices, section 7
    // (export) three names, section 10 (code) three bodies.
    auto section_count = [&](uint8_t id) -> int {
      size_t i = 8;  // past magic and version
      while (i < m.size())
      {
        const uint8_t sid = m[i++];
        uint32_t len = 0, shift = 0;
        while (i < m.size())
        {
          const uint8_t byte = m[i++];
          len |= static_cast<uint32_t>(byte & 0x7F) << shift;
          shift += 7;
          if (!(byte & 0x80))
            break;
        }
        if (sid == id)
          return static_cast<int>(m[i]);  // counts here are all < 128
        i += len;
      }
      return -1;
    };
    check(section_count(3) == 3, "the function section lists three");
    check(section_count(7) == 3, "the export section lists three");
    check(section_count(10) == 3, "the code section lists three");
  }

  // A compiled block hands back its BODY as well as a whole module, so a
  // caller can pack several into one. The two must describe the same code:
  // the body has to appear inside the module the same call produced.
  {
    ppc2wasm::StateLayout L2{.gpr_offset = 20, .memory_pages = 4096, .memory_shared = true};
    const uint32_t two[] = {0x38600005, 0x38830007};
    auto rb = ppc2wasm::CompileBlock(two, 0x80001000, 2, L2);
    check(!rb.body.empty(), "a compiled block exposes its function body");
    check(std::search(rb.module.begin(), rb.module.end(), rb.body.begin(), rb.body.end()) !=
              rb.module.end(),
          "the body is the code inside the module from the same call");
  }
  if (bytes != expect)
  {
    std::printf("    got %zu bytes, want %zu\n", bytes.size(), expect.size());
    for (size_t i = 0; i < bytes.size() && i < expect.size(); ++i)
    {
      if (bytes[i] != expect[i])
      {
        std::printf("    first difference at byte %zu: %02x vs %02x\n", i, bytes[i], expect[i]);
        break;
      }
    }
  }
  // ── the block compiler's decode half ──
  //
  // Emission is only checkable by running it, which needs a browser
  // (harness/wasm-core/jitdiff.cpp). What CAN be checked anywhere is the
  // decoding: the mask PowerPC builds from MB..ME, and where a block stops.
  std::printf("\n");
  using ppc2wasm::detail::Mask;
  check(Mask(0, 31) == 0xFFFFFFFFu, "mask 0..31 is every bit");
  check(Mask(0, 0) == 0x80000000u, "mask 0..0 is the MOST significant bit");
  check(Mask(31, 31) == 0x00000001u, "mask 31..31 is the least significant bit");
  check(Mask(16, 31) == 0x0000FFFFu, "mask 16..31 is the low half");
  check(Mask(0, 15) == 0xFFFF0000u, "mask 0..15 is the high half");
  // MB > ME is legal and WRAPS -- it selects the complement of the gap, not
  // an empty set. Reading it as empty silently drops bits.
  check(Mask(28, 3) == 0xF000000Fu, "mask 28..3 wraps around");

  {
    ppc2wasm::StateLayout L{.gpr_offset = 20, .memory_pages = 4096, .memory_shared = true};
    const uint32_t three[] = {0x38600005, 0x38830007, 0x7CA32214};
    auto r = ppc2wasm::CompileBlock(three, 0x80001000, 3, L);
    check(r.instructions == 3, "three integer instructions compile");
    check(r.end_pc == 0x8000100C, "end pc follows the last instruction");
    check(!r.module.empty(), "a module is produced");

    // Floating point is not translated, so the block must END there rather
    // than skipping it -- translating past an instruction would produce code
    // that silently computes the wrong thing.
    const uint32_t with_float[] = {0x38600005, 0xFC21002A, 0x38830007};  // fadd in the middle
    auto b = ppc2wasm::CompileBlock(with_float, 0x80001000, 3, L);
    check(b.instructions == 1, "a block stops at the first unsupported instruction");
    check(b.end_pc == 0x80001004, "end pc points AT the unsupported instruction");

    // ── static successors ──
    //
    // Where a region compiler is told control can go next. A wrong answer
    // costs a wasted compile and never correctness, but a MISSING one
    // costs batch size, which is the whole point of gathering them.
    {
      // Fall-through only: nothing here ends the block.
      const uint32_t plain[] = {0x38600005, 0x38830007};
      auto a = ppc2wasm::CompileBlock(plain, 0x80001000, 2, L);
      check(a.nsucc == 1 && a.succ[0] == 0x80001008, "a plain block falls through to its end");

      // An unconditional branch goes one place: the target.
      const uint32_t uncond[] = {0x48000010};  // b +16
      auto b2 = ppc2wasm::CompileBlock(uncond, 0x80001000, 1, L);
      check(b2.nsucc == 1 && b2.succ[0] == 0x80001010, "b has one successor, its target");

      // A call has two: the target, and the RETURN SITE, which is a block
      // entry and a hot one.
      const uint32_t call[] = {0x48000011};  // bl +16
      auto c = ppc2wasm::CompileBlock(call, 0x80001000, 1, L);
      check(c.nsucc == 2 && c.succ[0] == 0x80001010 && c.succ[1] == 0x80001004,
            "bl yields both the target and the return site");

      // A conditional branch has both edges.
      const uint32_t cond[] = {0x41820008};  // beq +8
      auto d2 = ppc2wasm::CompileBlock(cond, 0x80001000, 1, L);
      check(d2.nsucc == 2 && d2.succ[0] == 0x80001008 && d2.succ[1] == 0x80001004,
            "a conditional branch yields taken and not-taken");

      // An unsupported instruction: control resumes PAST it, because the
      // interpreter will have run exactly that one.
      const uint32_t stops[] = {0x38600005, 0xFC21002A};  // addi then fadd
      auto e2 = ppc2wasm::CompileBlock(stops, 0x80001000, 2, L);
      check(e2.nsucc == 1 && e2.succ[0] == 0x80001008,
            "a refused instruction is stepped over, not retried");

      // blr's target lives in a register, so only the not-taken edge is
      // knowable.
      const uint32_t ret_cond[] = {0x4D820020};  // beqlr
      auto f2 = ppc2wasm::CompileBlock(ret_cond, 0x80001000, 1, L);
      check(f2.nsucc == 1 && f2.succ[0] == 0x80001004,
            "beqlr yields only its not-taken edge");
      const uint32_t ret_always[] = {0x4E800020};  // blr
      auto g2 = ppc2wasm::CompileBlock(ret_always, 0x80001000, 1, L);
      check(g2.nsucc == 0, "an unconditional blr yields no static successor");
    }

    // A branch IS translated, and terminates the block: control leaves, so
    // whatever follows belongs to a different block.
    const uint32_t with_branch[] = {0x38600005, 0x48000010, 0x38830007};
    auto br = ppc2wasm::CompileBlock(with_branch, 0x80001000, 3, L);
    check(br.instructions == 2, "a branch is translated and ends the block");
    check(br.end_pc == 0x80001008, "end pc is past the branch, not at it");

    // A conditional branch that also decrements CTR IS translated now, and
    // like every branch it ends the block.
    const uint32_t ctr_branch[] = {0x4200FFFC};  // bdnz -4
    auto cb = ppc2wasm::CompileBlock(ctr_branch, 0x80001000, 1, L);
    check(cb.instructions == 1, "a CTR-decrementing branch is translated");
    check(cb.end_pc == 0x80001004, "end pc is past the bdnz");

    // bcctr with BO2 clear remains refused: CTR cannot be both the counter
    // and the branch target, and the architecture calls it invalid.
    const uint32_t ctr_indirect[] = {0x4C000420};  // bcctr, BO2 clear
    auto ci = ppc2wasm::CompileBlock(ctr_indirect, 0x80001000, 1, L);
    check(ci.instructions == 0, "bcctr that decrements CTR is still refused");

    // The dot forms set CR0 and ARE translated now.
    const uint32_t rc_form[] = {0x7C632215};  // add. r3, r3, r4
    auto d = ppc2wasm::CompileBlock(rc_form, 0x80001000, 1, L);
    check(d.instructions == 1, "an Rc (dot) form is translated");
    check(ppc2wasm::detail::RcDestination(0x7C632215) == 3, "add. writes rD");
    check(ppc2wasm::detail::RcDestination(0x7C652378) == 5, "or. writes rA, not rD");
    check(ppc2wasm::detail::RcDestination(0x5464103B) == 4, "rlwinm. writes rA");
    // An Rc bit on a form that has no Rc is an encoding this does not
    // understand, and must still stop the block rather than be guessed at.
    check(ppc2wasm::detail::RcDestination(0x7CA3212F) < 0, "stwx has no Rc destination");
    const uint32_t rc_mem[] = {0x7CA3212F};  // stwx with bit 0 set
    auto dm = ppc2wasm::CompileBlock(rc_mem, 0x80001000, 1, L);
    check(dm.instructions == 0, "a reserved bit on a memory form stops the block");

    // Memory operands compile, and a block that needs scratch locals must
    // actually declare them -- a body that uses local 1 without declaring it
    // is rejected by the browser, which is the good case; the bad case is
    // declaring too few and having an index mean something else.
    const uint32_t loads[] = {0x80830000, 0x90830004};  // lwz r4,0(r3); stw r4,4(r3)
    auto m = ppc2wasm::CompileBlock(loads, 0x80001000, 2, L);
    check(m.instructions == 2, "a load and a store compile");
    check(m.module.size() > 150, "the memory block is substantially larger than a register one");

    // The guard means an unsupported REGION is a runtime decision, not a
    // compile-time one: the instruction still compiles.
    const uint32_t mem2[] = {0x80830000};
    auto g = ppc2wasm::CompileBlock(mem2, 0x80001000, 1, L);
    check(g.instructions == 1, "a load compiles regardless of the address it will compute");

    // Compares write the condition register through Dolphin's own lookup
    // table, so the layout must carry its address; a zero base would emit a
    // load from address 0 that the browser accepts and that reads garbage.
    const uint32_t cmps[] = {0x2C030005, 0x7D832000};  // cmpwi r3,5 ; cmpw cr3,r3,r4
    auto cp = ppc2wasm::CompileBlock(cmps, 0x80001000, 2, L);
    check(cp.instructions == 2, "immediate and register compares both compile");

    const uint32_t nothing[] = {0xFC21002A};  // fadd, not translated
    auto n = ppc2wasm::CompileBlock(nothing, 0x80001000, 1, L);
    check(n.module.empty(), "a block that compiles nothing produces no module");
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
#else
  const int pages = MemoryPages();
  const bool shared = MemoryIsShared() != 0;
  std::printf("target: wasm32  heap=%d pages (%d MB)  shared=%s\n\n", pages, pages / 16,
              shared ? "yes" : "no");

  // ── 1. arithmetic only ──
  // f(a, b) = a + b * 2. No imports, so a failure here is the encoder.
  {
    ppc2wasm::ModuleBuilder mb;
    ppc2wasm::Bytes body;
    body.push_back(ppc2wasm::kLocalGet);
    ppc2wasm::PutU32(body, 0);
    body.push_back(ppc2wasm::kLocalGet);
    ppc2wasm::PutU32(body, 1);
    body.push_back(ppc2wasm::kI32Const);
    ppc2wasm::PutI32(body, 2);
    body.push_back(ppc2wasm::kI32Mul);
    body.push_back(ppc2wasm::kI32Add);
    mb.SetFunction({ppc2wasm::kI32, ppc2wasm::kI32}, {ppc2wasm::kI32}, body, "f");

    const auto bytes = mb.Build();
    std::printf("  module is %zu bytes\n", bytes.size());
    const int idx = InstallModule(bytes.data(), static_cast<int>(bytes.size()));
    check(idx >= 0, "runtime-built module compiled and installed in the table");
    if (idx >= 0)
    {
      AddFn f = reinterpret_cast<AddFn>(idx);
      const int r = f(3, 4);
      std::printf("  f(3, 4) = %d (want 11)\n", r);
      check(r == 11, "called generated code through a C function pointer");
      check(f(-5, 10) == 15, "signed arguments survive the call");
    }
  }

  // ── 2. reaching guest state ──
  // f(addr) = *(int*)addr + 1, with the host's memory imported. Arithmetic on
  // arguments would be useless for a recompiler; the emitted code has to read
  // and write the same heap the emulator keeps guest state in.
  {
    ppc2wasm::ModuleBuilder mb;
    mb.ImportMemory(static_cast<uint32_t>(pages), static_cast<uint32_t>(pages), shared);
    ppc2wasm::Bytes body;
    body.push_back(ppc2wasm::kLocalGet);
    ppc2wasm::PutU32(body, 0);
    body.push_back(ppc2wasm::kI32Load);
    ppc2wasm::PutU32(body, 2);  // alignment hint: 2^2 = 4 bytes
    ppc2wasm::PutU32(body, 0);  // offset
    body.push_back(ppc2wasm::kI32Const);
    ppc2wasm::PutI32(body, 1);
    body.push_back(ppc2wasm::kI32Add);
    mb.SetFunction({ppc2wasm::kI32}, {ppc2wasm::kI32}, body, "f");

    const auto bytes = mb.Build();
    const int idx = InstallModule(bytes.data(), static_cast<int>(bytes.size()));
    check(idx >= 0, "module importing the host's shared memory installed");
    if (idx >= 0)
    {
      static int cell = 0;
      cell = 41;
      LoadFn f = reinterpret_cast<LoadFn>(idx);
      const int r = f(static_cast<int>(reinterpret_cast<uintptr_t>(&cell)));
      std::printf("  loaded %d from host memory (want 42)\n", r);
      check(r == 42, "generated code read a C++ variable out of the shared heap");
    }
  }

  // ── 3. many modules ──
  // A recompiler installs thousands of blocks. If the table cannot grow
  // repeatedly, or instantiation leaks badly, that shows up here rather than
  // after a day of building a decoder.
  {
    int installed = 0;
    int sum = 0;
    for (int k = 0; k < 64; ++k)
    {
      ppc2wasm::ModuleBuilder mb;
      ppc2wasm::Bytes body;
      body.push_back(ppc2wasm::kI32Const);
      ppc2wasm::PutI32(body, k);
      mb.SetFunction({}, {ppc2wasm::kI32}, body, "f");
      const auto bytes = mb.Build();
      const int idx = InstallModule(bytes.data(), static_cast<int>(bytes.size()));
      if (idx < 0)
        break;
      ++installed;
      sum += reinterpret_cast<int (*)()>(idx)();
    }
    std::printf("  installed %d modules, constants sum to %d (want 2016)\n", installed, sum);
    check(installed == 64, "64 separate modules installed into a growing table");
    check(sum == 64 * 63 / 2, "every installed module returns its own constant");
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
#endif
}
