"""GX vertex descriptor decoding — how many bytes one vertex occupies.

Needed to skip vertex payloads while walking a FIFO command stream. Without
this a decoder must stop at the first draw, which on a real capture means
seeing about 5% of the frame.

A draw opcode's low 3 bits select one of 8 vertex formats. Each format is
described by five CP registers:

    0x50    VCD lo   which attributes are present, and how (direct / indexed)
    0x60    VCD hi   same, for the 8 texture coordinate sets

VCD is GLOBAL -- one descriptor shared by all formats -- while VAT is per
format. Indexing VCD per format reads zero and every draw looks empty.
    0x70+n  VAT A    component counts and formats: position, normal, colours, tex0
    0x80+n  VAT B    tex1..tex4
    0x90+n  VAT C    tex4..tex7

Layouts from Source/Core/VideoCommon/CPMemory.h.
"""

# Attribute presence (2 bits per attribute in the VCD registers)
NOT_PRESENT, DIRECT, INDEX8, INDEX16 = 0, 1, 2, 3

# Component format -> bytes per component (VAT *Format fields)
#   0=u8 1=s8 2=u16 3=s16 4=f32
COMP_SIZE = [1, 1, 2, 2, 4]

# Colour format -> bytes for the whole colour (VAT *Comp fields)
#   0=RGB565 1=RGB888 2=RGB888x 3=RGBA4444 4=RGBA6666 5=RGBA8888
COLOR_SIZE = [2, 3, 4, 2, 3, 4]


class VertexFormats:
    """Tracks the 8 CP vertex formats as the stream mutates them."""

    def __init__(self):
        self.vcd_lo = 0      # global, not per-format
        self.vcd_hi = 0
        self.vat_a = [0] * 8
        self.vat_b = [0] * 8
        self.vat_c = [0] * 8

    def load_initial(self, cp_mem):
        """Seed from the capture's initial CP memory snapshot."""
        self.set_reg(0x50, cp_mem[0x50])
        self.set_reg(0x60, cp_mem[0x60])
        for n in range(8):
            self.set_reg(0x70 + n, cp_mem[0x70 + n])
            self.set_reg(0x80 + n, cp_mem[0x80 + n])
            self.set_reg(0x90 + n, cp_mem[0x90 + n])

    def set_reg(self, addr, value):
        group, n = addr & 0xF0, addr & 0x07
        if group == 0x50:
            self.vcd_lo = value
        elif group == 0x60:
            self.vcd_hi = value
        elif group == 0x70:
            self.vat_a[n] = value
        elif group == 0x80:
            self.vat_b[n] = value
        elif group == 0x90:
            self.vat_c[n] = value

    # ── attribute presence ──
    def _pos(self):
        return (self.vcd_lo >> 9) & 3

    def _nrm(self):
        return (self.vcd_lo >> 11) & 3

    def _col(self, i):
        return (self.vcd_lo >> (13 + 2 * i)) & 3

    def _tex(self, i):
        return (self.vcd_hi >> (2 * i)) & 3

    @staticmethod
    def _indexed_size(presence, direct_size):
        """INDEX8/INDEX16 send an index instead of the data itself."""
        if presence == NOT_PRESENT:
            return 0
        if presence == INDEX8:
            return 1
        if presence == INDEX16:
            return 2
        return direct_size

    def vertex_size(self, n):
        """Bytes one vertex occupies under format `n`."""
        a, b, c = self.vat_a[n], self.vat_b[n], self.vat_c[n]
        size = 0

        # Matrix indices are always one byte each when present.
        if self.vcd_lo & 1:
            size += 1
        for i in range(8):
            if (self.vcd_lo >> (1 + i)) & 1:
                size += 1

        # Position: 2 or 3 components.
        pos_cnt = 3 if (a & 1) else 2
        pos_fmt = (a >> 1) & 7
        size += self._indexed_size(self._pos(),
                                   pos_cnt * COMP_SIZE[min(pos_fmt, 4)])

        # Normal: 3 components, or 9 for the normal/binormal/tangent triple.
        nrm_cnt = 9 if ((a >> 9) & 1) else 3
        nrm_fmt = (a >> 10) & 7
        size += self._indexed_size(self._nrm(),
                                   nrm_cnt * COMP_SIZE[min(nrm_fmt, 4)])

        # Colours 0 and 1.
        for i, shift in enumerate((13, 17)):
            comp = (a >> (shift + 1)) & 7
            size += self._indexed_size(self._col(i),
                                       COLOR_SIZE[min(comp, 5)])

        # Texture coordinates 0..7, 1 or 2 components each.
        # tex0 lives in VAT A, tex1-3 in B, tex4 straddles B and C, tex5-7 in C.
        tex_fields = [
            ((a >> 21) & 1, (a >> 22) & 7),
            ((b >> 0) & 1, (b >> 1) & 7),
            ((b >> 9) & 1, (b >> 10) & 7),
            ((b >> 18) & 1, (b >> 19) & 7),
            ((b >> 27) & 1, (b >> 28) & 7),
            ((c >> 5) & 1, (c >> 6) & 7),
            ((c >> 14) & 1, (c >> 15) & 7),
            ((c >> 23) & 1, (c >> 24) & 7),
        ]
        for i, (elems, fmt) in enumerate(tex_fields):
            cnt = 2 if elems else 1
            size += self._indexed_size(self._tex(i),
                                       cnt * COMP_SIZE[min(fmt, 4)])
        return size

    def pos_offset(self):
        """Byte offset of the position attribute within a vertex.

        Matrix indices, when present, precede it — one byte each.
        """
        off = 1 if (self.vcd_lo & 1) else 0
        for i in range(8):
            if (self.vcd_lo >> (1 + i)) & 1:
                off += 1
        return off

    def describe(self, n):
        names = []
        if self._pos():
            names.append("pos")
        if self._nrm():
            names.append("nrm")
        for i in range(2):
            if self._col(i):
                names.append(f"col{i}")
        for i in range(8):
            if self._tex(i):
                names.append(f"tex{i}")
        return ",".join(names) or "empty"
