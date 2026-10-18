// Models the emulator GPU thread: owns the canvas via OffscreenCanvas and
// renders every frame, so rendered frames never pass through the main thread.
let device, ctx, pipeline, state, frames = 0, signals = 0;
const IDX_GPU_SIGNALS = 5;
const IDX_FRAME = 0, IDX_GPU_DONE = 2;

self.onmessage = async (e) => {
  const { canvas, sab } = e.data;
  state = new Int32Array(sab);
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) { self.postMessage({ error: 'no WebGPU adapter in worker' }); return; }
  device = await adapter.requestDevice();
  ctx = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  ctx.configure({ device, format, alphaMode: 'opaque' });

  const mod = device.createShaderModule({ code: `
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  var p = array(vec2f(-0.8,-0.8), vec2f(0.8,-0.8), vec2f(0.0,0.8));
  return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(0.25, 0.65, 0.95, 1.0); }` });

  pipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module: mod, entryPoint: 'vs' },
    fragment: { module: mod, entryPoint: 'fs', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
  });

  self.postMessage({ ready: true });
  render();
};

function render() {
  let seen = 0;
  // Block on the frame signal rather than polling. setTimeout(1) is clamped
  // and fires late under load, so two frames can advance between polls and
  // the worker coalesces them -- it rendered 481/600 that way. A worker is
  // allowed to block, and blocking is what the real GPU thread would do:
  // queue.submit() is fire-and-forget, so nothing needs servicing in between.
  const tick = () => {
    let cur = Atomics.load(state, IDX_FRAME);
    if (cur === seen) {
      Atomics.wait(state, IDX_FRAME, cur, 50);   // timeout keeps shutdown responsive
      cur = Atomics.load(state, IDX_FRAME);
    }
    if (cur !== seen) {
      // Count every frame signal observed, separately from frames actually
      // presented. If signals keeps up with the pacer but presents lag, the
      // limit is canvas presentation, not our threading.
      signals += (cur - seen);
      seen = cur;
      Atomics.store(state, IDX_GPU_SIGNALS, signals);
      const enc = device.createCommandEncoder();
      const pass = enc.beginRenderPass({ colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.03, g: 0.04, b: 0.07, a: 1 },
        loadOp: 'clear', storeOp: 'store' }] });
      pass.setPipeline(pipeline); pass.draw(3); pass.end();
      device.queue.submit([enc.finish()]);
      frames++;
      Atomics.store(state, IDX_GPU_DONE, frames);
    }
    setTimeout(tick, 0);
  };
  tick();
}
