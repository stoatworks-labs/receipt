/**
 * The demo's CPU half (demo/printer.js) against the plugin's own C++.
 *
 *     demo/tools/check_port.sh          (finds node and c++, runs this)
 *     node demo/tools/check_port.mjs
 *
 * ------------------------------------------------------------ the reference
 *
 * refprint.cpp is compiled, with -ffp-contract=off as receipt_core is, against
 * source/Printer.cpp and source/Controls.cpp UNCHANGED, and against text this
 * script CUTS OUT of the plugin's source at run time:
 *
 *   from Receipt.h    the Stats struct and StatsForTest, the ParamID enum, and
 *                     the whole private section (every member), minus the one
 *                     line declaring nowSeconds(), which refprint defines to
 *                     return the script's clock in seconds
 *   from Receipt.cpp  floorDiv and floorMod; dataTones, printPaperRow and
 *                     resetPrinting; and the whole of ProcessOpenGL
 *
 * So the feed, the settings, the clock's dt, Static's window, Printing's ring
 * and tear, the uploads and every uniform the plugin hands its two passes come
 * from the plugin's own text, run against stand-ins for GL. A marker that is
 * not found fails the check rather than falling back to anything.
 *
 * ------------------------------------------------------------ what it compares
 *
 *   laws     every control law at 1,012 host values (0 .. 1 in thousandths and
 *            eleven outside or on a rounding edge), the integer hash at eight
 *            values, and SlipAt over 600,000 rows at three rates: exactly.
 *   rows     Printer alone, each dither at several heads: every row's fired bits,
 *            every dot's float density and every heater's double heat, exactly.
 *            The dither's carried error is private to Printer and is compared
 *            only through the bits it fires.
 *   frames   the page's Engine against the plugin's ProcessOpenGL, frame by
 *            frame through Static and Printing cases (below): rows printed, the
 *            receipt's start, the geometry, the image's size, which ring rows
 *            were uploaded, a 64-bit hash of the whole paper (the window, or the
 *            4,096-row ring) and every sample and display uniform, exactly; and
 *            at the end of each case the whole paper, dot for dot.
 *
 * ------------------------------------------------------------ what it cannot
 *
 * The tones are this script's, written to files both sides read: the sample
 * pass that makes them from a clip is the plugin's GLSL on a GPU and is not in
 * this check (check_shaders.py holds the page's copy of it to the plugin's).
 * refprint is not the plugin binary and has no GL: that the uniforms reach the
 * shaders, and that the page's GL calls in plugin.js match the plugin's, only a
 * reader checks. The cases are the cases below; agreement on them is evidence
 * about the port, not a proof of it.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');
const P = await import(join(REPO, 'demo', 'printer.js'));

let problems = 0;
const fail = (what) => {
  problems += 1;
  console.log(`FAIL  ${what}`);
};
const ok = (what) => console.log(`ok    ${what}`);

const scratch = mkdtempSync(join(tmpdir(), 'receipt-port-'));
process.on('exit', () => rmSync(scratch, { recursive: true, force: true }));

//---------------------------------------------------------------------------
// Cut the plugin's own text.
//---------------------------------------------------------------------------
function cut(text, start, end, what, { includeEnd = true } = {}) {
  const a = text.indexOf(start);
  if (a < 0) throw new Error(`${what}: start marker not found: ${JSON.stringify(start)}`);
  const b = text.indexOf(end, a + start.length);
  if (b < 0) throw new Error(`${what}: end marker not found: ${JSON.stringify(end)}`);
  return text.slice(a, includeEnd ? b + end.length : b);
}

const header = readFileSync(join(REPO, 'source', 'Receipt.h'), 'utf8');
const body = readFileSync(join(REPO, 'source', 'Receipt.cpp'), 'utf8');
let cutClass;
let cutDefs;
try {
  const stats = cut(header, '\t/// What the last frame cost', '\t\treturn stats;\n\t}\n', 'Stats');
  const ids = cut(header, '\t/// Everything the operator can reach', '\t};\n', 'ParamID');
  let members = cut(header, 'private:\n', '\n};', 'private section', { includeEnd: false });
  const nowDecl = /\n\t\/\/\/ The host's clock in seconds[^\n]*\n\tdouble nowSeconds\(\);\n/;
  if (!nowDecl.test(members)) throw new Error('private section: the nowSeconds() declaration is not where it was');
  members = members.replace(nowDecl, '\n');
  cutClass = `${stats}\n${ids}\n${members}\n`;

  const floors = cut(body, 'int64_t floorDiv( int64_t a, int64_t b )', 'return a - b * floorDiv( a, b );\n}\n', 'floorDiv/floorMod');
  const feed = cut(body, 'const float* Receipt::dataTones( int64_t dataRow )', '//---------------------------------------------------------------------------\nFFResult Receipt::ProcessOpenGL', 'feed', { includeEnd: false });
  const process = cut(body, 'FFResult Receipt::ProcessOpenGL( ProcessOpenGLStruct* pGL )', '\n}\n', 'ProcessOpenGL');
  // ProcessOpenGL's own closing brace is the first "\n}\n" at column 0 after it.
  cutDefs = `namespace\n{\n${floors}} // namespace\n\n${feed}\n${process}`;
} catch (error) {
  fail(`cutting the plugin's source: ${error.message}`);
  process.exit(1);
}
writeFileSync(join(scratch, 'cut_class.inc'), cutClass);
writeFileSync(join(scratch, 'cut_defs.inc'), cutDefs);

const refprint = join(scratch, 'refprint');
try {
  execFileSync('c++', [
    '-std=c++17', '-O2', '-ffp-contract=off', '-Wall',
    '-I', join(REPO, 'source'), '-I', scratch,
    '-o', refprint,
    join(HERE, 'refprint.cpp'), join(REPO, 'source', 'Printer.cpp'), join(REPO, 'source', 'Controls.cpp'),
  ], { stdio: ['ignore', 'pipe', 'pipe'] });
} catch (error) {
  console.log(String(error.stderr ?? error.message));
  fail('refprint did not build against the cut source');
  process.exit(1);
}
ok(`refprint built from Printer.cpp, Controls.cpp and ${cutDefs.split('\n').length + cutClass.split('\n').length} lines cut from Receipt.h and Receipt.cpp`);

const run = (...args) => execFileSync(refprint, args, { maxBuffer: 1 << 28 }).toString();

//---------------------------------------------------------------------------
// laws
//---------------------------------------------------------------------------
{
  const lines = run('laws').trim().split('\n');
  let laws = 0;
  let slips = 0;
  for (const line of lines) {
    const w = line.split(' ');
    if (w[0] === 'law') {
      laws += 1;
      const v = Number(w[1]);
      const c = P.controls;
      const js = [c.optionIndex(v, 2), c.optionIndex(v, 6), c.strobeBlocks(v), c.energy(v), c.carry(v), c.history(v),
        c.speedMmPerSecond(v), c.rowsPerSecond(v), c.slipRate(v), c.tearMm(v), c.age(v), c.tearRows(v)];
      const cpp = w.slice(2, 14).map(Number);
      for (let i = 0; i < js.length; i += 1) {
        if (!Object.is(js[i], cpp[i]) && js[i] !== cpp[i]) {
          fail(`law ${i} at v = ${w[1]}: js ${js[i]}, C++ ${cpp[i]}`);
          break;
        }
      }
    } else if (w[0] === 'hash') {
      const got = P.hashInt(Number(w[1]));
      if (got !== Number(w[2])) fail(`HashInt(${w[1]}): js ${got}, C++ ${w[2]}`);
    } else if (w[0] === 'slips') {
      slips += 1;
      const rate = Number(w[1]);
      let events = 0;
      let h = 0xcbf29ce484222325n;
      for (const row of [0, 4294967296, 4294967296 * 3 + 17]) {
        for (let k = 0; k < 200000; k += 1) {
          const s = P.slipAt(row + k, rate);
          if (s.kind === P.K_NO_SLIP) continue;
          events += 1;
          for (const word of [row + k, s.kind, s.rows]) {
            let x = BigInt(word);
            for (let b = 0; b < 8; b += 1) {
              h ^= x & 0xffn;
              h = (h * 0x100000001b3n) & 0xffffffffffffffffn;
              x >>= 8n;
            }
          }
        }
      }
      const hex = h.toString(16).padStart(16, '0');
      if (events !== Number(w[2]) || hex !== w[3]) fail(`SlipAt at rate ${w[1]}: js ${events} events ${hex}, C++ ${w[2]} ${w[3]}`);
    }
  }
  if (laws !== 1012 || slips !== 3) fail(`laws: ${laws} law lines and ${slips} slip lines from refprint`);
  else if (problems === 0) ok('laws: 12 control laws at 1,012 host values, HashInt at 8, SlipAt over 600,000 rows at 3 rates, all identical');
}

//---------------------------------------------------------------------------
// tones: this script's, written as float32 for both sides
//---------------------------------------------------------------------------
function tonesFor(N, rows, variant, k) {
  const out = new Float32Array(N * rows);
  for (let r = 0; r < rows; r += 1) {
    for (let x = 0; x < N; x += 1) {
      let t;
      if (variant === 'black') t = 0;
      else if (variant === 'white') t = 1;
      else if (variant === 'dark') t = 0.03 + 0.12 * (P.hashInt((x + r * 8192 + k * 7919) >>> 0) / 4294967296);
      else {
        const band = (r >> 4) % 8;
        if (band === 0) t = x / (N - 1);
        else if (band === 1) t = Math.floor(x / 9) % 3 === 0 ? 0 : 1;
        else if (band === 2) t = P.hashInt((x + r * 8192 + k * 7919) >>> 0) / 4294967296;
        else if (band === 3) t = [0.5, 0.25, 0.125, 1 / 3, 0.9, 0.05][Math.floor(x / 96) % 6];
        else if (band === 4) {
          const dx = x - (N / 2 + 60 * Math.sin(k));
          const dy = (r % 96) - 48;
          t = Math.min(1, Math.sqrt(dx * dx + dy * dy) / 120);
        } else if (band === 5) t = (x + r + k) % 64 < 32 ? 0.02 : 0.98;
        // Exactly on a Bayer threshold: 1 - t is ( M + 1/2 ) / 64 for some M,
        // so `>` and `>=` differ here and nowhere else.
        else if (band === 6) t = 1 - (2 * ((x * 5 + r * 3) % 64) + 1) / 128;
        // Exactly the supply's budget: 96 black dots in each half of the head,
        // so a block of two fires 96 and `<=` and `<` differ.
        else t = x % (N / 2) < 96 ? 0 : 1;
      }
      out[r * N + x] = t;
    }
  }
  return out;
}

const toneFiles = new Map();
function toneFile(N, rows, variant, k) {
  const key = `${N}-${rows}-${variant}-${k}`;
  if (!toneFiles.has(key)) {
    const path = join(scratch, `tones-${key}.f32`);
    const data = tonesFor(N, rows, variant, k);
    writeFileSync(path, Buffer.from(data.buffer));
    toneFiles.set(key, { path, data });
  }
  return toneFiles.get(key);
}

//---------------------------------------------------------------------------
// rows: Printer alone
//---------------------------------------------------------------------------
{
  const cases = [
    // dots rows dither blocks density carry history variant
    [576, 200, 0, 2, 1.0, 0.36, 0.5, 'card'],
    [576, 200, 1, 2, 1.0, 0.36, 0.5, 'card'],
    [576, 200, 2, 2, 1.0, 0.36, 0.5, 'card'],
    [384, 150, 1, 1, 1.5, 0.9 * Math.fround(0.9), 0.0, 'card'],
    [384, 150, 2, 8, 0.5, 0.0, 1.0, 'dark'],
    [576, 120, 1, 5, 1.2, 0.72, 0.3, 'dark'],
    [576, 64, 0, 7, 1.0, 0.81, 1.0, 'black'],
  ];
  let compared = 0;
  for (const [dots, rows, dither, blocks, energy, carry, history, variant] of cases) {
    const { path, data } = toneFile(dots, rows, variant, 1);
    const out = join(scratch, 'rows.bin');
    run('rows', path, String(dots), String(rows), String(dither), String(blocks), String(energy), String(carry), String(history), out);
    const cpp = readFileSync(out);
    const printer = new P.Printer();
    printer.configure({ dots, blocks, dither, energy, carry, history });
    const bits = new Uint8Array(dots);
    const density = new Float32Array(dots);
    const stride = dots * 13; // bits, float densities, double heat
    let bad = null;
    for (let r = 0; r < rows && !bad; r += 1) {
      printer.dither(data.subarray(r * dots, r * dots + dots), r, bits);
      printer.strobe(bits, density);
      const cb = cpp.subarray(r * stride, r * stride + dots);
      const cd = new Float32Array(cpp.buffer.slice(cpp.byteOffset + r * stride + dots, cpp.byteOffset + r * stride + dots * 5));
      const ch = new Float64Array(cpp.buffer.slice(cpp.byteOffset + r * stride + dots * 5, cpp.byteOffset + (r + 1) * stride));
      for (let x = 0; x < dots; x += 1) {
        if (bits[x] !== cb[x]) { bad = `row ${r} dot ${x}: bit js ${bits[x]}, C++ ${cb[x]}`; break; }
        if (!Object.is(density[x], cd[x])) { bad = `row ${r} dot ${x}: density js ${density[x]}, C++ ${cd[x]}`; break; }
        if (!Object.is(printer.theta[x], ch[x])) { bad = `row ${r} heater ${x}: heat js ${printer.theta[x]}, C++ ${ch[x]}`; break; }
        compared += 1;
      }
    }
    if (bad) fail(`rows (${P.DITHER_NAMES[dither]}, ${dots} dots, ${blocks} blocks, ${variant}): ${bad}`);
  }
  ok(`rows: ${compared.toLocaleString('en-GB')} dots over ${cases.length} runs of Printer, every fired bit, float density and double heat identical`);
}

//---------------------------------------------------------------------------
// frames
//---------------------------------------------------------------------------
const INDEX = Object.fromEntries(P.PARAM_IDS.map((id, i) => [id, i]));
const DEFAULTS = { width: 1, dither: 1, density: 0.5, heatCarry: 0.4, history: 0.5, strobeBlocks: 2, printSpeed: 0.4, slip: 0.15, mode: 0, tearLength: 0.4, age: 0.15, paperTint: 0, fit: 0, mix: 1 };

function paperHash(v) {
  const bytes = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
  let a = 2166136261;
  for (let i = 0; i < bytes.length; i += 1) a = Math.imul(a ^ bytes[i], 16777619) >>> 0;
  const words = new Uint32Array(v.buffer, v.byteOffset, v.length);
  let b = 0x9747b28c;
  for (let i = 0; i < words.length; i += 1) b = (Math.imul(b, 31) + words[i]) >>> 0;
  return a.toString(16).padStart(8, '0') + b.toString(16).padStart(8, '0');
}

const fmt = (u) => Object.keys(u).sort().map((k) => `${k}=${[].concat(u[k]).join(',')}`).join(' ');

/// A case: a list of steps, each { set } or { frame: [now, srcW, srcH, outW, outH], variant }.
function frames(fps, count, start = 0, size = [1280, 720, 1280, 720], variant = 'card') {
  const out = [];
  for (let i = 0; i < count; i += 1) out.push({ frame: [start + i / fps, ...size], variant });
  return out;
}

const CASES = {
  'Static, the defaults, 1280x720': [...frames(60, 3)],
  'Static, Bayer and Atkinson, 58 mm, 1920x1080': [
    { set: { dither: 0, width: 0 } }, ...frames(30, 2, 0, [1920, 1080, 1920, 1080]),
    { set: { dither: 2 } }, ...frames(30, 2, 1, [1920, 1080, 1920, 1080]),
  ],
  'Static, Rotate and Tile, 320x180 (the head cut off) and 3840x2160': [
    { set: { fit: 1 } }, ...frames(60, 2, 0, [320, 180, 320, 180]),
    { set: { fit: 2, tearLength: 1 } }, ...frames(60, 2, 0, [3840, 2160, 3840, 2160]),
    { set: { fit: 1, tearLength: 0 } }, ...frames(60, 2, 0, [640, 360, 640, 360]),
  ],
  'Static, heavy slips, hot head, every block count': [
    { set: { slip: 1, heatCarry: 1, history: 0, density: 1 } },
    ...[1, 2, 3, 4, 5, 6, 7, 8].flatMap((b) => [{ set: { strobeBlocks: b } }, ...frames(60, 1, b, [1280, 720, 1280, 720], b % 2 ? 'dark' : 'card')]),
  ],
  'Static, a source a different size from the output, Age and tints': [
    { set: { age: 1, paperTint: 2, mix: 0.5 } }, ...frames(60, 2, 0, [1920, 1080, 1280, 720]),
    { set: { age: 0.6, paperTint: 5, fit: 1 } }, ...frames(60, 2, 0, [720, 1280, 960, 540]),
  ],
  'Printing at 60 fps with the defaults': [{ set: { mode: 1 } }, ...frames(60, 240)],
  'Printing fast, short receipts, heavy slips, irregular frames': [
    { set: { mode: 1, printSpeed: 1, tearLength: 0.1, slip: 1, dither: 2 } },
    ...[0, 0.016, 0.05, 0.05, 0.4, 0.401, 0.401, 0.3, 1.0, 1.25, 1.26, 1.3, 1.9].map((now) => ({ frame: [now, 1280, 720, 1280, 720], variant: 'card' })),
    ...frames(50, 120, 2, [1280, 720, 1280, 720], 'dark'),
  ],
  'Printing past the ring (long receipt), then a resize, a head change and back': [
    { set: { mode: 1, printSpeed: 1, tearLength: 1, fit: 2, dither: 0 } }, ...frames(4, 30),
    ...frames(4, 4, 7.5, [1280, 720, 640, 360]),
    { set: { width: 0 } }, ...frames(4, 4, 8.5, [1920, 1080, 1920, 1080]),
    { set: { width: 1, mode: 0 } }, ...frames(4, 2, 9.5),
    { set: { mode: 1, printSpeed: 0.7, heatCarry: 0.9, history: 1 } }, ...frames(30, 30, 10),
    ...frames(30, 5, 3), // the clock going backwards: nothing prints until it passes
  ],
};

let frameCount = 0;
let dotCount = 0;
for (const [name, steps] of Object.entries(CASES)) {
  const params = { ...DEFAULTS };
  const engine = new P.Engine();
  const script = [];
  for (const id of P.PARAM_IDS) script.push(`set ${INDEX[id]} ${Math.fround(params[id])}`);
  const expected = [];
  for (const step of steps) {
    if (step.set) {
      Object.assign(params, step.set);
      for (const [id, v] of Object.entries(step.set)) script.push(`set ${INDEX[id]} ${Math.fround(v)}`);
      continue;
    }
    const [now, srcW, srcH, outW, outH] = step.frame;
    const geometry = engine.begin(params, now, srcW, srcH, outW, outH);
    const tones = toneFile(geometry.N, geometry.imageRows, step.variant, expected.length % 3);
    engine.imageTones.set(tones.data);
    const printed = engine.print();
    const paper = engine.mode === P.K_PRINTING ? engine.ring : engine.window;
    const uploads = printed.uploads.map(([y, h]) => `${y}+${h};`).join('');
    // The C++ Static branch uploads the whole window in one call.
    const ups = engine.mode === P.K_PRINTING ? uploads : `0+${geometry.windowRows};`;
    expected.push({
      line: `printed=${engine.printed} start=${engine.receiptStart} rows=${printed.rowsThisFrame} dots=${geometry.N} pitch=${geometry.pitch} x0=${geometry.x0} window=${geometry.windowRows} image=${geometry.imageRows} uploads=${ups} paper=${paper.length}:${paperHash(paper)}`,
      uniforms: `${fmt(P.sampleUniforms(geometry, srcW, srcH))} | ${fmt(P.displayUniforms(engine, printed, outW, outH, params.mix))}`,
      paper,
    });
    script.push(`frame ${now} ${srcW} ${srcH} ${outW} ${outH} ${tones.path}`);
  }
  script.push('dump final.f32');
  const scriptPath = join(scratch, 'script.txt');
  writeFileSync(scriptPath, `${script.join('\n')}\n`);
  const lines = run('frames', scriptPath, scratch).trim().split('\n');

  let bad = null;
  if (lines.length !== expected.length) bad = `${lines.length} frames from refprint, ${expected.length} here`;
  for (let i = 0; i < expected.length && !bad; i += 1) {
    const m = /^frame (\d+) result=(\d+) read=(\w+) (.*?) \| (.*?)\| (.*)$/.exec(lines[i]);
    if (!m) { bad = `frame ${i}: cannot read refprint's line: ${lines[i]}`; break; }
    if (m[2] !== '0' || m[3] !== 'ok') { bad = `frame ${i}: ProcessOpenGL returned ${m[2]}, read-back ${m[3]} (the image size differs)`; break; }
    if (m[4] !== expected[i].line) { bad = `frame ${i}:\n        C++: ${m[4]}\n        js : ${expected[i].line}`; break; }
    const cppUniforms = `${m[5].trim().split(' ').sort().join(' ')} | ${m[6].trim().split(' ').sort().join(' ')}`;
    // %.17g against JS's shortest round-trip: compare as numbers.
    const norm = (s) => s.replace(/=([-\d.e+,]+)/g, (_x, list) => `=${list.split(',').map((n) => String(Number(n))).join(',')}`);
    if (norm(cppUniforms) !== norm(expected[i].uniforms)) { bad = `frame ${i} uniforms:\n        C++: ${norm(cppUniforms)}\n        js : ${norm(expected[i].uniforms)}`; break; }
  }
  if (!bad) {
    const final = new Float32Array(readFileSync(join(scratch, 'final.f32')).buffer.slice(0));
    const mine = expected[expected.length - 1].paper;
    if (final.length !== mine.length) bad = `final paper: ${final.length} floats in C++, ${mine.length} here`;
    for (let i = 0; i < final.length && !bad; i += 1) {
      if (!Object.is(final[i], mine[i])) bad = `final paper row ${Math.floor(i / 576)} dot ${i % 576}: js ${mine[i]}, C++ ${final[i]}`;
    }
    dotCount += final.length;
  }
  frameCount += expected.length;
  if (bad) fail(`${name}: ${bad}`);
  else ok(`${name}: ${expected.length} frames identical`);
}

console.log();
if (problems) {
  console.log(`${problems} difference(s) between demo/printer.js and the plugin's C++`);
  process.exit(1);
}
console.log(`printer.js agrees with the plugin's C++ exactly: laws, Printer rows, and ${frameCount} frames of ProcessOpenGL (${dotCount.toLocaleString('en-GB')} final dots)`);
