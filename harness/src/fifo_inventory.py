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

from gx_vertex import VertexFormats
from gx_arrays import GuestRAM, ArrayDescriptors

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
                cp_off=cp_off, cp_sz=cp_sz,
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
    cp = struct.unpack_from(f"<{h['cp_sz']}I", b, h["cp_off"])
    st["vfmt"].load_initial(cp)
    st["arrays"].load_initial(cp)
    return bp


def read_frames(b, h):
    """Frame records are 64 bytes (FileFrameInfo), fields packed with no
    padding: u64 dataOffset, u32 size/start/end, u64 updatesOffset,
    u32 updateCount, then a 32-byte reserved tail. The reserved tail is what
    makes the stride 64 while the fields occupy only the first 32."""
    frames = []
    for i in range(h["frame_count"]):
        base = h["frames_off"] + i * 64
        (data_off, data_sz, start, end, mu_off, mu_n) = struct.unpack_from(
            "<QIIIQI", b, base)
        updates = []
        for k in range(mu_n):
            # FileMemoryUpdate: 24 bytes, also packed
            (fifo_pos, addr, d_off, d_sz, typ) = struct.unpack_from(
                "<IIQIB", b, mu_off + k * 24)
            updates.append(dict(address=addr, data=b[d_off:d_off + d_sz],
                                type=typ))
        frames.append(dict(data=b[data_off:data_off + data_sz],
                           n_updates=mu_n, updates=updates))
    return frames


def decode(stream, st):
    """Walk the big-endian GX command stream, tallying what it uses."""
    i, n = 0, len(stream)
    while i < n:
        op = stream[i]
        if op == 0x00:                               # NOP
            i += 1
        elif op == 0x08:                             # load CP reg
            if i + 6 > n:
                break
            addr = stream[i + 1]
            val = struct.unpack_from(">I", stream, i + 2)[0]
            st["vfmt"].set_reg(addr, val)
            st["arrays"].set_reg(addr, val)
            st["cp"][addr] += 1
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
        elif op == 0x40:                             # GX_CMD_CALL_DL
            # 9 bytes: opcode, u32 address, u32 size
            # (VideoCommon/OpcodeDecoding.cpp:202-212).
            #
            # Skipping this as a 1-byte unknown walks straight into the address
            # field and reads it as opcodes, which desynchronises the rest of
            # the stream silently -- no desync flag fires, because the garbage
            # still parses. That is what produced a single "tri-strip" of
            # 34,583 vertices in a live capture, more than the entire title
            # screen's 3,667.
            #
            # The display list's COMMANDS ARE NOT IN THIS STREAM. They live in
            # guest memory at `address`, so a capture containing CALL_DL is
            # missing however much geometry the list holds. Counted so that
            # absence is visible rather than assumed away.
            if i + 9 > n:
                break
            dl_addr = struct.unpack_from(">I", stream, i + 1)[0]
            dl_size = struct.unpack_from(">I", stream, i + 5)[0]
            st["display_lists"] += 1
            st["display_list_bytes"] += dl_size & ~31
            _ = dl_addr
            i += 9
        elif op in (0x44, 0x48):                     # UNKNOWN_METRICS, INVL_VC
            # Genuinely one byte each, per OpcodeDecoding.cpp:205-218. Listing
            # them explicitly keeps them out of the "undecoded" bucket, which
            # should mean "we do not know", not "we know and it is fine".
            i += 1
        elif op == 0x61:                             # load BP reg
            if i + 5 > n:
                break
            word = struct.unpack_from(">I", stream, i + 1)[0]
            reg, val = word >> 24, word & 0xFFFFFF
            st["bp"][reg] += 1
            record_bp(reg, val, st)
            i += 5
        elif op & 0xF8 in PRIMITIVES:                # draw
            if i + 3 > n:
                break
            vat = op & 0x07
            vcount = struct.unpack_from(">H", stream, i + 1)[0]
            vsize = st["vfmt"].vertex_size(vat)
            st["prims"][PRIMITIVES[op & 0xF8]] += 1
            st["verts"] += vcount
            st["vtx_formats"][st["vfmt"].describe(vat)] += 1
            if st["export"] is not None and vsize and vcount:
                payload = stream[i + 3: i + 3 + vcount * vsize]
                st["export"].append(dict(
                    prim=PRIMITIVES[op & 0xF8], vat=vat, count=vcount,
                    vsize=vsize, fmt=st["vfmt"].describe(vat),
                    pos_fmt=(st["vfmt"].vat_a[vat] >> 1) & 7,
                    pos_cnt=3 if (st["vfmt"].vat_a[vat] & 1) else 2,
                    pos_frac=(st["vfmt"].vat_a[vat] >> 4) & 0x1F,
                    pos_presence=st["vfmt"]._pos(),
                    pos_offset=st["vfmt"].pos_offset(),
                    blend=st["cur_blend"], data=payload.hex()))
            if vsize == 0 and vcount:
                # A draw against an unconfigured format means our CP state is
                # wrong. Resynchronising would walk onto vertex data and invent
                # features; stop and say so instead.
                st["desync"] = True
                return
            end = i + 3 + vcount * vsize
            if end > n:
                # The draw's payload runs past the end of what we hold. The
                # walk then simply stops, and because it stopped at a draw
                # boundary rather than mid-opcode nothing else complains --
                # so coverage reads 100% for a stream that was cut in half.
                #
                # That is actively misleading: coverage measures whether the
                # walker consumed the bytes, not whether the capture contains
                # a complete command stream. A live capture is cut at whatever
                # byte the limit falls on, so this is the normal case for one,
                # and the vertex count of the final draw describes data that
                # was never captured.
                st["truncated"] = True
            i = end
        else:
            st["unknown_ops"][op] += 1
            i += 1


