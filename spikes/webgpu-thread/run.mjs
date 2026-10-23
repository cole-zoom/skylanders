// Run spike S1 in headless Chrome and report what it printed, plus the
// colour actually on screen at the canvas centre each time it says SAMPLE.
//
//   node run.mjs
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const PORT = 8110, DEBUG = 9345;
const server = spawn(process.execPath, [join(here, '../threading/serve.mjs'), String(PORT)],
                     { cwd: here, stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 's1-'));
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
  '--headless', '--no-sandbox', `--remote-debugging-port=${DEBUG}`,
  '--enable-unsafe-webgpu', '--enable-features=WebGPU', '--window-size=700,800',
  `--user-data-dir=${profile}`, `http://127.0.0.1:${PORT}/index.html`,
], { stdio: 'ignore' });

// The one pixel of a 1x1 PNG screenshot: inflate IDAT, skip the filter byte.
function pixel(b64) {
  const png = Buffer.from(b64, 'base64');
  let off = 8, idat = [];
  while (off < png.length) {
    const len = png.readUInt32BE(off), type = png.toString('ascii', off + 4, off + 8);
    if (type === 'IDAT') idat.push(png.subarray(off + 8, off + 8 + len));
    off += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idat));
  return [raw[1], raw[2], raw[3]];
}

try {
  let targets;
  for (let i = 0; i < 40 && !targets; i++) {
    try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json`)).json(); }
    catch { await sleep(250); }
  }
  const page = targets.find(t => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let id = 0;
  const send = (method, params) => new Promise(res => {
    const my = ++id;
    const h = ev => { const m = JSON.parse(ev.data);
      if (m.id === my) { ws.removeEventListener('message', h); res(m); } };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id: my, method, params }));
  });
  const sampled = new Set();
  let text = '';
  for (let i = 0; i < 300 && !text.includes('DONE'); i++) {
    const r = await send('Runtime.evaluate',
                         { expression: "document.getElementById('out').textContent",
                           returnByValue: true });
    text = r.result?.result?.value ?? '';
    for (const tag of ['SAMPLE red', 'SAMPLE green', 'SAMPLE blue', 'SAMPLE yellow']) {
      if (text.includes(tag) && !sampled.has(tag)) {
        sampled.add(tag);
        const shot = await send('Page.captureScreenshot',
                                { format: 'png', clip: { x: 320, y: 264, width: 1, height: 1, scale: 1 } });
        console.log(`  on screen at ${tag}: rgb(${pixel(shot.result.data).join(',')})`);
      }
    }
    await sleep(100);
  }
  console.log(text);
} finally {
  chrome.kill(); server.kill();
  await sleep(300);
  rmSync(profile, { recursive: true, force: true });
}
