// Saves: the game's own save files, the figures (their levels and gold) and
// the pipelines this browser has compiled, kept in IndexedDB across reloads.
//
// The emulator's files live in Emscripten's in-memory filesystem, which is
// on this thread (the CPU thread's file calls are proxied here), so the page
// reads and writes them directly: restored before main() runs, and copied
// out every 10 s. The game writes a save in several calls, so a file is
// stored only once two polls in a row find it unchanged -- never half a
// write. A file is read only when its size or time has moved since it was
// last stored: Cache/Shaders holds megabytes, and is rarely touched.
const ROOT = '/tmp/dolphin-boot-user/';
// The game's NAND title (SSPE), every figure, the UID cache, and the WGSL of
// every shader compiled (WebGPU::Shader::GetBinary), so a returning player
// skips glslang and Tint.
const KEPT = ['Wii/title/00010000/53535045', 'figures', 'Cache/SSPE52.uidcache', 'Cache/Shaders'];
// Rebuilt by any browser that plays; left out of Export.
const NOT_EXPORTED = 'Cache/Shaders/';
const DB = 'skylanders-saves', STORE = 'files';

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
const same = (a, b) => a && b && a.length === b.length && a.every((v, i) => v === b[i]);

// Everything stored, as path -> bytes. Called before the module loads.
export async function loadSaves() {
  const db = await openDb();
  const [keys, values] = await Promise.all([run(db, 'readonly', s => s.getAllKeys()),
                                            run(db, 'readonly', s => s.getAll())]);
  return { db, files: new Map(keys.map((k, i) => [k, values[i]])) };
}

// In preRun: put the files back where the emulator will look for them.
export function restoreSaves(FS, files) {
  for (const [path, data] of files) {
    FS.mkdirTree(ROOT + path.slice(0, path.lastIndexOf('/')));
    FS.writeFile(ROOT + path, data);
  }
}

// Every kept file, as path -> "size mtime".
function statKept(FS) {
  const out = new Map();
  const walk = rel => {
    let st;
    try { st = FS.stat(ROOT + rel); } catch { return; }
    if (FS.isDir(st.mode)) {
      for (const name of FS.readdir(ROOT + rel))
        if (name !== '.' && name !== '..') walk(`${rel}/${name}`);
    } else {
      out.set(rel, `${st.size} ${+st.mtime}`);
    }
  };
  KEPT.forEach(walk);
  return out;
}

const toBase64 = bytes => {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000)
    s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
};
const fromBase64 = text => Uint8Array.from(atob(text), c => c.charCodeAt(0));

// After start: keep copying, and wire the Export / Import / Reset buttons.
export function keepSaves(FS, { db, files }, ui) {
  const stored = new Map(files);
  // The stamp each stored file had when last found equal to what is stored.
  const storedStamp = new Map();
  let last = new Map();
  const show = text => { ui.status.textContent = text; };
  show(stored.size ? `Saves: ${stored.size} files restored` : 'Saves: none yet');

  const poll = async () => {
    const now = statKept(FS);
    const changed = [];
    for (const [p, stamp] of now) {
      if (stamp !== last.get(p) || stamp === storedStamp.get(p)) continue;
      const d = FS.readFile(ROOT + p);
      if (same(d, stored.get(p))) storedStamp.set(p, stamp);
      else changed.push([p, d, stamp]);
    }
    last = now;
    if (!changed.length) return;
    await run(db, 'readwrite', s => { for (const [p, d] of changed) s.put(d, p); });
    for (const [p, d, stamp] of changed) {
      stored.set(p, d);
      storedStamp.set(p, stamp);
    }
    show(`Saves: kept ${new Date().toLocaleTimeString()}`);
  };
  setInterval(() => poll().catch(e => show(`Saves: failed (${e.message})`)), 10000);

  ui.exportButton.addEventListener('click', () => {
    const body = JSON.stringify({ skylandersSaves: 1, files:
      Object.fromEntries([...stored].filter(([p]) => !p.startsWith(NOT_EXPORTED))
                                    .map(([p, d]) => [p, toBase64(d)])) });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([body], { type: 'application/json' }));
    a.download = `skylanders-saves-${new Date().toISOString().slice(0, 10)}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
  });
  ui.importButton.addEventListener('click', () => {
    const input = Object.assign(document.createElement('input'), { type: 'file', accept: '.json' });
    input.addEventListener('change', async () => {
      const { files: imported } = JSON.parse(await input.files[0].text());
      if (!confirm('Replace the saves in this browser with this file and restart?')) return;
      await run(db, 'readwrite', s => {
        s.clear();
        for (const [p, b64] of Object.entries(imported)) s.put(fromBase64(b64), p);
      });
      location.reload();
    });
    input.click();
  });
  ui.resetButton.addEventListener('click', async () => {
    if (!confirm('Delete every save in this browser (game, figures) and restart?')) return;
    await run(db, 'readwrite', s => s.clear());
    location.reload();
  });
}
