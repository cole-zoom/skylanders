// Models DSP output: pull samples from a lock-free ring in shared memory.
// AudioWorklet runs on its own realtime thread with a 128-frame quantum, so
// underruns here are a direct signal that the producer is not keeping up.
class RingPlayer extends AudioWorkletProcessor {
  constructor(opts) {
    super();
    const { sab, ringSab } = opts.processorOptions;
    this.state = new Int32Array(sab);
    this.ring = new Float32Array(ringSab);
    this.read = 0;
    this.underruns = 0;
    this.pulled = 0;
  }
  process(_inputs, outputs) {
    const out = outputs[0][0];
    const write = Atomics.load(this.state, 3);   // IDX_AUDIO_WRITE
    const avail = write - this.read;
    if (avail < out.length) { this.underruns++; out.fill(0); }
    else {
      for (let i = 0; i < out.length; i++)
        out[i] = this.ring[(this.read + i) % this.ring.length] * 0.05;
      this.read += out.length;
      this.pulled += out.length;
    }
    Atomics.store(this.state, 4, this.underruns);  // IDX_AUDIO_UNDERRUN
    return true;
  }
}
registerProcessor('ring-player', RingPlayer);
