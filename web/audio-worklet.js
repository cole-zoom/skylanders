// The AudioWorklet half of the game's sound: plays the stereo ring that
// bootgame.cpp's audio thread fills (AudioRing). Runs on the browser's audio
// thread, reading wasm memory directly -- the heap is a SharedArrayBuffer.
//
// Layout at `address`: u32 write, u32 read, u32 frames, u32 underruns, then
// `frames` interleaved s16 stereo frames. write and read count frames and
// only grow; this side owns read.
class RingPlayer extends AudioWorkletProcessor {
  constructor({ processorOptions: { buffer, address } }) {
    super();
    this.index = new Int32Array(buffer, address, 4);
    this.frames = this.index[2];
    this.data = new Int16Array(buffer, address + 16, 2 * this.frames);
  }

  process(_inputs, outputs) {
    const [left, right] = outputs[0];
    const n = left.length;
    const write = Atomics.load(this.index, 0) >>> 0;
    const read = Atomics.load(this.index, 1) >>> 0;
    if (((write - read) >>> 0) < n) {
      // Short: play silence and leave the ring alone, so what is there
      // plays whole next time rather than in slivers.
      Atomics.add(this.index, 3, 1);
      return true;
    }
    for (let i = 0; i < n; i++) {
      const at = ((read + i) % this.frames) * 2;
      left[i] = this.data[at] / 32768;
      right[i] = this.data[at + 1] / 32768;
    }
    Atomics.store(this.index, 1, (read + n) | 0);
    return true;
  }
}
registerProcessor('ring-player', RingPlayer);
