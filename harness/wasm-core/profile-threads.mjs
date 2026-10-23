// A per-thread view of the profiles profile.mjs wrote.
//
// Under PROXY_TO_PTHREAD every thread is a worker and every worker gets its
// own .cpuprofile, so the emulation thread, the GPU thread (--dual-core) and
// the idle pool workers all land in the same directory. A summary over all
// of them is useless: the idle workers' futex waits drown everything else.
// This reads each profile, sets the waits aside, and reports every thread
// that did real work -- its self time by category and its hottest functions
// -- so a dual-core run shows the CPU thread and the GPU thread side by
// side, each with its busy share of the sampled wall time. The thread at
// ~100% busy is the one that bounds the run.
//
//   node harness/wasm-core/profile-threads.mjs <dir>
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

export function classify(name) {
  if (/^b_[0-9a-f]{8}$/.test(name)) return 'compiled blocks';
  if (/^(__timedwait_cp|emscripten_futex_wait|pthread_cond_wait|__pthread_cond_timedwait|emscripten_futex_wake|\(idle\)|\(program\)|\(root\)|\(garbage collector\))$/.test(name)) return 'idle/wait';
  // The recompiler itself: translating, encoding and installing modules.
  // Checked before the video and disc patterns, whose words ("hash",
  // "Fetch") appear in these names too.
  if (/ppc2wasm::|JitInstall|JitEntry|^Module$|^Instance$|WebAssembly|PPCTables::GetOpInfo|InstructionCache::ReadInstruction/.test(name)) return 'translation/install';
  if (/^Interpreter::|ppc_interp_op|NI_madd|NI_msub|^fma$|ForceSingle|Helper_|RunTable|Dequantize|Quantize|ConvertTo/.test(name)) return 'interpreter/hatch';
  if (/VertexLoader|TexDecoder|TextureCache|MurmurHash|Fifo|OpcodeDecoder|VertexManager|LoadBPReg|LoadCPReg|LoadXFReg|GPFifo|Pos_Read|TexCoord_Read|Color_Read|Normal_Read|SkipVertex|AbstractStagingTexture|EFB|XFB|Renderer|VideoBackend|BPFunctions|PixelEngine|CommandProcessor|SWVertexLoader|Rasterizer|Tev|GetHash|VertexShader|PixelShader|GeometryShader|ShaderCache|BoundingBox|FramebufferManager|Presenter/.test(name)) return 'video (VideoCommon)';
  if (/CoreTiming|SystemTimers|ProcessorInterface|Advance|ScheduleEvent|_main_thread|^main$|MSRUpdated|CheckExceptions|CheckExternalExceptions|Idle/.test(name)) return 'timing/dispatcher';
  if (/DSP|AX|Mixer|Audio|Sound|UCode|ProcessPBList/.test(name)) return 'audio (DSP HLE)';
  if (/MMU|WriteToHardware|ReadFromHardware|Memory|memcpy|memmove|memset|memcmp/.test(name)) return 'memory/MMU';
  if (/DVD|Disc|Volume|Blob|fetch|XHR|http|send|Fetch|IOS|FS|WiiRoot|Boot/.test(name)) return 'disc/IOS';
  return 'other';
}

export function threads(dir) {
  const out = [];
  for (const f of readdirSync(dir).filter(f => f.endsWith('.cpuprofile')).sort()) {
    const p = JSON.parse(readFileSync(join(dir, f), 'utf8'));
    const byId = new Map(p.nodes.map(n => [n.id, n]));
    const cat = new Map(), fn = new Map();
    let total = 0, busy = 0;
    for (let k = 0; k < p.samples.length; k++) {
      const n = byId.get(p.samples[k]);
      const us = p.timeDeltas[k] || 0;
      total += us;
      const name = n.callFrame.functionName || '(anonymous)';
      const c = classify(name);
      if (c !== 'idle/wait') busy += us;
      cat.set(c, (cat.get(c) || 0) + us);
      const key = c + ' | ' + name;
      fn.set(key, (fn.get(key) || 0) + us);
    }
    // startTime/endTime are microseconds; the deltas should sum to about
    // the same, but the wall span is what "busy %" is measured against.
    const wall = Math.max(p.endTime - p.startTime, total, 1);
    out.push({ file: f, total, busy, wall, cat, fn });
  }
  return out;
}

export function report(dir) {
  // A pool worker that never ran anything still shows a few milliseconds of
  // runtime; half a second of real work is the bar for being a thread worth
  // printing.
  const ts = threads(dir).filter(t => t.busy > 0.5e6).sort((a, b) => b.busy - a.busy);
  const s = (us) => (us / 1e6).toFixed(1).padStart(7) + ' s';
  let out = '';
  for (const t of ts) {
    const blocks = t.cat.get('compiled blocks') || 0;
    const video = t.cat.get('video (VideoCommon)') || 0;
    const label = blocks > video ? 'CPU thread' : video > 0 ? 'GPU thread' : 'thread';
    out += `${label}  (${t.file}): busy ${(t.busy / 1e6).toFixed(1)} s of ${(t.wall / 1e6).toFixed(1)} s sampled, ${(100 * t.busy / t.wall).toFixed(0)}%\n`;
    for (const [c, us] of [...t.cat.entries()].sort((a, b) => b[1] - a[1]))
      out += `  ${s(us)}  ${(100 * us / t.wall).toFixed(1).padStart(5)}%  ${c}\n`;
    const fns = [...t.fn.entries()].sort((a, b) => b[1] - a[1]);
    out += '  hottest functions, compiled blocks aside:\n';
    for (const [k, us] of fns.filter(([k]) => !k.startsWith('compiled blocks') && !k.startsWith('idle/wait')).slice(0, 24))
      out += `  ${s(us)}  ${(100 * us / t.wall).toFixed(1).padStart(5)}%  ${k}\n`;
    const bl = fns.filter(([k]) => k.startsWith('compiled blocks'));
    if (bl.length) {
      const bt = bl.reduce((a, [, us]) => a + us, 0);
      let cum = 0;
      out += `  compiled blocks: ${bl.length} sampled;`;
      bl.forEach(([, us], i) => { cum += us; if ([10, 100, 500].includes(i + 1)) out += ` top ${i + 1} = ${(100 * cum / bt).toFixed(1)}%`; });
      out += ' of block time\n';
      for (const [k, us] of bl.slice(0, 16))
        out += `  ${s(us)}  ${(100 * us / t.wall).toFixed(1).padStart(5)}%  ${k.replace('compiled blocks | ', '')}\n`;
    }
    out += '\n';
  }
  return out || 'no thread did more than half a second of work\n';
}

if (process.argv[1] && import.meta.url === new URL(`file://${process.argv[1]}`).href)
  process.stdout.write(report(process.argv[2] ?? '.'));
