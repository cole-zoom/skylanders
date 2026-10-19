// SharedArrayBuffer requires cross-origin isolation, which requires real
// headers — so this spike cannot run from file:// like the others.
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// Without both, SharedArrayBuffer is undefined and the whole threading model
// is unavailable. Serving them here is also the exact configuration the
// deployed app will need.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

// .wasm must be served as application/wasm or the browser refuses to stream-
// compile it and silently falls back to a slower ArrayBuffer path.
const TYPES = { '.html':'text/html', '.js':'text/javascript', '.mjs':'text/javascript',
                '.wasm':'application/wasm' };
const root = process.cwd();
const port = Number(process.argv[2] ?? 8099);

createServer(async (req, res) => {
  const path = normalize(decodeURIComponent(req.url.split('?')[0]));
  const file = join(root, path === '/' ? '/index.html' : path);
  if (!file.startsWith(root)) { res.writeHead(403).end(); return; }
  try {
    const body = await readFile(file);
    res.writeHead(200, {
      'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    }).end(body);
  } catch { res.writeHead(404).end('not found'); }
}).listen(port, () => console.log(`serving ${root} on :${port} (cross-origin isolated)`));
