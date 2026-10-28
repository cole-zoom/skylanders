// Serve the browser shell: the page, the built module, and the disc.
//
//   node web/serve.mjs [disc.iso] [port]
//
// The page posts its speed readout, one line a second, to /readout; they
// are appended to READOUT_LOG (default: skylanders-readout.log in the
// system's temp directory), so a play session can be looked at afterwards.
//
// One origin for all three, unlike the harness's two servers
// (harness/wasm-core/run-boot-browser.mjs): the page must be cross-origin
// isolated (COOP/COEP -- pthreads need SharedArrayBuffer), and a disc on the
// same origin needs no CORS or CORP to be loaded from it.
//
// The disc is served as ranges only, at /disc. DiscIO's HttpBlobReader asks
// for the ranges it reads and nothing more (docs/PLAN.md Bet 05); a server
// that answered a rangeless request with the whole image would turn the
// streaming client into a 4 GB download without anyone noticing, so that is
// refused, as in harness/wasm-core/serve-disc.mjs.
import { createServer } from 'node:http';
import { appendFileSync, createReadStream, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { readFile } from 'node:fs/promises';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const disc = process.argv[2] ?? '/tmp/ssa.iso';
const port = Number(process.argv[3] ?? 8080);
const root = dirname(fileURLToPath(import.meta.url));
const size = statSync(disc).size;
const readoutLog = process.env.READOUT_LOG ?? join(tmpdir(), 'skylanders-readout.log');

// .wasm must be application/wasm or the browser will not stream-compile it.
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
                '.wasm': 'application/wasm', '.css': 'text/css' };
const ISOLATED = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
};

function serveDisc(req, res) {
  const headers = { ...ISOLATED, 'Accept-Ranges': 'bytes' };
  const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '');
  if (!m) {
    res.writeHead(416, { ...headers, 'Content-Range': `bytes */${size}` })
       .end('this endpoint serves ranges only');
    return;
  }
  const start = Number(m[1]);
  const end = m[2] ? Math.min(Number(m[2]), size - 1) : size - 1;
  if (start >= size || end < start) {
    res.writeHead(416, { ...headers, 'Content-Range': `bytes */${size}` }).end();
    return;
  }
  res.writeHead(206, {
    ...headers,
    'Content-Range': `bytes ${start}-${end}/${size}`,
    'Content-Length': end - start + 1,
    'Content-Type': 'application/octet-stream',
  });
  createReadStream(disc, { start, end }).pipe(res);
}

createServer(async (req, res) => {
  const path = normalize(decodeURIComponent(req.url.split('?')[0]));
  if (path === '/disc') { serveDisc(req, res); return; }
  if (path === '/readout' && req.method === 'POST') {
    let body = '';
    req.setEncoding('utf8');
    req.on('data', chunk => { if (body.length < 1 << 20) body += chunk; });
    req.on('end', () => { appendFileSync(readoutLog, body); res.writeHead(204).end(); });
    return;
  }
  const file = join(root, path === '/' ? 'index.html' : path);
  if (!file.startsWith(root + '/')) { res.writeHead(403).end(); return; }
  try {
    const body = await readFile(file);
    res.writeHead(200, {
      ...ISOLATED,
      'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream',
      'Cache-Control': 'no-store',
    }).end(body);
  } catch { res.writeHead(404).end('not found'); }
}).listen(port, '127.0.0.1', () => {
  console.log(`http://127.0.0.1:${port}/  (disc ${disc}, ${(size / 1024 ** 3).toFixed(2)} GB)`);
  console.log(`speed readouts -> ${readoutLog}`);
});
