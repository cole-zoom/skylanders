#!/usr/bin/env python3
"""Tests for the GX command stream decoder.

Every stream here is SYNTHETIC, built byte by byte from the opcode formats in
VideoCommon/OpcodeDecoding.h. Nothing is derived from the game disc, so these
run anywhere -- which is the point: they are the part of the differential
harness that needs no 4.7 GB disc, no Emscripten toolchain and no browser, and
so they are the part that can gate every push.

Run:  python3 harness/src/test_decode.py
"""
import struct
import sys

from fifo_inventory import decode
from gx_vertex import VertexFormats
from gx_arrays import GuestRAM, ArrayDescriptors
from collections import Counter

FAILURES = []


def check(cond, what):
    print(f"  {what:<58} {'ok' if cond else 'FAIL'}")
    if not cond:
        FAILURES.append(what)


def state():
    return dict(bp=Counter(), xf=Counter(), prims=Counter(), unknown_ops=Counter(),
                verts=0, tev_stages=set(), texgens=set(), cull=set(),
                blend_enable=set(), logicop_enable=set(), dither=set(),
                src_factor=set(), dst_factor=set(), subtract=set(),
                logic_ops=set(), dst_alpha_used=set(), ztest=set(), zwrite=set(),
                alpha_test=0, efb_copies=0, tev_color_writes=0,
                tex_formats=set(), tex_filters=set(), line_point_width=0,
                stopped_at_draw=False, truncated=False, display_lists=0,
                display_list_bytes=0, bp_initial={}, decoded_bytes=0,
                total_bytes=0, vfmt=VertexFormats(), cp=Counter(),
                vtx_formats=Counter(), desync=False, cur_blend=0,
                arrays=ArrayDescriptors(), ram=GuestRAM(), export=None)


# ── stream builders, from VideoCommon/OpcodeDecoding.h ──
def nop(count=1):
    return b"\x00" * count


def cp(addr, val):
    return b"\x08" + bytes([addr]) + struct.pack(">I", val)


def bp(reg, val):
    return b"\x61" + struct.pack(">I", (reg << 24) | (val & 0xFFFFFF))


def xf(addr, words):
    return b"\x10" + struct.pack(">I", ((len(words) - 1) << 16) | addr) + \
        b"".join(struct.pack(">I", w) for w in words)


def call_dl(addr, size):
    return b"\x40" + struct.pack(">I", addr) + struct.pack(">I", size)


def draw(prim_op, vat, count, vsize):
    return bytes([prim_op | vat]) + struct.pack(">H", count) + b"\xAB" * (count * vsize)


# Position present and DIRECT (VCD lo bits 9-10 = 1), 3 components (VAT A bit
# 0) of f32 (VAT A bits 1-3 = 4). Nothing else present, so one vertex is
# exactly 3 * 4 = 12 bytes.
VCD_LO_POS_DIRECT = 1 << 9
VAT_A_POS_3xF32 = 1 | (4 << 1)
CONFIGURE_POS_ONLY = cp(0x50, VCD_LO_POS_DIRECT) + cp(0x60, 0) + cp(0x70, VAT_A_POS_3xF32)


def test_vertex_size():
    print("vertex size")
    v = VertexFormats()
    v.set_reg(0x50, VCD_LO_POS_DIRECT)
    v.set_reg(0x60, 0)
    v.set_reg(0x70, VAT_A_POS_3xF32)
    check(v.vertex_size(0) == 12, "position-only f32x3 vertex is 12 bytes")
    check(v.describe(0) == "pos", "format describes as position-only")


def test_basic_stream():
    print("basic stream")
    st = state()
    s = nop(2) + CONFIGURE_POS_ONLY + bp(0x40, 0x1234) + xf(0x1000, [1, 2]) + \
        draw(0x90, 0, 3, 12)
    st["total_bytes"] = len(s)
    decode(s, st)
    check(not st["desync"], "did not desynchronise")
    check(not st["truncated"], "not reported as truncated")
    check(st["cp"] and sum(st["cp"].values()) == 3, "three CP writes seen")
    check(sum(st["bp"].values()) == 1, "one BP write seen")
    check(sum(st["xf"].values()) == 1, "one XF write seen")
    check(st["prims"].get("tri-strip") == 1, "one tri-strip draw seen")
    check(st["verts"] == 3, "three vertices counted")
    check(not st["unknown_ops"], "no undecoded opcodes")


def test_call_dl_is_nine_bytes():
    """The regression that produced a 34,583-vertex draw.

    GX_CMD_CALL_DL carries a u32 address and a u32 size
    (OpcodeDecoding.cpp:202-212). Skipping it as a one-byte unknown walks into
    the address field and reads it as opcodes. The damage is silent: the
    garbage still parses, so no desync flag fires and coverage stays at 100%.

    The address here is chosen so that mis-skipping lands on 0x90, a tri-strip
    opcode, followed by a large vertex count -- reproducing the original
    failure exactly rather than merely detecting a byte-count change.
    """
    print("CALL_DL")
    st = state()
    s = CONFIGURE_POS_ONLY + call_dl(0x90008700, 0x40) + nop(1)
    st["total_bytes"] = len(s)
    decode(s, st)
    check(st["display_lists"] == 1, "display list counted")
    check(st["display_list_bytes"] == 0x40, "display list size recorded")
    check(st["verts"] == 0, "no phantom vertices invented from the address")
    check(not st["prims"], "no phantom primitive invented from the address")
    check(not st["unknown_ops"], "CALL_DL is not reported as undecoded")


def test_one_byte_opcodes():
    print("one-byte opcodes")
    st = state()
    s = CONFIGURE_POS_ONLY + b"\x44" + b"\x48" + draw(0x88, 0, 2, 12)
    st["total_bytes"] = len(s)
    decode(s, st)
    check(not st["unknown_ops"], "0x44 and 0x48 are known, not undecoded")
    check(st["prims"].get("triangles") == 1, "stream stayed in sync after them")
    check(st["verts"] == 2, "vertex count correct after them")


def test_truncation_detected():
    """A capture cut mid-draw must not report itself as complete.

    The walker stops at the draw boundary, so nothing else complains and
    coverage reads 100% -- which is why this needs its own flag.
    """
    print("truncation")
    st = state()
    full = CONFIGURE_POS_ONLY + draw(0x90, 0, 10, 12)
    s = full[:-60]                      # cut 5 vertices off the payload
    st["total_bytes"] = len(s)
    decode(s, st)
    check(st["truncated"], "truncated capture is flagged")
    check(not st["desync"], "truncation is not reported as desync")


def test_desync_on_unconfigured_format():
    print("desync")
    st = state()
    s = draw(0x90, 3, 4, 0)             # format 3 never configured
    st["total_bytes"] = len(s)
    decode(s, st)
    check(st["desync"], "draw against an unconfigured format desyncs")


def main():
    for t in (test_vertex_size, test_basic_stream, test_call_dl_is_nine_bytes,
              test_one_byte_opcodes, test_truncation_detected,
              test_desync_on_unconfigured_format):
        t()
    print()
    if FAILURES:
        print(f"FAILED ({len(FAILURES)} failures)")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
