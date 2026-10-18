# Spike B — WebGPU and the GX pipeline

**Status: capability question ANSWERED and favourable. The "render one frame
correctly" gate is NOT met.**

What works end to end: capture a real frame from the game, decode 100% of it,
resolve its indexed vertex arrays out of guest RAM, push the geometry through
WebGPU in a real browser, and render a reference image from the same capture
with Dolphin's software rasteriser for comparison.

What does not: the WebGPU output is not a correct image. It draws the right
geometry in the wrong space, untextured and unshaded.

## Question

Does WebGPU have what Dolphin's GX renderer needs to draw *Skylanders*?

Asked generally this is unanswerable — GX has 16 TEV stages, 8 texture units and
a programmable blend pipeline that WebGPU has no equivalent for. But no game
uses all of GX. The useful question is what *this game* touches.

## What Skylanders actually uses

From `harness/traces/ssa-title.dff` — 3 frames, **57,628 bytes of commands,
100% decoded**, 238 draws, 3,667 vertices:

| GX feature | Used by SSA | WebGPU |
|---|---|---|
| TEV stages | **1–2** (of 16 possible) | trivial in WGSL |
| texgens | 1 | fine |
| Blend src factors | `src_alpha`, `one`, `inv_dst_alpha` | native |
| Blend dst factors | `inv_src_alpha`, `one` | native |
| Subtract blend | no | — |
| **Logic ops** | **not used** | would have been the hard one |
| Destination-alpha pass | yes (varies per draw) | `dual-source-blending` |
| Alpha test | yes | discard in WGSL |
| EFB copies | 10 | render targets + compute |
| Texture formats | 1 (I8), 3 (IA8), 6 (RGBA8), **14 (CMPR)** | `texture-compression-bc` |
| Primitives | **229 tri-fan**, 9 tri-strip | see below |
| Vertex layouts | `pos,tex0` ×22, `pos,col0,tex0` ×162, `pos,col0,tex0,tex1` ×54 | — |

Two findings matter more than the rest.

**Logic ops are unused.** GX logic ops require reading the destination colour
inside the fragment shader. WebGPU has no framebuffer fetch and no programmable
blending, so supporting them means a readback per draw or full subpass
emulation — expensive and invasive. This capture needs none of it.

**WebGPU has no triangle-fan topology, and 229 of 238 draws are fans.** Every
fan must be expanded to a triangle list. This is not an edge case to handle
later; it is the hot path, and it inflates both vertex count and bandwidth. The
replayer does the expansion (3,191 triangles from 3,667 vertices) and it is
cheap, but a real backend should do it in the vertex loader rather than per
draw.

## WebGPU capability — measured in Chrome 155, not read from docs

```
adapter: apple / metal-3
  YES  texture-compression-bc   CMPR/DXT1 (GX format 14), no CPU decode
  YES  dual-source-blending     dst-alpha pass in one pass
  YES  float32-filterable       filtered float EFB targets
maxTextureDimension2D 16384   maxColorAttachments 8   maxBindGroups 4
device acquired: true
```

Every feature the capture needs is present. The adapter reporting **metal-3**
means WebGPU here *is* Metal — direct support for the plan's bet that the
backend should be modelled on `VideoBackends/Metal` rather than `OGL`.

## Why the gate is not met

`harness/reference/ssa-title-ref.png` is the real title screen, rendered by
Dolphin's software rasteriser from our capture.
`harness/reference/ssa-title-webgpu.png` is what the replayer draws: correct
geometry, wrong space, flat colours.

Three things are missing, and all three are Phase 3 work rather than spike work:

1. **No XF transform.** Positions arrive in mixed spaces — screen-space quads
   spanning 0–811 × 0–456 alongside object-space 3D geometry. Without tracking
   the XF position and projection matrices, the replayer normalises against
   global bounds, which squashes everything together.
2. **No textures.** I8, IA8, RGBA8 and CMPR all need decode and upload. The
   title screen is almost entirely textured, so an untextured render cannot
   resemble it.
3. **No TEV.** The 1–2 stages per draw need translating to WGSL, along with
   per-draw blend state.

