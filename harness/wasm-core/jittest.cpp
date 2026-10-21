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
#include <cstdio>
#include <cstring>
#include <vector>

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
