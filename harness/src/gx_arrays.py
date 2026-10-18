"""Resolve GX indexed vertex attributes.

Skylanders does not put vertex data in the FIFO. Its vertex descriptor marks
position, colour and texture coordinates as INDEX8/INDEX16, so the command
stream carries *indices* and the real arrays live in guest RAM. Decoding those
index bytes as coordinates produces plausible-looking nonsense — tiny shapes
with identical x and y extents — which is exactly what the first replayer drew.

Two things are needed to recover real geometry:

  * a RAM model, populated from the capture's MemoryUpdate records
  * the CP array descriptors: base address at 0xA0+n, stride at 0xB0+n

Array slot numbering (CPMemory.h):
    0  position
    1  normal
    2  colour 0
    3  colour 1
    4..11  texture coordinates 0..7
"""
import struct

ARRAY_POSITION = 0
ARRAY_NORMAL = 1
ARRAY_COLOR0 = 2
ARRAY_TEXCOORD0 = 4


class GuestRAM:
    """Sparse RAM assembled from the capture's memory updates.

    Addresses are masked to 24 bits because MEM1 is 24 MB and the captured
    pointers carry the 0x8000_0000 cached-region tag.
    """

    MASK = 0x01FFFFFF

    def __init__(self):
        self.chunks = []          # (start, bytes), newest last

    def apply(self, address, data):
        self.chunks.append((address & self.MASK, data))

    def read(self, address, size):
        """Latest write wins, matching replay order."""
        a = address & self.MASK
        for start, data in reversed(self.chunks):
            if start <= a and a + size <= start + len(data):
                off = a - start
                return data[off:off + size]
        return None


class ArrayDescriptors:
    def __init__(self):
        self.base = [0] * 16
        self.stride = [0] * 16

    def set_reg(self, addr, value):
        if 0xA0 <= addr <= 0xAF:
            self.base[addr & 0x0F] = value
        elif 0xB0 <= addr <= 0xBF:
            self.stride[addr & 0x0F] = value

    def load_initial(self, cp_mem):
        for n in range(16):
            self.base[n] = cp_mem[0xA0 + n]
            self.stride[n] = cp_mem[0xB0 + n]

    def fetch(self, ram, slot, index, size):
        stride = self.stride[slot]
        if stride == 0:
            return None
        return ram.read(self.base[slot] + index * stride, size)


# Position component format -> (struct code, bytes)
POS_STRUCT = {0: ("B", 1), 1: ("b", 1), 2: ("H", 2), 3: ("h", 2), 4: ("f", 4)}


def resolve_positions(draw, ram, arrays):
    """Return [(x, y, z), ...] for one draw, following indices into RAM.

    Returns None when the arrays are not resolvable, so callers can report a
    miss rather than silently emit zeroes.
    """
    pres = draw["pos_presence"]
    cnt = draw["pos_cnt"]
    fmt = draw["pos_fmt"]
    frac = draw["pos_frac"]
    code, csize = POS_STRUCT.get(fmt, ("f", 4))
    scale = 1.0 if fmt == 4 else float(1 << frac)
    raw = bytes.fromhex(draw["data"])
    elem_size = cnt * csize

    out = []
    for v in range(draw["count"]):
        off = v * draw["vsize"] + draw["pos_offset"]
        if off + 2 > len(raw) and pres == 3:
            break
        if pres == 1:                                   # DIRECT
            if off + elem_size > len(raw):
                break
            comps = struct.unpack_from(f">{cnt}{code}", raw, off)
        else:                                           # INDEX8 / INDEX16
            if pres == 2:
                if off >= len(raw):
                    break
                index = raw[off]
            else:
                if off + 2 > len(raw):
                    break
                index = struct.unpack_from(">H", raw, off)[0]
            blob = arrays.fetch(ram, ARRAY_POSITION, index, elem_size)
            if blob is None or len(blob) < elem_size:
                return None
            comps = struct.unpack_from(f">{cnt}{code}", blob, 0)
        xyz = [c / scale for c in comps]
        while len(xyz) < 3:
            xyz.append(0.0)
        out.append(tuple(xyz))
    return out or None
