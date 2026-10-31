# Handoff: the hosted game

Written 2026-10-31, after #117 (main 56d4414). For whoever picks up the
game as it is played from a host: the page on Vercel (`vercel.json`,
`api/disc.js`), the disc on an object store across the internet. Read
`docs/HANDOFF-FIGHT-SPEED.md` for the speed of the emulator itself,
which this file takes as given: the fights run at 100% on the player's
laptop when the disc is local.

## 0. The report

The player, on the hosted page, the evening of 2026-10-31: "it is
actually great, but it has lag spikes like every 10-20 seconds. it
goes down to like 50%". Their local sessions never showed that (the
session log of 2026-10-30: 203 seconds, one below 90%).

## 1. The finding: the disc, across the internet

The game streams from the disc while it plays -- its level, its
characters, its music, in order, ~31 KB a read, hundreds of reads a
minute. The page fetches the disc in megabyte chunks (#117) and keeps
them, so most reads hit the cache; a read that crosses into a chunk
nobody has fetches it, synchronously, on the CPU thread, which then
waits out one round trip to the store.

That round trip is the lag spike. From this machine, a range request to
the R2 bucket (`curl -r`, five samples): first byte after 180-750 ms,
the whole megabyte 250-800 ms later -- and a 32 KB range is no quicker,
190-280 ms to the first byte. The store charges a round trip per
request whatever the size, so a chunk costs a quarter to three quarters
of a second however small it is. With the game's streams advancing
through a chunk in ten to twenty seconds, that is one stall of 200-500
ms every ten to twenty seconds, which a per-second readout shows as a
second at 50-75%. Through the host's proxy (`api/disc.js`, a function
that fetches from the store and streams back) it is that plus the
function's own hop.

The harness sees it. The fight state against the R2 disc
(`DISC=<url> harness/wasm-core/run-jit.sh ... --frame-times`), the same
flags as the day's fight runs, which against the local disc show no
disc time at all:

```
hitch  t=1.25 s:  440 ms for 17 ms emulated; compile 0, disc 505, idle-wait 0 ms
hitch  t=1.47 s:  328 ms for 17 ms emulated; compile 0, disc 294, idle-wait 0 ms
hitch  t=1.38 s:  205 ms for 17 ms emulated; compile 0, disc 207, idle-wait 0 ms
hitch  t=6.71 s:  176 ms for 17 ms emulated; compile 0, disc 191, idle-wait 0 ms
hitch  t=11.94 s: 169 ms for 17 ms emulated; compile 0, disc 202, idle-wait 0 ms
hitch  t=2.17 s:  166 ms for 17 ms emulated; compile 0, disc 271, idle-wait 0 ms
disc   16 requests, 16.00 MB fetched
```

Sixteen chunks over twenty seconds of fight, each a stall of 190-505
ms on the CPU thread: the report, reproduced.

## 2. The fix: fetch the next chunk before the game asks (#118)

`Harness::HttpByteSource` takes `ahead`: after every read, the chunks
after the read's last one -- `ahead` of them -- are queued for a helper
thread, which fetches them into the cache on its own worker while the
emulator runs. A read that lands on a chunk the helper is fetching waits
for that fetch rather than starting its own. The game's streams are
sequential, and a chunk lasts them ten seconds or more, so the next one
is there long before it is wanted. `--disc-ahead=N`, default 2; the
page passes `?ahead=`.

The stats the harness prints grew to say what this does:
`disc window: N requests (M read ahead); reads waited X ms (Y% of the
window) in Z stalls` -- the time the game's reads spent waiting for the
transport inside the measured window, which is the number the player
feels. The page's readout line got `disc N in M ms` for the same thing
per second, so the next report can be read off the log.

Measured on the fight state against the R2 disc, read-ahead off, 2, 1,
2, off (ABCBA, the player's own game running on the machine throughout,
which halves every run's speed alike and leaves the disc waits as the
comparable figure):

| run | read-ahead | chunks in the window (of them ahead) | reads waited | stalls |
|---|---|---|---|---|
| 1 | off | 3 (0) | 688 ms | 3 |
| 2 | 2 | 7 (5) | 357 ms | 2 |
| 3 | 1 | 5 (3) | 257 ms | 2 |
| 4 | 2 | 7 (5) | 303 ms | 2 |
| 5 | off | 3 (0) | 417 ms | 3 |

