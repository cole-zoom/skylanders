#!/usr/bin/env python3
"""Inventory the GX features a captured FIFO log actually uses.

Spike B asks whether WebGPU can render this game. The general answer ("GX has
16 TEV stages and WebGPU has none") is useless, because no game uses all of GX.
This decodes a real capture and reports what *Skylanders* touches, so the
WebGPU backend can be scoped to that rather than to the whole console.

Reads the .dff written by dolphin-emu-nogui --fifo-record. Format from
Source/Core/Core/FifoPlayer/FifoDataFile.cpp (FILE_ID 0x0d01f1f0, version 6).
All multi-byte fields are little-endian; the FIFO command stream itself is
big-endian, because it is Broadway's.
"""
import struct
import sys
from collections import Counter

FILE_ID = 0x0D01F1F0

# BP registers we care about, from VideoCommon/BPMemory.h
BP_GENMODE, BP_LINEPTWIDTH = 0x00, 0x22
BP_ZMODE, BP_BLENDMODE, BP_CONSTANTALPHA = 0x40, 0x41, 0x42
BP_TRIGGER_EFB_COPY = 0x52
BP_TX_SETMODE0, BP_TX_SETIMAGE0 = 0x80, 0x88
BP_TEV_COLOR_ENV, BP_TEV_ALPHA_ENV = 0xC0, 0xC1
BP_ALPHACOMPARE, BP_TEV_KSEL = 0xF3, 0xF6

PRIMITIVES = {
    0x80: "quads", 0x88: "triangles", 0x90: "tri-strip",
    0x98: "tri-fan", 0xA0: "lines", 0xA8: "line-strip", 0xB0: "points",
}
BLEND_FACTORS = ["zero", "one", "src_color", "inv_src_color",
                 "src_alpha", "inv_src_alpha", "dst_alpha", "inv_dst_alpha"]
LOGIC_OPS = ["clear", "and", "and_reverse", "copy", "and_inverted", "noop",
             "xor", "or", "nor", "equiv", "invert", "or_reverse",
             "copy_inverted", "or_inverted", "nand", "set"]


def read_header(b):
    (file_id, ver, minver) = struct.unpack_from("<III", b, 0)
    if file_id != FILE_ID:
        sys.exit(f"not a Dolphin FIFO log (id {file_id:#x})")
    off = 12
    (bp_off, bp_sz, cp_off, cp_sz, xf_off, xf_sz, xfr_off, xfr_sz,
     frames_off, frame_count, flags) = struct.unpack_from("<QIQIQIQIQII", b, off)
    return dict(version=ver, bp_off=bp_off, bp_sz=bp_sz,
                frames_off=frames_off, frame_count=frame_count, flags=flags)


def read_initial_state(b, h, st):
    """The header carries full BP/CP/XF snapshots: the entire configured GX
    state when recording started. Decoding the stream only shows what CHANGED;
    this shows what was already set."""
    bp = struct.unpack_from(f"<{h['bp_sz']}I", b, h["bp_off"])
    for reg, val in enumerate(bp):
        if val:
            st["bp_initial"][reg] = val & 0xFFFFFF
            record_bp(reg, val & 0xFFFFFF, st)
    return bp


def read_frames(b, h):
    """Frame records are 40 bytes: fifo offset/size/start/end, updates off/count."""
    frames = []
    for i in range(h["frame_count"]):
        base = h["frames_off"] + i * 40
        (data_off, data_sz, start, end, mu_off, mu_n) = struct.unpack_from(
            "<QIIIQI", b, base)
        frames.append(dict(data=b[data_off:data_off + data_sz],
                           n_updates=mu_n))
    return frames


def decode(stream, st):
    """Walk the big-endian GX command stream, tallying what it uses."""
    i, n = 0, len(stream)
    while i < n:
        op = stream[i]
        if op == 0x00:                               # NOP
            i += 1
        elif op == 0x08:                             # load CP reg
            i += 6
        elif op == 0x10:                             # load XF reg
            if i + 5 > n:
                break
            word = struct.unpack_from(">I", stream, i + 1)[0]
            cnt = ((word >> 16) & 0xF) + 1
            addr = word & 0xFFFF
            st["xf"][addr] += 1
            i += 5 + cnt * 4
        elif op in (0x20, 0x28, 0x30, 0x38):         # index-mode loads
            i += 5
        elif op == 0x61:                             # load BP reg
            if i + 5 > n:
                break
            word = struct.unpack_from(">I", stream, i + 1)[0]
            reg, val = word >> 24, word & 0xFFFFFF
            st["bp"][reg] += 1
            record_bp(reg, val, st)
            i += 5
        elif op & 0xF8 in PRIMITIVES:                # draw
            st["decoded_bytes"] += i
            if i + 3 > n:
                break
            vcount = struct.unpack_from(">H", stream, i + 1)[0]
            st["prims"][PRIMITIVES[op & 0xF8]] += 1
            st["verts"] += vcount
            # Vertex payload length depends on the active vertex descriptor,
            # which we do not track; stop decoding this frame rather than
            # silently resynchronise onto garbage.
            st["stopped_at_draw"] = True
            return
        else:
            st["unknown_ops"][op] += 1
            i += 1


