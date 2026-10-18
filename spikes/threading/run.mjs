// Serve the spike with COOP/COEP, drive Chrome over CDP, print the result.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const PORT = 8099;
const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const server = spawn(process.execPath, ['serve.mjs', String(PORT)], { stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 'spikec-'));
let chrome;

const cleanup = async () => {
  try { chrome?.kill(); } catch {}
  try { server.kill(); } catch {}
  await sleep(400);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
};

try {
  await sleep(600);
  chrome = spawn(CHROME, [
    '--headless', '--no-sandbox', '--remote-debugging-port=9334',
    '--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU,SharedArrayBuffer',
    '--autoplay-policy=no-user-gesture-required',
    `--user-data-dir=${profile}`, `http://127.0.0.1:${PORT}/index.html`,
  ], { stdio: 'ignore' });

  let targets;
  for (let i = 0; i < 60; i++) {
    try { targets = await (await fetch('http://127.0.0.1:9334/json')).json(); break; }
    catch { await sleep(250); }
  }
  const page = targets?.find(t => t.type === 'page');
  if (!page) throw new Error('no page target');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  const send = (id, method, params) => new Promise(res => {
    const h = ev => { const m = JSON.parse(ev.data);
      if (m.id === id) { ws.removeEventListener('message', h); res(m); } };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id, method, params }));
  });

  for (let i = 0; i < 180; i++) {
    const r = await send(i + 1, 'Runtime.evaluate', {
      expression: "document.getElementById('out').textContent", returnByValue: true });
    const t = r.result?.result?.value ?? '';
    if (/GATE MET|GATE NOT MET|ERROR/.test(t)) { console.log(t); break; }
    await sleep(500);
    if (i === 179) console.log('TIMEOUT\n' + t);
  }
  ws.close();
} finally { await cleanup(); }
