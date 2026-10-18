// Drive headless Chrome over the DevTools Protocol.
// --dump-dom captures at load, which is too early for anything async:
// WebGPU adapter/device requests are promises, so the page still says
// "running" when the DOM is dumped. CDP lets us await the real result.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const profile = mkdtempSync(join(tmpdir(), 'probe-'));
const url = `file://${process.cwd()}/${process.argv[2] ?? 'probe.html'}`;

const chrome = spawn(CHROME, [
  '--headless', '--no-sandbox', '--remote-debugging-port=9333',
  '--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU',
  `--user-data-dir=${profile}`, url,
], { stdio: 'ignore' });

// Chrome keeps writing to its profile for a moment after kill(); removing it
// immediately races and throws ENOTEMPTY. Give it a beat, and never let
// cleanup failure mask the actual result.
const cleanup = async () => {
  try { chrome.kill(); } catch {}
  await sleep(500);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
};

try {
  let targets;
  for (let i = 0; i < 40; i++) {           // wait for the debug endpoint
    try { targets = await (await fetch('http://127.0.0.1:9333/json')).json(); break; }
    catch { await sleep(250); }
  }
  const page = targets?.find(t => t.type === 'page');
  if (!page) throw new Error('no page target');

  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);

  const send = (id, method, params) => new Promise(res => {
    const h = (ev) => { const m = JSON.parse(ev.data);
      if (m.id === id) { ws.removeEventListener('message', h); res(m); } };
    ws.addEventListener('message', h);
    ws.send(JSON.stringify({ id, method, params }));
  });

  // Poll the page until its script has written a final result.
  for (let i = 0; i < 60; i++) {
    const r = await send(i + 1, 'Runtime.evaluate', {
      expression: "document.getElementById('out').textContent",
      returnByValue: true,
    });
    const text = r.result?.result?.value ?? '';
    if (text && text !== 'running') { console.log(text); break; }
    await sleep(500);
    if (i === 59) console.log('TIMEOUT — page never produced a result');
  }
  ws.close();
} finally { await cleanup(); }
