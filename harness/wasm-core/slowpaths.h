// The compact slow paths compiled blocks call (StateLayout::hatch_exit_index),
// shared by the dispatcher (bootgame.cpp) and the differentials
// (jitdiff.cpp), so what the differential checks is what the game runs.
//
// Included after the file's own ppc_interp_op and g_hatch_state. Each does
// what the inline path it replaces did -- EmitHatchCall, or EmitTranslate's
// miss branch -- and charges the budget exactly as that did: insts_before
// arrives with enable_linking in its top bit.
#pragma once

extern "C" {
u32 g_jit_exit_value = 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE u32 ppc_hatch_exit(u32 state, u32 ins, u32 pc, u32 n)
{
  if (!ppc_interp_op(state, ins, pc))
    return 0;
  auto& st = *g_hatch_state;
  if (n & 0x80000000u)
    st.downcount -= static_cast<s32>((n & 0x7FFFFFFFu) + 1);
  g_jit_exit_value = st.pc;
  return 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE u32 ppc_mem_slow(u32 state, u32 ea, u32 ins, u32 pc, u32 n)
{
  // The hardware registers -- 0xCC00xxxx, 0xCD00xxxx, 0xCD80xxxx -- as
  // EmitIsMmio decides; anything else outside RAM is refused.
  if ((ea & 0xFEFF0000u) == 0xCC000000u || (ea & 0xFFFF0000u) == 0xCD800000u)
    return ppc_hatch_exit(state, ins, pc, n);
  if ((n & 0x80000000u) && (n & 0x7FFFFFFFu))
    g_hatch_state->downcount -= static_cast<s32>(n & 0x7FFFFFFFu);
  g_jit_exit_value = pc | 1u;
  return 1;
}
