// Models an emulator CPU thread: block on a frame signal, do a burst of work,
// report done. Blocking with Atomics.wait is the whole point — this is what
// the main thread is forbidden from doing.
let state, scratch, id, budgetMs;

self.onmessage = (e) => {
  ({ id, budgetMs } = e.data);
  state = new Int32Array(e.data.sab);
  scratch = new Float64Array(e.data.work);
  loop();
};

const IDX_FRAME = 0;     // incremented by the pacer each frame
const IDX_DONE  = 1;     // workers increment when their burst completes

function loop() {
  let seen = 0;
  for (;;) {
    // Wait until the pacer publishes a new frame number.
    const cur = Atomics.load(state, IDX_FRAME);
    if (cur === seen) {
      Atomics.wait(state, IDX_FRAME, cur);   // legal here; illegal on main
      continue;
    }
    seen = cur;

    // Burn a fixed slice of wall time, touching shared memory so this cannot
    // be optimised away and so cache traffic is realistic.
    const end = performance.now() + budgetMs;
    let acc = 0;
    while (performance.now() < end) {
      for (let i = 0; i < 2000; i++) {
        const j = (i * 7 + id) & 1023;
        scratch[j] = scratch[j] * 1.0000001 + 1e-9;
        acc += scratch[j];
      }
    }
    if (acc === Infinity) scratch[0] = 0;   // defeat DCE

    Atomics.add(state, IDX_DONE, 1);
    Atomics.notify(state, IDX_DONE);
  }
}
