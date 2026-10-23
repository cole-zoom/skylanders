# Spike S2 — Dolphin's shaders, SPIR-V to WGSL through Tint

**Status: ANSWERED, favourable. Tint is the translator.** Every shader the
game creates in 390 seconds translates and validates in Chrome, after four
changes to what VideoCommon emits for this backend. Plan:
`docs/PHASE3-WEBGPU.md` §3 and §5.

## What was run

1. **The shaders.** `bootgame --dump-shaders` (patch 0017, a hook in
   `NullGfx::CreateShaderFromSource`) reports the capabilities a WebGPU
   backend would — Metal's (`MTLUtil.mm`), less what core WebGPU lacks:
   dynamic sampler indexing, subgroups, bounding box — so VideoCommon
   generates that backend's shaders, in Metal's GLSL dialect. The 390-s
   gameplay run, title to tutorial level: **349 distinct shaders, 55 vertex
   and 294 pixel** (16 by the end of the boot).
2. **GLSL → SPIR-V**: `glsl2spv.cpp`, glslang under Dolphin's options
   (`Spirv.cpp`'s `CompileShaderToSPV`), with Metal's shader header minus its
   8- and 16-bit extensions.
3. **SPIR-V → WGSL**: Tint at the Dawn release `emdawnwebgpu` pins
   (`UPSTREAM.lock`), natively for coverage, then in wasm (`spv2wgsl.cpp`).
4. **Chrome's verdict**: `validate.html` — every WGSL module through
   `createShaderModule` and `getCompilationInfo` on a device with the
   features the backend would request.

## What failed, and what it takes

As generated today, Tint translates 11 of the 349. Every failure is one of
four kinds, and each has a small fix in what VideoCommon emits for an
`APIType::WebGPU`:

| | shaders | cause | the change |
|---|---|---|---|
| 1 | **285** — every TEV pixel shader | `uniform sampler2DArray samp[8]`: core WebGPU has no arrays of textures ("arrays of handle types are not supported") | eight bindings, and `sampleTextureWrapper` — the one indexing site, always called with a literal — as a switch the optimiser folds |
| 2 | **50** vertex | `isnan` → SPIR-V `OpIsNan`, which Tint's reader does not implement | the bit test `WriteIsNanHeader` already emits for D3D |
| 3 | **122** pixel (hidden behind 1) | `layout(early_fragment_tests)`: WGSL cannot force early depth tests | report `bSupportsEarlyZ = false`; VideoCommon has the other path |
| 4 | **3** — palette conversion | `uint16_t palette[]` in a storage buffer (Metal's path); WGSL has no 16-bit integers and WebGPU no texel buffers (Vulkan's) | read the palette as 32-bit words and unpack |

With 1–3 applied as text (`glsl2spv --rewrite`, a stand-in for the
generator changes): **346 of 346 translate; 346 of 346 are valid in Chrome**,
no warnings. Item 4's three shaders are the only ones not translated.

The device needs three optional features: `dual-source-blending` (285
pixel shaders use `@blend_src`), `clip-distances` (50 vertex shaders), and
`texture-compression-bc`. All three are present here (Chrome 155, Apple M4,
Metal); without `clip-distances` those vertex shaders need another way to
clip.

Combined image-samplers (`sampler2DArray` in GLSL, `OpTypeSampledImage` in
SPIR-V) were not a problem: Tint splits them.

## Tint in wasm

Built with Emscripten (`build-spv2wgsl.sh`; Dawn's CMake skips SPIR-V-Tools
under Emscripten, so `dawn-tint-under-emscripten.patch` lifts that for a
Tint-only build):

| | |
|---|---|
| translated | 346 / 346, every one **byte-identical** to the native Tint's output |
| time per shader | **5.9 ms** mean, 35 ms the first (cold) — Chrome 155's V8 under node |
| added to the wasm | **3.98 MB**, **1.15 MB gzip**; the emulator's is 6.7 MB today |

glslang is ~1.6 ms a shader natively (0.57 s for all 349), so perhaps 3 ms
in wasm: about **9 ms a shader, ~18 ms for a new pipeline's vertex and
pixel pair**, plus ~1 ms for Chrome's `createShaderModule`. The same order
as a native driver's compile. Two things make it a non-issue later:
translation is pure CPU work, so it can run on Dolphin's background compile
threads, leaving only `createShaderModule` on the GPU thread; and the WGSL
can be cached by UID across sessions (OPFS), skipping both steps.

## Reproduce

```sh
# Dawn, its four submodules, Tint native and for wasm
git -C dawn submodule update --init --depth 1 third_party/spirv-headers/src \
  third_party/spirv-tools/src third_party/abseil-cpp third_party/jinja2 third_party/markupsafe
git -C dawn apply ../spikes/webgpu-shaders/dawn-tint-under-emscripten.patch
cmake -S dawn -B dawn/out/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DDAWN_BUILD_SAMPLES=OFF \
  -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_SPV_READER=ON \
  -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF \
  -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_SPV_WRITER=OFF \
  -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_NULL=OFF -DDAWN_USE_GLFW=OFF \
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=OFF && ninja -C dawn/out/native tint
# the same options under emcmake, with -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_WGSL_READER=OFF
# -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF and
# CMAKE_{C,CXX}_FLAGS="-pthread -Wno-deprecated-pragma", into dawn/out/wasm: ninja tint_api

harness/wasm-core/run-jit.sh 300000000000 --jit --dual-core --input=A --skylander \
  --ticks=284310000000 --dump-shaders > shaders.log
spikes/webgpu-shaders/build-glsl2spv.sh && spikes/webgpu-shaders/out/glsl2spv shaders.log <dir> --rewrite
for f in <dir>/*.spv; do dawn/out/native/tint --format wgsl -o ${f%.spv}.wgsl $f; done
node spikes/webgpu-shaders/run-validate.mjs                        # reads out/game-rw/all.json
spikes/webgpu-shaders/build-spv2wgsl.sh && node spikes/webgpu-shaders/out/spv2wgsl.js <dir>
```

## Not answered here

- The generator changes themselves, and an `APIType::WebGPU` to hang them
  on; the binding layout WGSL ends up with (sets 0–3 map to groups 0–3, as
  Metal's dialect has them) against what the backend's bind groups will be.
- Ubershaders: the run used Dolphin's default, specialised shaders only.
- glslang inside wasm: timed natively only.
- Shaders of scenes past 390 s; the title screen's 9-stage TEV is in the set.
