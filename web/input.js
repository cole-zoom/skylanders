// The player's keyboard, mouse and gamepad, as the emulated Wii Remote and
// Nunchuk.
//
// The state goes straight into wasm memory, into the LiveInput struct that
// bootgame.cpp's --live-input overrides read (harness_live_input() gives its
// address). The heap is a SharedArrayBuffer, so the CPU thread sees these
// stores without a message or a proxied call. The layout must match the
// struct:
//
//   +0  u32  buttons, bit i = BUTTONS[i]
//   +4  f32  stick x   (-1..1)
//   +8  f32  stick y   (-1..1, up)
//   +12 f32  pointer x (-1..1)
//   +16 f32  pointer y (-1..1, up)
//   +20 u32  pointer on
export const BUTTONS = ['A', 'B', '1', '2', '+', '-', 'Home',
                        'Up', 'Down', 'Left', 'Right', 'C', 'Z'];
const BIT = Object.fromEntries(BUTTONS.map((b, i) => [b, 1 << i]));

// KeyboardEvent.code -> a Wii button, or a stick direction.
export const KEYS = {
  Space: 'A', KeyJ: 'A',
  ShiftLeft: 'B', KeyK: 'B',
  KeyE: 'C', KeyL: 'C',
  KeyQ: 'Z', KeyU: 'Z',
  ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right',
  Enter: '+', Backspace: '-', KeyH: 'Home',
  Digit1: '1', Digit2: '2',
};
const STICK_KEYS = { KeyW: [0, 1], KeyS: [0, -1], KeyA: [-1, 0], KeyD: [1, 0] };

// The standard gamepad mapping's button indices.
const PAD_BUTTONS = { 0: 'A', 1: 'B', 2: 'C', 3: 'Z', 4: '1', 5: '2', 6: 'Z', 7: 'B',
                      8: '-', 9: '+', 12: 'Up', 13: 'Down', 14: 'Left', 15: 'Right',
                      16: 'Home' };
const DEADZONE = 0.15;
// Right stick moves the pointer at this many screen-widths a second.
const POINTER_SPEED = 1.2;

const dead = v => (Math.abs(v) < DEADZONE ? 0 : v);
const clamp = v => Math.max(-1, Math.min(1, v));

export function attachInput(canvas, heap, address) {
  const u32 = new Uint32Array(heap, address, 6);
  const f32 = new Float32Array(heap, address, 6);
  const held = new Set();      // keys down
  const mouse = new Set();     // buttons from the mouse
  let pointer = null;          // [x, y] once the mouse or right stick has moved it
  let last = performance.now();

  const block = e => { if (e.code in KEYS || e.code in STICK_KEYS) e.preventDefault(); };
  addEventListener('keydown', e => { block(e); if (!e.repeat) held.add(e.code); });
  addEventListener('keyup', e => { block(e); held.delete(e.code); });
  // A key released while the window is not focused never sends keyup.
  addEventListener('blur', () => { held.clear(); mouse.clear(); });

  const toPointer = e => {
    const r = canvas.getBoundingClientRect();
    pointer = [clamp(((e.clientX - r.left) / r.width) * 2 - 1),
               clamp(1 - ((e.clientY - r.top) / r.height) * 2)];
  };
  canvas.addEventListener('mousemove', toPointer);
  canvas.addEventListener('mousedown', e => {
    toPointer(e);
    if (e.button === 0) mouse.add('A');
    if (e.button === 2) mouse.add('B');
  });
  addEventListener('mouseup', e => {
    if (e.button === 0) mouse.delete('A');
    if (e.button === 2) mouse.delete('B');
  });
  canvas.addEventListener('contextmenu', e => e.preventDefault());

  // Gamepads are polled, not evented, so the whole state is rebuilt every
  // animation frame -- well above the 60 Hz the Wii Remote is read at.
  const tick = now => {
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;
    let buttons = 0, sx = 0, sy = 0;
    for (const code of held) {
      if (code in KEYS) buttons |= BIT[KEYS[code]];
      if (code in STICK_KEYS) { sx += STICK_KEYS[code][0]; sy += STICK_KEYS[code][1]; }
    }
    for (const b of mouse) buttons |= BIT[b];
    for (const pad of navigator.getGamepads?.() ?? []) {
      if (!pad || pad.mapping !== 'standard') continue;
      pad.buttons.forEach((b, i) => { if (b.pressed && PAD_BUTTONS[i]) buttons |= BIT[PAD_BUTTONS[i]]; });
      sx += dead(pad.axes[0]);
      sy -= dead(pad.axes[1]);
      const px = dead(pad.axes[2]), py = dead(pad.axes[3]);
      if (px || py) {
        pointer ??= [0, 0];
        pointer = [clamp(pointer[0] + px * POINTER_SPEED * dt),
                   clamp(pointer[1] - py * POINTER_SPEED * dt)];
      }
    }
    // Two keys at once read as a diagonal, not as a longer push.
    const len = Math.hypot(sx, sy);
    if (len > 1) { sx /= len; sy /= len; }
    f32[1] = sx;
    f32[2] = sy;
    if (pointer) { f32[3] = pointer[0]; f32[4] = pointer[1]; u32[5] = 1; }
    u32[0] = buttons;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}