def record_bp(reg, val, st):
    if reg == BP_GENMODE:
        st["tev_stages"].add(((val >> 10) & 0xF) + 1)
        st["texgens"].add(val & 0xF)
        st["cull"].add((val >> 14) & 0x3)
    elif reg == BP_BLENDMODE:
        st["cur_blend"] = val
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
              stopped_at_draw=False, truncated=False, display_lists=0,
              display_list_bytes=0, bp_initial={}, decoded_bytes=0,
              total_bytes=0, vfmt=VertexFormats(), cp=Counter(),
              vtx_formats=Counter(), desync=False, cur_blend=0,
              arrays=ArrayDescriptors(), ram=GuestRAM(),
              export=[] if _export_path() else None)

    print(f"{path}")
    print(f"  format version {h['version']}, {h['frame_count']} frames, "
          f"{len(b)/1024/1024:.1f} MB")
    read_initial_state(b, h, st)
    print(f"  initial BP snapshot: {len(st['bp_initial'])} registers set")
    for i, f in enumerate(frames):
        print(f"  frame {i}: {len(f['data'])} bytes of commands, "
              f"{f['n_updates']} memory updates")
        for u in f.get("updates", []):
            st["ram"].apply(u["address"], u["data"])
        st["total_bytes"] += len(f["data"])
        decode(f["data"], st)
        if not st["desync"]:
            st["decoded_bytes"] += len(f["data"])

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
    print(f"  display lists called    {st['display_lists']}"
          f" ({st['display_list_bytes']} bytes of commands NOT in this stream)")
    if st["unknown_ops"]:
        print(f"  UNDECODED opcodes       "
              f"{ {hex(k): v for k, v in st['unknown_ops'].most_common(5)} }")
    print(f"  vertex formats          {dict(st['vtx_formats']) or 'n/a'}")
    print(f"  vertices drawn          {st['verts']}")
    pct = 100.0 * st["decoded_bytes"] / max(st["total_bytes"], 1)
    print(f"\n  stream decode coverage  {st['decoded_bytes']}/{st['total_bytes']}"
          f" bytes ({pct:.1f}%)")
    if st["truncated"]:
        print("  WARNING: the final draw runs past the end of the stream --")
        print("  the capture is TRUNCATED. Its vertex count describes data")
        print("  that is not here, and coverage above is not a validity check.")
    if st["desync"]:
        print("  WARNING: decoder desynchronised (draw against an unconfigured")
        print("  vertex format). Figures above are a LOWER BOUND.")
    elif pct > 99.0:
        print("  Full frame decoded -- the feature list above is complete for")
        print("  this capture.")
    ok, msg = assess_capture(st)
    print(f"\n  {'OK  ' if ok else 'WARN'}  {msg}")
    if st["export"] is not None:
        write_export(st, _export_path())
    if "--check" in sys.argv and not ok:
        sys.exit(1)


# A capture that lands on a loading screen or a fade still writes a plausible
# ~2 MB file with no error, because most of that size is the 1 MB TexMem
# snapshot. The inventory drawn from it then reports almost no GX usage --
# quietly wrong rather than obviously broken, which is the expensive kind.
# These floors are well below the two known-good captures (title: 57,628 B /
# 238 draws; gameplay: 114,436 B / 207 draws) and well above a loading screen.
MIN_COMMAND_BYTES = 20_000
MIN_DRAWS = 50


def _args():
    """Positional args only, so flags are never mistaken for paths."""
    return [a for a in sys.argv[1:] if not a.startswith("-")]


def _export_path():
    a = _args()
    return a[1] if len(a) > 1 else None


def assess_capture(st):
    """Return (ok, message). Cheap guard against a silently empty capture."""
    draws = sum(st["prims"].values())
    byts = st["total_bytes"]
    problems = []
    if byts < MIN_COMMAND_BYTES:
        problems.append(f"only {byts} bytes of commands (want >= {MIN_COMMAND_BYTES})")
    if draws < MIN_DRAWS:
        problems.append(f"only {draws} draws (want >= {MIN_DRAWS})")
    if st["desync"]:
        problems.append("decoder desynchronised")
    if problems:
        return False, ("capture looks EMPTY -- likely a loading screen, a fade, "
                       "or a mistimed delay:\n    " + "\n    ".join(problems))
    return True, f"capture looks substantive: {byts} bytes, {draws} draws"


def write_export(st, path):
    import json
    draws = st["export"]
    json.dump(dict(draw_count=len(draws),
                   total_verts=sum(d["count"] for d in draws),
                   draws=draws), open(path, "w"))
    print(f"\n  exported {len(draws)} draws -> {path}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "harness/traces/ssa-boot.dff")