So the spike establishes that the path is open and the capability is there. It
does not establish that the renderer is correct, because there is not yet a
renderer to be correct.

## Bugs found, each of which produced plausible wrong output

1. **Recording at `State::Running` captures nothing.** The core reaches Running
   long before the game submits GX work; the first capture was 55 bytes with
   zero memory updates. Hence `--fifo-delay`.
2. **XF register count misdecoded.** The load count is bits 16–19 of the `u32`
   after the opcode, not the top nibble of the first `u16`. Desynchronised the
   decoder onto invalid opcodes. After the fix, XF went 0 → 13 distinct.
3. **Initial register snapshot ignored.** Decoding the stream shows only what
   *changed*; the header's BP/CP/XF snapshots hold the full configured state.
   Reading them took TEV evidence from "n/a" to "1–2".
4. **VCD treated as per-format.** VCD is a single global descriptor (CP 0x50 /
   0x60); only VAT is per-format (0x70/0x80/0x90 ×8). Indexing VCD per format
   read zero, so every vertex format decoded as "empty".
5. **VAT read after the fact.** `export_draws.py` originally read `vat_a` once
   decoding finished, giving the *final* VAT state for every draw. VAT changes
   between draws, so position format and fractional shift were wrong for most
   of them. Now captured at draw time.
6. **Indexed arrays decoded as inline data.** The biggest one. Skylanders marks
   position, colour and texcoords as INDEX8, so the FIFO carries *indices* and
   the real arrays live in guest RAM. Decoding index bytes as coordinates gave
   tiny shapes with suspiciously identical x and y extents — plausible-looking
   nonsense. Fixed by modelling RAM from the capture's memory updates and
   following the CP array base (0xA0+n) and stride (0xB0+n) descriptors.
7. **Frame record stride wrong.** `FileFrameInfo` is 64 bytes — six packed
   fields in the first 32, then a 32-byte reserved tail. Reading it as 40 made
   `memoryUpdatesOffset` garbage and made frames 1 and 2 look empty. Fixing it
   took the capture from 79 draws to 238 and from 19,180 bytes to 57,628.

Items 6 and 7 are why the earlier "5.4% decoded, 79 draws" figures were wrong.

## Limits that still stand

- **Title screen, not gameplay.** Captured 75 s into boot with no input. Real
  gameplay will use more TEV stages, more texgens, probably indirect textures,
  possibly logic ops. **Every row of the inventory is a floor, not a ceiling.**
  Capturing gameplay needs scripted input (a `.dtm` movie) and a Skylander on
  the emulated portal.
- **One adapter.** Apple/Metal via Chrome 155. Nothing here speaks to
  Windows/D3D12, Linux/Vulkan, Firefox, or Safari.
- **Three undecoded `0x48` opcodes** remain in the stream. Small, but not zero.

## Next

1. Track XF position/projection matrices so geometry lands in the right space.
2. Texture decode for I8, IA8, RGBA8, CMPR — CMPR uploads directly as BC1.
3. TEV → WGSL for the 1–2 stages used, plus per-draw blend state.
4. Capture real gameplay before any scoping decision rests on this inventory.

## Reproduce

```sh
# capture (delay so the game is actually drawing)
dolphin-web/build-native/Binaries/dolphin-emu-nogui \
  -u /tmp/dolphin-user -p headless -v Null -a HLE \
  --fifo-record=3 --fifo-delay=75 --fifo-output=harness/traces/ssa-title.dff \
  -e "skylanders-rom/<disc>.rvz"

# reference image from the same capture
dolphin-web/build-native/Binaries/dolphin-emu-nogui \
  -u /tmp/dolphin-user -p headless -v Software -a HLE \
  --screenshot-after=15 --screenshot-path=ssa-title-ref \
  -e harness/traces/ssa-title.dff

cd harness/src
python3 fifo_inventory.py ../traces/ssa-title.dff
python3 export_draws.py ../traces/ssa-title.dff ../traces/ssa-title-draws.json

cd ../../spikes/webgpu
node run-probe.mjs probe.html                                    # capability
node run-probe.mjs replay.html ../../harness/reference/out.png   # replay
```
