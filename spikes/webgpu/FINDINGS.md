# Spike B — WebGPU and the GX pipeline

**Status: PARTIAL. Capability question answered; the "render one frame" gate is
NOT met.**

## Question

Does WebGPU have what Dolphin's GX renderer needs to draw *Skylanders*?

The general form of that question is unanswerable and useless — GX has 16 TEV
stages, 8 texture units and a programmable blend pipeline, and WebGPU has none
of it directly. But **no game uses all of GX**. The useful question is what
*this game* touches. So: capture a real frame, inventory it, and check that
specific list against a real browser.

## What was built

1. **Headless FIFO capture.** `dolphin-emu-nogui` gained `--fifo-record=N`,
   `--fifo-delay=SECONDS` and `--fifo-output=PATH`
   (`dolphin-web/patches/0001-nogui-fifo-record.patch`). Upstream wires FIFO
   recording only into the Qt GUI, so there was no headless path at all.
2. **FIFO inventory tool.** `harness/src/fifo_inventory.py` parses the `.dff`,
   reads the initial BP/CP/XF register snapshots, and decodes the command
   stream, reporting which GX features appear.
3. **WebGPU capability probe.** `spikes/webgpu/probe.html` driven by
   `run-probe.mjs` over the DevTools Protocol, run against real Chrome 155.

## What Skylanders actually uses

From `harness/traces/ssa-title.dff` — 3 frames captured 75 s into boot,
19,180 bytes of commands, 31 memory updates, 147 registers set in the initial
snapshot:

| GX feature | Used by SSA | WebGPU |
|---|---|---|
| TEV stages | **1–2** (of 16 possible) | trivial in WGSL |
| texgens | 1 | fine |
| Blend factors | `src_alpha` / `inv_src_alpha` only | native |
| Subtract blend | no | — |
| **Logic ops** | **not used** | would have been the hard one |
| Destination-alpha pass | **yes** | `dual-source-blending` |
| Alpha test | yes | discard in WGSL |
| EFB copies | 4 | render targets + compute |
| Texture formats | 1 (I8), 3 (IA8), **14 (CMPR/DXT1)** | `texture-compression-bc` |
| Primitives | tri-fan | index rewrite |

**The absence of logic ops is the single most important line in this table.**
GX logic ops require reading the destination colour inside the fragment shader.
WebGPU has no framebuffer fetch and no programmable blending, so supporting them
means either a readback per draw or a full subpass emulation — expensive and
invasive. This capture does not use them.

## WebGPU capability — real Chrome 155, not documentation

```
adapter: apple / metal-3
features (26): ... dual-source-blending, float32-blendable, float32-filterable,
               texture-compression-astc, texture-compression-bc,
               texture-compression-etc2, ...

needed by the SSA capture:
  YES  texture-compression-bc   CMPR/DXT1 textures (GX format 14), no CPU decode
  YES  dual-source-blending     GX dst-alpha pass in one pass
  YES  float32-filterable       filtered float targets for EFB formats

maxTextureDimension2D 16384   maxColorAttachments 8   maxBindGroups 4
maxStorageBuffersPerStage 10  maxComputeWorkgroupSizeX 1024
device acquired: true
```

Every feature the capture needs is present. Two notes:

- `texture-compression-bc` means CMPR textures upload directly as BC1 rather
  than being decoded on CPU or in a compute pass — a real saving, since CMPR is
  the dominant format in Wii games.
- The adapter reports **metal-3**. WebGPU on this machine *is* Metal, which is
  direct support for the plan's bet that the WebGPU backend should be modelled
  on `VideoBackends/Metal` rather than `OGL`.

## Why this does not meet the gate

The Phase 0 gate for Spike B is "replay one captured frame through a minimal
WebGPU renderer and get a correct image." **That renderer does not exist yet.**
What is established is that the *capability* question has a favourable answer
and the capture pipeline works end to end.

Further limits on the inventory itself:

- **Title/boot screen, not gameplay.** Captured 75 s into boot with no input, so
  this is menu and logo rendering. Gameplay will use more TEV stages, more
  texgens, probably indirect textures, and possibly logic ops. Treat every row
  above as a floor.
- **Only 5.4% of the command stream is decoded.** The decoder stops at the first
  draw in each frame because vertex payload length depends on the active vertex
  descriptor (VCD/VAT), which the tool does not track. Most of the per-frame
  feature evidence comes from the initial register snapshot, not the stream.
- **Only frame 0 carried commands.** Frames 1 and 2 were empty, so this is
  effectively a one-frame sample.
- **One adapter.** Apple/Metal via Chrome 155. Nothing here says anything about
  Windows/D3D12, Linux/Vulkan, or Firefox and Safari.

## Bugs found and fixed along the way

1. **Recording at `State::Running` captures nothing.** The core reaches Running
   long before the game submits any GX work; the first capture produced 55 bytes
   and zero memory updates. Hence `--fifo-delay`.
2. **XF register count misdecoded.** The load count is bits 16–19 of the `u32`
   following the opcode; the first version read the top nibble of the first
   `u16`, desynchronising the decoder onto invalid opcodes (`0x48`, `0x40`,
   `0xc3`, `0xd5`). After the fix, XF registers went from 0 to 13 distinct and
   only one stray opcode remains.
3. **The initial register snapshot was ignored.** Decoding only the stream shows
   what *changed*; the header's BP/CP/XF snapshots record the full configured
   state. Reading them took TEV stage evidence from "n/a" to "1–2".
4. **`--dump-dom` fires too early for WebGPU.** Adapter and device requests are
   promises, so the dumped DOM still said `running`. Replaced with a DevTools
   Protocol client that polls for a real result.

## Next

1. Track VCD/VAT so the decoder can skip vertex payloads and cover 100% of the
   stream rather than 5.4%.
2. Capture actual gameplay, which needs scripted input (a `.dtm` movie) rather
   than a wall-clock delay.
3. Build the minimal WGSL/WebGPU replayer that the gate actually asks for.

## Reproduce

```sh
# capture (75s delay so the game is drawing)
dolphin-web/build-native/Binaries/dolphin-emu-nogui \
  -u /tmp/dolphin-user -p headless -v Null -a HLE \
  --fifo-record=3 --fifo-delay=75 --fifo-output=harness/traces/ssa-title.dff \
  -e "skylanders-rom/<disc>.rvz"

python3 harness/src/fifo_inventory.py harness/traces/ssa-title.dff
cd spikes/webgpu && node run-probe.mjs probe.html
```
