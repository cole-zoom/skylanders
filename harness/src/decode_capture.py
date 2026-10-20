#!/usr/bin/env python3
"""Decode a live GX command stream captured from the emulator.

fifo_inventory.py reads a .dff written by Dolphin's FIFO recorder. This reads
the stream captured directly out of Core/HW/GPFifo.cpp's gather-pipe flush --
the same bytes, but taken live from a running boot on either target, so the
native and browser builds can be compared on what they actually send to the GPU
rather than on CPU state alone.

Input is the harness's own output: lines beginning "F " carrying base64. A
browser has no filesystem to write a capture to, so the stream comes back
through stdout like everything else.

Usage:
    decode_capture.py <run-output.txt> [...]

With two or more files it also reports whether the captures are byte-identical,
which is the point of capturing from both targets.
"""
import base64
import sys
from collections import Counter

from fifo_inventory import decode, assess_capture
from gx_vertex import VertexFormats
from gx_arrays import GuestRAM, ArrayDescriptors


def read_capture(path):
    """Pull the base64 payload out of a harness run's stdout."""
    out = []
    for line in open(path, errors="replace"):
        if line.startswith("F "):
            out.append(line[2:].strip())
    if not out:
        return b""
    return base64.b64decode("".join(out))


def fresh_state():
    # Same shape fifo_inventory.main builds; decode() mutates it in place.
    return dict(bp=Counter(), xf=Counter(), prims=Counter(), unknown_ops=Counter(),
                verts=0, tev_stages=set(), texgens=set(), cull=set(),
                blend_enable=set(), logicop_enable=set(), dither=set(),
                src_factor=set(), dst_factor=set(), subtract=set(),
                logic_ops=set(), dst_alpha_used=set(), ztest=set(), zwrite=set(),
                alpha_test=0, efb_copies=0, tev_color_writes=0,
                tex_formats=set(), tex_filters=set(), line_point_width=0,
                stopped_at_draw=False, bp_initial={}, decoded_bytes=0,
                total_bytes=0, vfmt=VertexFormats(), cp=Counter(),
                vtx_formats=Counter(), desync=False, cur_blend=0,
                arrays=ArrayDescriptors(), ram=GuestRAM(), export=None)


def report(path, stream):
    st = fresh_state()
    st["total_bytes"] = len(stream)
    decode(stream, st)
    if not st["desync"]:
        st["decoded_bytes"] = len(stream)

    print(f"{path}: {len(stream)} bytes of GX command stream")
    print(f"  CP register writes      {sum(st['cp'].values())} "
          f"across {len(st['cp'])} distinct registers")
    print(f"  BP register writes      {sum(st['bp'].values())} "
          f"across {len(st['bp'])} distinct registers")
    print(f"  XF register writes      {sum(st['xf'].values())} "
          f"across {len(st['xf'])} distinct registers")
    print(f"  primitives              {dict(st['prims']) or 'none'}")
    print(f"  vertices drawn          {st['verts']}")
    print(f"  vertex formats          {dict(st['vtx_formats']) or 'none'}")
    print(f"  TEV stages in use       {sorted(st['tev_stages']) or 'none'}")
    print(f"  texture formats         {sorted(st['tex_formats']) or 'none'}")
    print(f"  EFB copies              {st['efb_copies']}")
    if st["unknown_ops"]:
        print(f"  UNDECODED opcodes       "
              f"{ {hex(k): v for k, v in st['unknown_ops'].most_common(5)} }")
    pct = 100.0 * st["decoded_bytes"] / max(st["total_bytes"], 1)
    print(f"  decode coverage         {st['decoded_bytes']}/{st['total_bytes']}"
          f" bytes ({pct:.1f}%)")
    if st["desync"]:
        print("  WARNING: decoder desynchronised -- figures are a LOWER BOUND.")
    ok, msg = assess_capture(st)
    print(f"  {'OK  ' if ok else 'note'}  {msg}")
    return st


def main(paths):
    streams = []
    for p in paths:
        s = read_capture(p)
        if not s:
            print(f"{p}: no 'F ' capture lines found")
            return 1
        streams.append(s)
        report(p, s)
        print()

    if len(streams) > 1:
        print("── capture differential ──")
        base = streams[0]
        for p, s in zip(paths[1:], streams[1:]):
            if s == base:
                print(f"  MATCH -- {paths[0]} and {p} are byte-identical "
                      f"({len(s)} bytes)")
            else:
                # Report the first differing byte; a command stream that
                # diverges partway is far more informative than a length diff.
                n = min(len(base), len(s))
                at = next((i for i in range(n) if base[i] != s[i]), n)
                print(f"  DIVERGED -- {paths[0]} vs {p}")
                print(f"    first difference at byte {at} "
                      f"(lengths {len(base)} and {len(s)})")
                lo = max(0, at - 8)
                print(f"    {paths[0]}: {base[lo:at+8].hex()}")
                print(f"    {p}: {s[lo:at+8].hex()}")
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["/tmp/cap-native.txt"]))
