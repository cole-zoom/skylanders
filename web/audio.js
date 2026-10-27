// The game's sound: an AudioWorklet (audio-worklet.js) playing the ring the
// emulator fills (bootgame.cpp's --audio). Browsers start audio only after
// the player interacts with the page, so the context is resumed on the first
// key press or click; the button mutes and unmutes.
export async function attachAudio(heap, address, button) {
  const ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('audio-worklet.js');
  const node = new AudioWorkletNode(ctx, 'ring-player', {
    numberOfInputs: 0,
    outputChannelCount: [2],
    processorOptions: { buffer: heap, address },
  });
  const gain = ctx.createGain();
  node.connect(gain).connect(ctx.destination);

  let muted = false;
  const show = () => {
    const on = !muted && ctx.state === 'running';
    button.textContent = on ? 'Sound on' : ctx.state === 'running' ? 'Sound off' : 'Sound (press a key)';
  };
  const wake = () => { if (ctx.state !== 'running') ctx.resume().then(show); };
  addEventListener('keydown', wake);
  addEventListener('pointerdown', wake);
  ctx.addEventListener('statechange', show);
  button.addEventListener('click', () => {
    muted = !muted;
    gain.gain.value = muted ? 0 : 1;
    show();
  });
  show();
}
