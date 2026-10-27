// Save states: the whole machine, in four slots, kept in IndexedDB
// gzipped, and downloadable. The emulator does the saving and loading on
// its CPU thread (StateBox in bootgame.cpp); this side moves the bytes.
//
// StateBox, as i32 words: 0 request, 1 done, 2 op, 3 ptr, 4 size, 5 result,
// then a 160-byte message (why it failed) at byte 24.
// Ops: 1 save, 2 load (ptr, size), 3 allocate (size), 4 free (ptr).
const SLOTS = 4;
const DB = 'skylanders-states', STORE = 'states';

const openDb = () => new Promise((resolve, reject) => {
  const r = indexedDB.open(DB, 1);
  r.onupgradeneeded = () => r.result.createObjectStore(STORE);
  r.onsuccess = () => resolve(r.result);
  r.onerror = () => reject(r.error);
});
const run = (db, mode, fn) => new Promise((resolve, reject) => {
  const t = db.transaction(STORE, mode);
  const req = fn(t.objectStore(STORE));
  t.oncomplete = () => resolve(req?.result);
  t.onerror = () => reject(t.error);
});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const gzip = bytes => new Response(new Blob([bytes]).stream()
  .pipeThrough(new CompressionStream('gzip'))).blob();
const gunzip = async blob => new Uint8Array(await new Response(blob.stream()
  .pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());

export async function attachStates(module, root) {
  const db = await openDb();
  const heap = () => module.HEAPU8;
  const box = new Int32Array(heap().buffer, module._harness_states(), 6);
  const message = () => {
    const at = module._harness_states() + 24, h = heap();
    let end = at;
    while (h[end] && end < at + 160) end++;
    return new TextDecoder().decode(h.slice(at, end));
  };
  let queue = Promise.resolve();
  const send = (op, ptr = 0, size = 0) => (queue = queue.then(async () => {
    while (Atomics.load(box, 1) !== Atomics.load(box, 0)) await sleep(20);
    box[2] = op; box[3] = ptr; box[4] = size;
    const request = Atomics.add(box, 0, 1) + 1;
    while (Atomics.load(box, 1) !== request) await sleep(20);
    return { result: box[5], ptr: box[3] >>> 0, message: message() };
  }));

  root.innerHTML = '<h2>Save states</h2><ul class="states"></ul><p class="state-status"></p>';
  const list = root.querySelector('.states');
  const status = root.querySelector('.state-status');
  let busy = false;
  const act = async (what, fn) => {
    if (busy) return;
    busy = true;
    status.textContent = `${what}…`;
    try { status.textContent = await fn(); }
    catch (e) { status.textContent = `${what} failed: ${e.message}`; }
    busy = false;
    show();
  };

  const save = slot => act(`Saving slot ${slot}`, async () => {
    const t0 = performance.now();
    const { result, ptr, message: why } = await send(1);
    if (result < 0) throw new Error(why);
    const raw = heap().slice(ptr, ptr + result);
    await send(4, ptr);
    const blob = await gzip(raw);
    await run(db, 'readwrite', s => s.put({ when: Date.now(), blob }, slot));
    return `Saved slot ${slot}: ${(raw.length / 2 ** 20).toFixed(0)} MB, ` +
           `${(blob.size / 2 ** 20).toFixed(1)} MB kept, ${((performance.now() - t0) / 1000).toFixed(1)} s`;
  });
  const loadBytes = async (bytes, what) => {
    const { result: ptr, message: why } = await send(3, 0, bytes.length);
    if (ptr < 0) throw new Error(why);
    heap().set(bytes, ptr >>> 0);
    const r = await send(2, ptr >>> 0, bytes.length);
    if (r.result < 0) throw new Error(r.message);
    return `Loaded ${what}`;
  };
  const load = slot => act(`Loading slot ${slot}`, async () => {
    const entry = await run(db, 'readonly', s => s.get(slot));
    if (!entry) return `Slot ${slot} is empty`;
    return loadBytes(await gunzip(entry.blob), `slot ${slot}`);
  });
  const download = slot => act(`Downloading slot ${slot}`, async () => {
    const entry = await run(db, 'readonly', s => s.get(slot));
    if (!entry) return `Slot ${slot} is empty`;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(entry.blob);
    a.download = `skylanders-state-${new Date(entry.when).toISOString().slice(0, 16).replace(':', '')}.sav.gz`;
    a.click();
    URL.revokeObjectURL(a.href);
    return `Downloaded slot ${slot}`;
  });
  const upload = slot => {
    const input = Object.assign(document.createElement('input'), { type: 'file', accept: '.gz' });
    input.addEventListener('change', () => act(`Loading the file into slot ${slot}`, async () => {
      const blob = input.files[0];
      const bytes = await gunzip(blob);  // fails early on a file that is not one
      await run(db, 'readwrite', s => s.put({ when: Date.now(), blob }, slot));
      return loadBytes(bytes, `${input.files[0].name} (now slot ${slot})`);
    }));
    input.click();
  };

  const show = async () => {
    const entries = await Promise.all([...Array(SLOTS)].map((_, i) =>
      run(db, 'readonly', s => s.get(i + 1))));
    list.replaceChildren(...entries.map((e, i) => {
      const slot = i + 1;
      const li = document.createElement('li');
      li.append(`${slot}: ${e ? new Date(e.when).toLocaleString() : 'empty'} `);
      for (const [text, fn, needs] of [['Save', save, false], ['Load', load, true],
                                       ['Download', download, true], ['Upload', upload, false]]) {
        const b = document.createElement('button');
        b.textContent = text;
        b.disabled = needs && !e;
        b.addEventListener('click', () => fn(slot));
        li.append(b);
      }
      return li;
    }));
  };
  // For tests that put a state in the store behind the panel's back.
  root.refresh = show;
  show();
}