Off, the reads waited 553 ms a fight on average; on, 306 ms: the
stalls on the streams the game reads in order are gone, and what is
left is two stalls a fight at the same places every run -- t=1.4 s, the
character and item files opened as the figure is placed, and t=6.7 s, a
jump back into the level file -- the first read of a file or region the
game had not touched, which no read-ahead can see coming. Those are
what a persistent cache (item 2 below) removes on every visit but the
first. The player's spikes every ten to twenty seconds in free play are
the stream kind, the kind this fixes; a new character or menu still
costs one round trip the first time.

The helper fetches about as many chunks again as the game asks for
(the fight: 16 on demand, 22 ahead of it): bytes transferred roughly
double where the reads are scattered. R2 meters no egress; a player on
a metered connection pays it.

The page also stops going through the host's function for every chunk
when it can read the store itself: `GET /disc?url` answers with the
store's URL (`api/disc.js`, from the same `DISC_URL` environment
variable; never in the repository), and the page passes that to the
emulator as the disc. One hop fewer a chunk, no function invocation a
chunk, and the function's 4.5 MB response limit stops bounding the
chunk size. The proxy stays for a browser that cannot (the bucket's
CORS rule gone, say); the local server answers the probe with 416 and
the page uses its range endpoint as before.

## 2b. The chunks kept in the browser (#119)

After #118 the player, on the hosted page: "still minor spikes", and
lag "when putting in a new character, or doing something new in
general". That is the first read of a file nobody had fetched: placing
a figure opens four files (the item's .arc and .bld, the character's
.arc of 4-7 MB and .bld of 0.7 MB), and each first chunk is a round
trip on the CPU thread. The fight shows them at t=1.25, 1.38, 1.47 and
2.17 s, 200-500 ms each.

**Tried and dropped: longer requests.** From curl, one 6 MB range costs
about what a 1 MB one does (0.43-0.65 s vs 0.39-0.65 s), which
suggested reading ahead to a file's end in one request. Built
(`--disc-span=N`: runs of chunks per request, to the end of the file a
read lands in, from the file table; two helpers), measured on the fight
in two alternating series of six: reads waited 3.5-3.9 s a run with
span 1, 4.0-6.0 s with span 8. Chrome is not curl: in the browser a
7 MB range took 0.7-2.9 s (5-10 MB/s), three parallel 1 MB ranges as
long as one, and the emulator's own fetches issued together finished
~220 ms apart. The browser's link to the store is bandwidth-bound once
the first byte is in, so every extra byte fetched ahead delays the
chunk the game is waiting for. The diff is not kept; this paragraph is
the record. It also undercuts plan item 1 below: more helpers or deeper
read-ahead cannot load a level faster than the link carries it.

**Built: web/sw.js**, a service worker between the emulator's threads
and the network. The page registers it (`sw.js?disc=<url>`) when the
disc is on another origin; it answers chunk-aligned range requests for
that URL from Cache Storage and keeps every chunk it fetches. The
emulator's reads are synchronous XHRs from its pthread workers, which
the worker controls like any fetch; no C++ changed. `?cache=0` turns
it off and unregisters it. The page logs `disc cache: on (N ms)`.

Measured with `web/smoke.mjs` against R2, headless Chrome:

| run | profile | page to first frame | reads waited (stalls) |
|---|---|---|---|
| first visit | fresh | 12 s | 3,250 ms (11) |
| second visit | the same | **2 s** | **38 ms** (8) |

A first visit pays nothing for it: fresh profiles alternating off/on/on/off
waited 2,816 / 2,593 / 1,924 / 1,441 ms and booted in 11-12 s alike;
registering costs 26 ms the first time, 1 ms after. 90 seconds of
boot and play kept 89 MB.

What it does not do: anything new on a first visit still costs its
round trips. A figure never placed on this browser, a level never
loaded, still stalls once. Prefetching the likely set in the
background (the 76 character files are 230 MB, the items 76 MB) would
cover that at the cost of the link's bandwidth during play; not built.

## 3. The plan: what remains for play from a host

In order. Each its own PR, measured, not merged until it is right.

