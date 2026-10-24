#!/usr/bin/env python3
"""Compare two frames dumped by bootgame --dump-xfb (docs/PHASE3-WEBGPU.md M3).

Each input is a bootgame log: the frame is the PPM it emitted as base64 on
"F " lines. The reference is the Software renderer's frame, the subject the
WebGPU backend's, both from the same FIFO capture.

Prints the difference and, with --png, writes reference | subject | the
difference (amplified x4) side by side.

    compare_frames.py <reference.log> <subject.log> [--png out.png] [--rows N]

--rows limits the comparison to the first N rows: the XFB is read as 640x480
but this game's frame is 456 lines, and what lies below is not drawn by
either backend.

What to expect, and why the verdict looks at 8x8 block averages: the
Software renderer emulates the EFB's 6-bit colour with dithering, and
Dolphin's hardware backends -- this one included -- default to "Force
24-bit color", which skips both. Per pixel that is noise of a few levels
everywhere; averaged over a block it cancels, leaving a level or two of
bias. Edges differ by a pixel between any rasteriser and the Software one;
the edge-tolerant figures forgive exactly that. A wrong texture, colour or
blend survives both and fails the verdict.

Exit status: 0 when the frame passes, 1 when it does not.
"""

import argparse
import base64
import math
import struct
import sys
import zlib


def read_frame(path):
    data = "".join(line[2:].strip() for line in open(path, errors="replace") if line.startswith("F "))
    ppm = base64.b64decode(data)
    # P6\n<w> <h>\n255\n<rgb>
    parts = ppm.split(b"\n", 3)
    if parts[0] != b"P6":
        sys.exit(f"{path}: no PPM in the log")
    w, h = map(int, parts[1].split())
    return w, h, parts[3][: w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("subject")
    ap.add_argument("--png")
    ap.add_argument("--rows", type=int, default=456)
    a = ap.parse_args()

    w, h, ref = read_frame(a.reference)
    w2, h2, sub = read_frame(a.subject)
    if (w, h) != (w2, h2):
        sys.exit(f"sizes differ: {w}x{h} against {w2}x{h2}")
    rows = min(a.rows, h)
    n = w * rows

    total = 0
    worst = 0
    over8 = over32 = 0
    sq = 0
    diff = bytearray(w * rows * 3)
    for i in range(n):
        p = i * 3
        d = max(abs(ref[p + c] - sub[p + c]) for c in range(3))
        for c in range(3):
            e = ref[p + c] - sub[p + c]
            sq += e * e
            diff[p + c] = min(255, abs(e) * 4)
        total += d
        worst = max(worst, d)
        over8 += d > 8
        over32 += d > 32
    # Edge-tolerant: a pixel differs only if no subject pixel in its 3x3
    # neighbourhood is within the threshold. A software rasteriser and a GPU
    # disagree about which pixels an edge covers and how a texture filters
    # across it, by a pixel; that is what this forgives, and nothing else.
    def near(i):
        x, y = i % w, i // w
        best = 255
        for dy in (-1, 0, 1):
            yy = y + dy
            if yy < 0 or yy >= rows:
                continue
            for dx in (-1, 0, 1):
                xx = x + dx
                if xx < 0 or xx >= w:
                    continue
                q = (yy * w + xx) * 3
                best = min(best, max(abs(ref[i * 3 + c] - sub[q + c]) for c in range(3)))
                if best == 0:
                    return 0
        return best

    tol16 = tol32 = 0
    for i in range(n):
        p = i * 3
        if max(abs(ref[p + c] - sub[p + c]) for c in range(3)) <= 16:
            continue
        d = near(i)
        tol16 += d > 16
        tol32 += d > 32
    # 8x8 block averages, the per-channel worst per block.
    B = 8
    blocks = []
    for by in range(0, rows - B + 1, B):
        for bx in range(0, w, B):
            worst_block = 0.0
            for c in range(3):
                r = s_ = 0
                for y in range(B):
                    base = ((by + y) * w + bx) * 3 + c
                    for x in range(B):
                        r += ref[base + x * 3]
                        s_ += sub[base + x * 3]
                worst_block = max(worst_block, abs(r - s_) / (B * B))
            blocks.append(worst_block)
    blocks.sort()
    nb = len(blocks)
    block_p99 = blocks[int(nb * 0.99)]
    block_over8 = 100 * sum(b > 8 for b in blocks) / nb
    mse = sq / (n * 3)
    psnr = float("inf") if mse == 0 else 10 * math.log10(255 * 255 / mse)
    print(f"  frame     {w}x{rows}")
    print(f"  mean      {total / n:.2f} (largest channel difference per pixel, averaged)")
    print(f"  max       {worst}")
    print(f"  > 8       {100 * over8 / n:.2f}% of pixels")
    print(f"  > 32      {100 * over32 / n:.2f}% of pixels")
    print(f"  psnr      {psnr:.1f} dB")
    print(f"  edge-tolerant > 16   {100 * tol16 / n:.3f}% of pixels")
    print(f"  edge-tolerant > 32   {100 * tol32 / n:.3f}% of pixels")
    print(f"  8x8 blocks: median {blocks[nb // 2]:.2f}  p99 {block_p99:.2f}  "
          f"max {blocks[-1]:.1f}  > 8 in {block_over8:.2f}%")
    # Set above the worst scene measured when the suite was written (the
    # tutorial level: p99 10.3, 3.2% of blocks, 0.28% edge-tolerant), so
    # that what passes today keeps passing and a real error does not.
    ok = block_p99 <= 12 and block_over8 <= 5 and 100 * tol32 / n <= 0.5
    print(f"  verdict   {'PASS' if ok else 'FAIL'}")

    if a.png:
        out = bytearray()
        for y in range(rows):
            for img in (ref, sub, diff):
                out += img[y * w * 3 : (y + 1) * w * 3]
        write_png(a.png, w * 3, rows, bytes(out))
        print(f"  png       {a.png}")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
