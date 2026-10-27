// The Portal of Power: put figures on and take them off. Talks to the
// emulator through the PortalBox in wasm memory (bootgame.cpp): fill the
// request, bump `request`, wait for `done` to match.
//
// PortalBox, as i32 words: 0 request, 1 done, 2 op, 3 id, 4 var, 5 slot,
// 6 result, 7 version, then 16 slots of (present, id, var) from word 8.
const GAMES = ["Spyro's Adventure", 'Giants', 'Swap Force', 'Trap Team', 'SuperChargers'];
const ELEMENTS = [, 'Magic', 'Fire', 'Air', 'Life', 'Undead', 'Earth', 'Water', 'Tech', 'Dark',
                  'Light', 'Other'];
const TYPES = [, 'Skylander', 'Giant', 'Swapper', 'Trap Master', 'Mini', 'Magic item', 'Trophy',
               'Vehicle', 'Trap', 'Other'];
const SLOTS = 16;

const cString = (heap, ptr) => {
  let end = ptr;
  while (heap[end]) end++;
  return new TextDecoder().decode(heap.slice(ptr, end));
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

export function attachPortal(module, root) {
  const box = new Int32Array(module.HEAPU8.buffer, module._harness_portal(), 8 + 3 * SLOTS);
  const figures = JSON.parse(cString(module.HEAPU8, module._harness_figure_list()))
    .map(([id, variant, name, game, element, type]) => ({ id, variant, name, game, element, type }));
  const byKey = new Map(figures.map(f => [`${f.id}:${f.variant}`, f]));

  // One request at a time, in order.
  let queue = Promise.resolve();
  const send = (op, id = 0, variant = 0, slot = 0) => (queue = queue.then(async () => {
    while (Atomics.load(box, 1) !== Atomics.load(box, 0)) await sleep(20);
    box[2] = op; box[3] = id; box[4] = variant; box[5] = slot;
    const request = Atomics.add(box, 0, 1) + 1;
    while (Atomics.load(box, 1) !== request) await sleep(20);
    return box[6];
  }));

  root.innerHTML = `
    <h2>Portal of Power</h2>
    <ul class="slots"></ul>
    <div class="find">
      <input type="search" placeholder="Find a figure" aria-label="Find a figure">
      <select data-k="game" aria-label="Game"><option value="0">${GAMES[0]}</option>
        <option value="">All games</option>
        ${GAMES.slice(1).map((g, i) => `<option value="${i + 1}">${g}</option>`).join('')}</select>
      <select data-k="element" aria-label="Element"><option value="">All elements</option>
        ${ELEMENTS.map((e, i) => e ? `<option value="${i}">${e}</option>` : '').join('')}</select>
      <select data-k="type" aria-label="Type"><option value="">All types</option>
        ${TYPES.map((t, i) => t ? `<option value="${i}">${t}</option>` : '').join('')}</select>
    </div>
    <ul class="figures"></ul>`;
  const slotsEl = root.querySelector('.slots');
  const figuresEl = root.querySelector('.figures');
  const search = root.querySelector('input');
  const selects = [...root.querySelectorAll('select')];
  // Keys typed into the panel are not the game's.
  root.addEventListener('keydown', e => e.stopPropagation());
  root.addEventListener('keyup', e => e.stopPropagation());

  const label = f => `${f.name} · ${ELEMENTS[f.element] ?? ''}`;
  let version = -1;
  const showSlots = () => {
    const v = Atomics.load(box, 7);
    if (v === version) return;
    version = v;
    slotsEl.replaceChildren();
    for (let s = 0; s < SLOTS; s++) {
      const [present, id, variant] = box.subarray(8 + 3 * s, 11 + 3 * s);
      if (!present) continue;
      const f = byKey.get(`${id}:${variant}`) ?? { name: `figure ${id}:${variant}` };
      const li = document.createElement('li');
      const off = document.createElement('button');
      off.textContent = 'Take off';
      off.addEventListener('click', () => send(2, 0, 0, s));
      li.append(label(f), ' ', off);
      slotsEl.append(li);
    }
    if (!slotsEl.children.length) slotsEl.innerHTML = '<li class="empty">Empty</li>';
  };

  const showFigures = () => {
    const words = search.value.toLowerCase().split(/\s+/).filter(Boolean);
    const want = Object.fromEntries(selects.map(s => [s.dataset.k, s.value]));
    const hits = figures.filter(f =>
      (want.game === '' || f.game === +want.game) &&
      (want.element === '' || f.element === +want.element) &&
      (want.type === '' || f.type === +want.type) &&
      words.every(w => f.name.toLowerCase().includes(w)));
    figuresEl.replaceChildren(...hits.slice(0, 80).map(f => {
      const li = document.createElement('li');
      const on = document.createElement('button');
      on.textContent = f.name;
      on.title = `${ELEMENTS[f.element] ?? ''} ${TYPES[f.type] ?? ''} · ${GAMES[f.game]}`;
      on.addEventListener('click', async () => {
        on.disabled = true;
        const slot = await send(1, f.id, f.variant);
        on.disabled = false;
        if (slot < 0) on.title = 'The portal refused it (full?)';
      });
      li.append(on);
      return li;
    }));
    if (hits.length > 80) figuresEl.append(Object.assign(document.createElement('li'),
      { className: 'empty', textContent: `…and ${hits.length - 80} more; narrow the search` }));
  };
  search.addEventListener('input', showFigures);
  selects.forEach(s => s.addEventListener('change', showFigures));
  showFigures();
  setInterval(showSlots, 250);
  showSlots();
}
