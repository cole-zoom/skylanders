// How much of a module install is per MODULE, and how much is per block?
//
// docs/PHASE2-PROFILE.md measures installation at 27.8% of an 800M-
// instruction boot: 76,533 calls to new WebAssembly.Module at about 22 us
// each. Whether packing several blocks into one module helps depends
// entirely on where that 22 us goes. If it is fixed per module, batching
// divides it. If it scales with the code inside, batching buys nothing and
// the next idea is a different one.
//
// So: build modules holding 1, 2, 4 ... 512 real compiled blocks, install
// each, and report the cost per block. One run answers it.
#include <cstdio>
#include <vector>

#include "../../ppc2wasm/src/block.h"

#include <emscripten.h>

EM_JS(int, InstallBatch, (const uint8_t* ptr, int len, int nfuncs), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, {
      env: { memory: wasmMemory, __indirect_function_table: wasmTable },
    });
    const base = wasmTable.length;
    wasmTable.grow(nfuncs);
    for (let i = 0; i < nfuncs; i++)
      wasmTable.set(base + i, inst.exports['f' + i]);
    return base;
  } catch (e) {
    out('  install failed: ' + e);
    return -1;
  }
});

EM_JS(int, MemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });
EM_JS(double, Now, (), { return performance.now(); });

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("install cost against blocks per module\n\n");

  ppc2wasm::StateLayout layout;
  layout.gpr_offset = 32;
  layout.memory_pages = static_cast<uint32_t>(MemoryPages());
  layout.memory_shared = true;
  layout.ram_base = 0x10000;
  layout.cr_offset = 1024;
  layout.xer_so_ov_offset = 1100;
  layout.spr_offset = 1200;
  layout.cr_table_base = 2048;

  // A block of a realistic shape and length: the measured average is 2.82
  // instructions per entry, but blocks that end at a branch average 5.16,
  // and those are what a batch would mostly hold.
  const uint32_t prog[] = {
      0x3860'0005,  // li   r3, 5
      0x8083'0010,  // lwz  r4, 16(r3)
      0x7CA3'2214,  // add  r5, r3, r4
      0x2C05'0000,  // cmpwi r5, 0
      0x4182'0008,  // beq  +8
  };
  auto r = ppc2wasm::CompileBlock(prog, 0x8000'1000, 5, layout);
  std::printf("  one block: %u instructions, body %zu bytes, module %zu bytes\n\n",
              r.instructions, r.body.size(), r.module.size());

  std::printf("  %8s %10s %12s %14s\n", "blocks", "bytes", "install ms", "us per block");
  for (int n = 1; n <= 512; n *= 2)
  {
    // A fresh module each time, and distinct export names, so nothing is
    // shared between measurements.
    ppc2wasm::ModuleBuilder mb;
    mb.ImportMemory(layout.memory_pages, layout.memory_pages, layout.memory_shared);
    mb.SetSignature({ppc2wasm::kI32}, {ppc2wasm::kI32});
    for (int i = 0; i < n; ++i)
      mb.AddFunction(r.body, "f" + std::to_string(i), r.locals);
    const auto bytes = mb.Build();

    // Repeat small batches so the total is well above clock resolution.
    const int reps = n >= 64 ? 2 : (512 / n);
    const double t0 = Now();
    for (int k = 0; k < reps; ++k)
      if (InstallBatch(bytes.data(), static_cast<int>(bytes.size()), n) < 0)
        return 1;
    const double ms = Now() - t0;
    std::printf("  %8d %10zu %12.1f %14.2f\n", n, bytes.size(), ms,
                ms * 1000.0 / (static_cast<double>(n) * reps));
  }
  std::printf("\ndone\n");
  return 0;
}
