// The disc's chunks, kept in this browser across visits.
//
// The emulator reads the disc in chunk-aligned HTTP range requests
// (Harness::HttpByteSource, one megabyte by default). Across the internet
// each costs a round trip to the store, 200-500 ms, and the game opens a new
// file -- a new figure, a level, a menu -- with a read nobody could have
// fetched ahead: that is the lag a player feels the first time anything new
// happens. This worker sits between the emulator's threads and the network
// and keeps every chunk it sees in Cache Storage, so a chunk crosses the
// internet once per browser rather than once per visit.
//
// Registered by web/shell.js as sw.js?disc=<url> when the disc is on another
// origin (the object store); it touches only range requests to that URL and
// lets everything else through. The emulator's reads are synchronous XHRs
// from its pthread workers, which a service worker controls like any fetch.
const disc = new URL(location.href).searchParams.get('disc');
const STORE = 'disc-chunks-v1';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', event => {
  const req = event.request;
  if (!disc || req.method !== 'GET' || req.url !== disc) return;
  const m = /^bytes=(\d+)-(\d+)$/.exec(req.headers.get('Range') ?? '');
  if (!m) return;
  const first = Number(m[1]), last = Number(m[2]);
  // Chunk-aligned reads only (a read of exactly what the game asked, with
  // --disc-chunk=0, would fill the store with overlapping pieces).
  const size = last - first + 1;
  if (size < 65536 || first % size) return;
  event.respondWith(chunk(req, first, size));
});

// The key: the disc's URL (it names one image; the object store's name
// carries a content hash) and the range, as a URL Cache Storage accepts.
const keyOf = (first, size) =>
  `https://disc-chunk.invalid/${encodeURIComponent(disc)}/${first}-${size}`;

const answer = (body, first, total) => new Response(body, {
  status: 206,
  headers: {
    'Content-Type': 'application/octet-stream',
    'Content-Length': String(body.byteLength),
    'Content-Range': `bytes ${first}-${first + body.byteLength - 1}/${total}`,
  },
});

async function chunk(req, first, size) {
  const store = await caches.open(STORE).catch(() => null);
  const key = keyOf(first, size);
  const kept = await store?.match(key).catch(() => null);
  if (kept) return answer(await kept.arrayBuffer(), first, kept.headers.get('X-Disc-Size') ?? '*');

  const res = await fetch(req);
  if (res.status !== 206) return res;
  const body = await res.arrayBuffer();
  const total = (res.headers.get('Content-Range') ?? '').split('/')[1] ?? '*';
  // Kept after answering: the emulator's thread is waiting for this chunk,
  // not for the store. A full or failing store only means no keeping.
  store?.put(key, new Response(body.slice(0), { headers: { 'X-Disc-Size': total } }))
    .catch(() => {});
  return answer(body, first, total);
}
