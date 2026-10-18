// Inline exported geometry into the replayer page.
// The page must be self-contained: it is opened as file:// in headless Chrome,
// where fetch() of a sibling JSON is blocked by the file:// origin policy.
import { readFileSync, writeFileSync } from 'node:fs';
const geo = readFileSync(process.argv[2] ?? '../../harness/traces/ssa-title-draws.json', 'utf8');
const tpl = readFileSync('replay-template.html', 'utf8');
writeFileSync('replay.html', tpl.replace('/*GEOMETRY*/', geo));
console.log(`replay.html built with ${(geo.length / 1024).toFixed(0)} KB of geometry`);
