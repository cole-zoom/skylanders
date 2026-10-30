// Compile a guest program with ppc2wasm natively and write the module, to
// read the emitted wasm (wasm2wat) or the machine code TurboFan makes of it
// (harness/wasm-core/jit-asm.mjs --compile, under node with
// --experimental-wasm-branch-hinting for the hints to count).
//
//   g++ -std=c++23 -O0 harness/wasm-core/dumpblock.cpp -o /tmp/dumpblock
//   /tmp/dumpblock out.wasm [groups] [direct] [hints] [warm] [l1] [regs] -- <hex words...>
//
// The layout is the dispatcher's shape with made-up addresses: the code is
// what is being read, not run, so only the offsets' structure matters.
#include "../../ppc2wasm/src/block.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: dumpblock out.wasm [groups] [direct] [hints] [warm] [l1] [regs] -- "
                         "<hex words...>\n");
    return 2;
  }
  ppc2wasm::StateLayout L;
  L.gpr_offset = 0;
  L.memory_pages = 8192;
  L.memory_shared = true;
  L.ram_base = 0x01000000;
  L.cr_offset = 0x100;
  L.xer_so_ov_offset = 0x108;
  L.xer_ca_offset = 0x10c;
  L.xer_stringctrl_offset = 0x10e;
  L.spr_offset = 0x200;
  L.msr_offset = 0x300;
  L.downcount_offset = 0x304;
  L.pc_offset = 0x30c;
  L.link_base = 0x02000000;
  L.link_mask = 4095;
  L.enable_linking = true;
  L.cr_table_base = 0x03000000;
  L.ps_offset = 0x400;
  L.fpscr_offset = 0x600;
  L.reserve_offset = 0x604;
  L.reserve_address_offset = 0x608;
  L.interp_hatch = true;
  L.hatch_mmio = true;
  L.paired_madd = true;
  L.cr_diff = true;
  L.hatch_exit_index = 1;
  L.mem_slow_index = 2;
  L.exit_value_addr = 0x04000000;
  L.short_convert = true;
  L.region_table = 0x05000000;
  L.gather_pipe_ea = 0xCC008000u;
  L.gather_pipe_ptr_offset = 0x700;
  L.gather_pipe_base_offset = 0x704;
  static const uint32_t gqr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  L.gqr_values = gqr;
  int i = 2;
  for (; i < argc && std::strcmp(argv[i], "--") != 0; ++i)
  {
    if (std::strcmp(argv[i], "groups") == 0)
      L.mem_groups = true;
    else if (std::strcmp(argv[i], "direct") == 0)
    {
      L.direct_base = 0x01000000;
      L.direct_mem2_delta = 0x02040000;
      L.direct_mask = 0x301;
    }
    else if (std::strcmp(argv[i], "hints") == 0)
      L.branch_hints = true;
    else if (std::strcmp(argv[i], "warm") == 0)
      L.warm_entry = true;
    else if (std::strcmp(argv[i], "l1") == 0)
      L.l1_cache_base = 0x06000000;
    else if (std::strcmp(argv[i], "regs") == 0)
      L.reg_cache = true;
    else
    {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      return 2;
    }
  }
  std::vector<uint32_t> prog;
  for (++i; i < argc; ++i)
    prog.push_back(static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 16)));
  if (prog.empty())
  {
    std::fprintf(stderr, "no instructions after --\n");
    return 2;
  }
  auto r = ppc2wasm::CompileBlock(prog.data(), 0x80001000, static_cast<uint32_t>(prog.size()), L);
  std::fprintf(stderr, "compiled %u of %zu (%s), locals %u/%u/%u tail %u, %zu hints, %zu bytes\n",
               r.instructions, prog.size(), r.stopped_on, r.locals, r.locals_i64, r.locals_v128,
               r.locals_tail, r.hints.size(), r.module.size());
  FILE* f = std::fopen(argv[1], "wb");
  if (!f)
    return 1;
  std::fwrite(r.module.data(), 1, r.module.size(), f);
  std::fclose(f);
  return 0;
}
