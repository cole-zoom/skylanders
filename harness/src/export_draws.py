#!/usr/bin/env python3
"""Decode a captured frame's draws into float positions for the WebGPU replayer.

GX stores positions quantised: an s8 or s16 integer plus a per-format
fractional shift from VAT A. The browser wants plain floats, so the
dequantisation happens here rather than in WGSL.

Usage:  python3 export_draws.py <capture.dff> <out.json>
"""
import json
import struct
import sys

from fifo_inventory import read_header, read_frames, read_initial_state, decode, record_bp
from collections import Counter
from gx_vertex import VertexFormats, COMP_SIZE, COLOR_SIZE
from gx_arrays import GuestRAM, ArrayDescriptors, resolve_positions

# VAT A position format -> struct code. 0=u8 1=s8 2=u16 3=s16 4=f32
POS_STRUCT = {0: "B", 1: "b", 2: "H", 3: "h", 4: "f"}


def decode_positions(draw, vfmt=None):
    """Return [(x, y, z), ...] in GX object space.

    Uses the VAT fields captured AT DRAW TIME. Reading vfmt after the decode
    finishes gives the final VAT state, which is wrong for every draw that
    preceded the last VAT change -- and in this capture that is most of them.
    """
    cnt = draw["pos_cnt"]
    fmt = draw["pos_fmt"]
    frac = draw["pos_frac"]
    raw = bytes.fromhex(draw["data"])
    code = POS_STRUCT.get(fmt, "f")
    scale = 1.0 if fmt == 4 else float(1 << frac)

    out = []
    for v in range(draw["count"]):
        off = v * draw["vsize"]
        try:
            comps = struct.unpack_from(f">{cnt}{code}", raw, off)
        except struct.error:
            break
        xyz = [c / scale for c in comps]
        while len(xyz) < 3:
            xyz.append(0.0)
        out.append(tuple(xyz))
    return out


def main(src, dst):
    b = open(src, "rb").read()
    h = read_header(b)
    st = dict(bp=Counter(), xf=Counter(), prims=Counter(), unknown_ops=Counter(),
              verts=0, tev_stages=set(), texgens=set(), cull=set(),
              blend_enable=set(), logicop_enable=set(), dither=set(),
              src_factor=set(), dst_factor=set(), subtract=set(),
              logic_ops=set(), dst_alpha_used=set(), ztest=set(), zwrite=set(),
              alpha_test=0, efb_copies=0, tev_color_writes=0,
              tex_formats=set(), tex_filters=set(), line_point_width=0,
              stopped_at_draw=False, bp_initial={}, decoded_bytes=0,
              total_bytes=0, vfmt=VertexFormats(), cp=Counter(),
              vtx_formats=Counter(), desync=False, cur_blend=0, export=[],
              arrays=ArrayDescriptors(), ram=GuestRAM())
    read_initial_state(b, h, st)
    for f in read_frames(b, h):
        for u in f.get("updates", []):
            st["ram"].apply(u["address"], u["data"])
        decode(f["data"], st)
    print(f"RAM chunks from memory updates: {len(st['ram'].chunks)}")

    draws = []
    allx, ally = [], []
    misses = 0
    for d in st["export"]:
        pts = resolve_positions(d, st["ram"], st["arrays"])
        if not pts:
            misses += 1
            continue
        draws.append(dict(prim=d["prim"], fmt=d["fmt"], blend=d["blend"],
                          pos=[c for p in pts for c in p]))
        allx += [p[0] for p in pts]
        ally += [p[1] for p in pts]

    if misses:
        print(f"  {misses}/{len(st['export'])} draws unresolved "
              f"(vertex arrays not in the captured memory updates)")
    meta = dict(
        draw_count=len(draws),
        total_verts=sum(len(d["pos"]) // 3 for d in draws),
        bounds=dict(xmin=min(allx), xmax=max(allx),
                    ymin=min(ally), ymax=max(ally)) if allx else None,
        draws=draws,
    )
    json.dump(meta, open(dst, "w"))
    print(f"exported {meta['draw_count']} draws, {meta['total_verts']} verts -> {dst}")
    if meta["bounds"]:
        bnd = meta["bounds"]
        print(f"  x {bnd['xmin']:.3f}..{bnd['xmax']:.3f}   "
              f"y {bnd['ymin']:.3f}..{bnd['ymax']:.3f}")
    print(f"  primitives: {dict(Counter(d['prim'] for d in draws))}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
