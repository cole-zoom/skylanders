// node run-validate.mjs: validate.html in headless Chrome, printed.
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const PORT = 8111, DEBUG = 9346;
const server = spawn(process.execPath, [join(here, '../threading/serve.mjs'), String(PORT)],
                     { cwd: here, stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 's2-'));
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
  '--headless', '--no-sandbox', `--remote-debugging-port=${DEBUG}`, '--enable-unsafe-webgpu',
  `--user-data-dir=${profile}`, `http://127.0.0.1:${PORT}/validate.html`], { stdio: 'ignore' });
try {
  let targets;
  for (let i = 0; i < 40 && !targets; i++) {
    try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json`)).json(); } catch { await sleep(250); }
  }
  const ws = new WebSocket(targets.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.onopen = r);
  let text = '';
  for (let i = 0; i < 600 && !text.includes('DONE'); i++) {
    await sleep(200);
    text = await new Promise(res => {
      ws.onmessage = ev => res(JSON.parse(ev.data).result?.result?.value ?? '');
      ws.send(JSON.stringify({ id: i + 1, method: 'Runtime.evaluate',
        params: { expression: "document.getElementById('out').textContent", returnByValue: true } }));
    });
  }
  console.log(text);
} finally { chrome.kill(); server.kill(); await sleep(300); rmSync(profile, { recursive: true, force: true }); }
