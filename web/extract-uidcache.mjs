// Pull the pipeline UID file out of a harness run's output.
//
//   node web/extract-uidcache.mjs <run output> [out dir]   (default web/dist)
//
// A run with --dump-uidcache prints "UIDCACHE <name> <bytes>", the file as
// base64 lines ("F ..."), then "UIDCACHE END". The page passes the file to
// the emulator with --uidcache-url, so Dolphin compiles every pipeline in it
// at startup instead of mid-game (see bootgame.cpp).
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const [log, outDir = join(dirname(fileURLToPath(import.meta.url)), 'dist')] = process.argv.slice(2);
if (!log) {
  console.error('usage: node web/extract-uidcache.mjs <run output> [out dir]');
  process.exit(2);
}
let name = null, size = 0, b64 = '';
for (const line of readFileSync(log, 'utf8').split('\n')) {
  const head = /^UIDCACHE (\S+) (\d+)$/.exec(line);
  if (head) { [, name, size] = head; size = Number(size); b64 = ''; continue; }
  if (line === 'UIDCACHE END' && name) break;
  if (name && line.startsWith('F ')) b64 += line.slice(2);
}
const bytes = Buffer.from(b64, 'base64');
if (!name || bytes.length !== size) {
  console.error(`no complete UID file in ${log} (got ${bytes.length} of ${size} bytes)`);
  process.exit(1);
}
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, name), bytes);
console.log(`${join(outDir, name)}: ${bytes.length} bytes`);
