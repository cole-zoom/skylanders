// Serve a disc image with HTTP Range support.
//
// This is the server side of docs/PLAN.md Bet 05. The browser never downloads
// the 4.5 GB image; it asks for the ranges DiscIO wants. A server that ignores
// Range would return the whole file for every request -- which still "works"
// and silently defeats the entire point -- so this answers 206 Partial
// Content and the client refuses anything else.
import { createServer } from 'node:http';
import { createReadStream, statSync } from 'node:fs';

const file = process.argv[2] ?? '/tmp/ssa.iso';
const port = Number(process.argv[3] ?? 8101);
const size = statSync(file).size;

createServer((req, res) => {
  const headers = {
    // Required: the wasm module is served from a different origin than this,
    // and a cross-origin-isolated page can only load what opts in.
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Range',
    'Access-Control-Expose-Headers': 'Content-Range, Content-Length',
    'Cross-Origin-Resource-Policy': 'cross-origin',
    'Accept-Ranges': 'bytes',
  };

  if (req.method === 'OPTIONS') { res.writeHead(204, headers).end(); return; }

  const range = req.headers.range;
  if (!range) {
    // Deliberately refused. Serving the whole image on a rangeless request is
    // how a streaming client quietly becomes a downloading one.
    res.writeHead(416, { ...headers, 'Content-Range': `bytes */${size}` })
       .end('this endpoint serves ranges only');
    return;
  }

  const m = /^bytes=(\d+)-(\d*)$/.exec(range);
  if (!m) { res.writeHead(400, headers).end('bad range'); return; }
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
  createReadStream(file, { start, end }).pipe(res);
}).listen(port, () => {
  console.log(`serving ${file} (${(size / 1024 ** 3).toFixed(2)} GB) on :${port}, ranges only`);
});