def record_bp(reg, val, st):
    if reg == BP_GENMODE:
        st["tev_stages"].add(((val >> 10) & 0xF) + 1)
        st["texgens"].add(val & 0xF)
        st["cull"].add((val >> 14) & 0x3)
    elif reg == BP_BLENDMODE:
        st["blend_enable"].add(val & 1)
        st["logicop_enable"].add((val >> 1) & 1)
        st["dither"].add((val >> 2) & 1)
        st["dst_factor"].add(BLEND_FACTORS[(val >> 5) & 7])
        st["src_factor"].add(BLEND_FACTORS[(val >> 8) & 7])
        st["subtract"].add((val >> 11) & 1)
        if (val >> 1) & 1:
            st["logic_ops"].add(LOGIC_OPS[(val >> 12) & 0xF])
    elif reg == BP_CONSTANTALPHA:
        st["dst_alpha_used"].add(bool((val >> 8) & 1))
    elif reg == BP_ZMODE:
        st["ztest"].add(val & 1)
        st["zwrite"].add((val >> 4) & 1)
    elif reg == BP_ALPHACOMPARE:
        st["alpha_test"] += 1
    elif reg == BP_TRIGGER_EFB_COPY:
        st["efb_copies"] += 1
    elif BP_TEV_COLOR_ENV <= reg <= BP_TEV_COLOR_ENV + 31:
        st["tev_color_writes"] += 1
    elif BP_TX_SETIMAGE0 <= reg < BP_TX_SETIMAGE0 + 4:
        st["tex_formats"].add((val >> 20) & 0xF)
    elif BP_TX_SETMODE0 <= reg < BP_TX_SETMODE0 + 4:
        st["tex_filters"].add((val >> 4) & 0x7)
    elif reg == BP_LINEPTWIDTH:
        st["line_point_width"] += 1


def main(path):
    b = open(path, "rb").read()
    h = read_header(b)
    frames = read_frames(b, h)

    st = dict(bp=Counter(), xf=Counter(), prims=Counter(), unknown_ops=Counter(),
              verts=0, tev_stages=set(), texgens=set(), cull=set(),
              blend_enable=set(), logicop_enable=set(), dither=set(),
              src_factor=set(), dst_factor=set(), subtract=set(),
              logic_ops=set(), dst_alpha_used=set(), ztest=set(), zwrite=set(),
              alpha_test=0, efb_copies=0, tev_color_writes=0,
              tex_formats=set(), tex_filters=set(), line_point_width=0,
              stopped_at_draw=False, bp_initial={}, decoded_bytes=0,
              total_bytes=0)

    print(f"{path}")
    print(f"  format version {h['version']}, {h['frame_count']} frames, "
          f"{len(b)/1024/1024:.1f} MB")
    read_initial_state(b, h, st)
    print(f"  initial BP snapshot: {len(st['bp_initial'])} registers set")
    for i, f in enumerate(frames):
        print(f"  frame {i}: {len(f['data'])} bytes of commands, "
              f"{f['n_updates']} memory updates")
        st["total_bytes"] += len(f["data"])
        decode(f["data"], st)

    print("\n── GX features used ──")
    print(f"  BP register writes      {sum(st['bp'].values())} "
          f"across {len(st['bp'])} distinct registers")
    print(f"  XF register writes      {sum(st['xf'].values())} "
          f"across {len(st['xf'])} distinct registers")
    print(f"  TEV stages in use       {sorted(st['tev_stages']) or 'n/a'}")
    print(f"  texgens                 {sorted(st['texgens']) or 'n/a'}")
    print(f"  TEV combiner writes     {st['tev_color_writes']}")
    print(f"  blend enabled           {sorted(st['blend_enable']) or 'n/a'}")
    print(f"  blend src factors       {sorted(st['src_factor']) or 'n/a'}")
    print(f"  blend dst factors       {sorted(st['dst_factor']) or 'n/a'}")
    print(f"  subtract blend          {sorted(st['subtract']) or 'n/a'}")
    print(f"  LOGIC OPS enabled       {sorted(st['logicop_enable']) or 'n/a'}"
          f"  ops={sorted(st['logic_ops']) or 'none'}")
    print(f"  dst-alpha pass          {sorted(st['dst_alpha_used']) or 'n/a'}")
    print(f"  alpha-test writes       {st['alpha_test']}")
    print(f"  EFB copies              {st['efb_copies']}")
    print(f"  texture formats         {sorted(st['tex_formats']) or 'n/a'}")
    print(f"  primitives              {dict(st['prims']) or 'n/a'}")
    print(f"  line/point width writes {st['line_point_width']}")
    if st["unknown_ops"]:
        print(f"  UNDECODED opcodes       "
              f"{ {hex(k): v for k, v in st['unknown_ops'].most_common(5)} }")
    if st["stopped_at_draw"]:
        pct = 100.0 * st["decoded_bytes"] / max(st["total_bytes"], 1)
        print(f"\n  Stream decode covered {st['decoded_bytes']}/{st['total_bytes']}"
              f" bytes ({pct:.1f}%) before the first draw.")
        print("  NOTE: decoding stops at the first draw in each frame. Vertex")
        print("  payload length depends on the active vertex descriptor, which")
        print("  this tool does not track. Register state above is therefore")
        print("  setup-only and is a LOWER BOUND on features used.")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "harness/traces/ssa-boot.dff")