1. **Loading screens.** Reconsidered (§2b): the browser's link to
   the store is bandwidth-bound, so more helpers do not help; the
   persistent cache makes a second load free. The plan as it was: The boot and each level load read tens of
   megabytes in order, in bursts; one helper fetching one chunk at a
   time caps that at a chunk per round trip (a megabyte per 300 ms,
   ~3 MB/s), and the game waits for the rest. Measure the time to the
   first frame and a level load from the page against the store
   (`web/smoke.mjs 90 <out> "disc=<url>&log=1"` and `&ahead=0`, from
   the readout log's first `readout` line) with read-ahead 2, 4 and
   two helpers fetching in parallel. Not measured yet: #118 was built
   and measured on the fight only.
2. **A persistent cache.** Done (#119, §2b); the text below is the
   plan as it was. A chunk fetched once should never be fetched
   again on that browser: the Cache API or OPFS, written by the helper
   as chunks arrive, read before the network on a miss. The fight
   touched 16 MB, a 400-second walk 121 MB; the game's working set is
   likely under a gigabyte, and Chrome's quota is a share of the disk.
   Emscripten's own IndexedDB fetch cache (`EMSCRIPTEN_FETCH_PERSIST_FILE`)
   is not usable from a pthread (Fetch.js skips its database on
   workers), so the page's main thread or the helper's JS would do it.
   The second session's boot then costs no round trips at all.
3. **The first visit's shader compiles.** With the disc out of the way,
   the hitches left in the fight are the GPU thread making pipelines
   (253 ms at t=4.67 s above, with the cached pipeline list loaded).
   `docs/HANDOFF-FIGHT-SPEED.md` item 1 and #107 cover what is known;
   a first visit on a browser whose Metal cache is cold is the case to
   measure, with Chrome for Testing, never the player's Chrome.
4. **The readout on the host.** The page posts its per-second readout
   to `/readout`, which the host does not have (404, then it stops).
   A function that appends to a store, or the player pasting the log
   (`?log=1`), is how the next report comes with numbers.

## 4. How to measure

- `DISC=<url> harness/wasm-core/run-jit.sh <steps> <flags>` runs any
  scene against a disc on the internet; the `disc window` line says
  how long the game's reads waited inside the window, and the hitch
  table's `disc` column which frames paid. Keep the URL out of the
  repository and out of committed logs: it is the game.
- A run's speed is only comparable with nothing else on the machine;
  the player's game running at the same time put both the local
  baseline and the R2 runs at 60-70% of real time. The disc waits are
  comparable regardless: they are the network's, not the CPU's.
- `web/smoke.mjs` with `disc=<url>` in the query boots the page itself
  against the store; the readout log in its output directory has the
  per-second lines, `disc N in M ms` included.
- The page's own log (`?log=1`) and readout are the player's evidence;
  the hosted page cannot read a disc served from localhost (Chrome
  blocks it), so a hosted test needs the store.

## 5. Landmines

- web/sw.js keys chunks by the disc's URL. The store's object name
  carries a hash of the image, so a new image is a new URL; replacing
  the object at the same URL would serve the old chunks forever on every
  browser that kept them. Change `STORE` in sw.js if that ever happens.
- The worker serves the page's origin, so it outlives a deploy that
  drops it: removing the cache means shipping a sw.js that unregisters,
  or the page's `?cache=0` path, not deleting the file.
- The page waits up to 5 s for the worker to take control on a first
  visit; if it never does (a browser with service workers disabled),
  the log says `not in control` and the disc reads go to the network.

- The helper is a pthread: one of the pool's workers
  (`-sPTHREAD_POOL_SIZE=8`, link.sh), created when the disc is opened.
  If the pool is ever full by then, the thread waits for the main
  browser thread to make a worker, which it can since nothing of ours
  runs there; a helper that never runs shows as `0 read ahead`.
- A fetch that fails (a 416 past the disc's end, a network error)
  leaves no chunk and the read comes back short, as before. The helper
  queues chunks past the end when the game reads the disc's last
  megabyte; each costs one failed request and nothing else.
- The read-ahead queue is replaced by every read, with that read's next
  chunks: two streams reading far apart take turns, and each gets its
  next chunk when the helper gets to it. Enough for a helper that
  finishes a chunk in a third of a second against reads every sixtieth.
- The `disc` counters in `SourceStats` are atomic now; `realdisc.cpp`
  reads them through the implicit conversion, which is fine until
  someone takes a reference.
