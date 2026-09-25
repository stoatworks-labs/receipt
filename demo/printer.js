/**
 * Receipt's CPU half, PORTED to JavaScript: the print engine and the feed.
 *
 * The browser cannot run the plugin's C++, and the print is the CPU's in the
 * plugin (the sample pass on the GPU is read back, `Printer.cpp` dithers,
 * applies history control, the supply's budget, the heat and the paper's
 * response in double, row by row, and the display pass draws the dots). So
 * this file is a hand translation of, function for function:
 *
 *   source/Model.h        the constants, the Bayer matrix, the hash, SlipAt
 *   source/Controls.cpp   every slider to its unit
 *   source/Printer.cpp    Dither and Strobe
 *   source/Receipt.cpp    dataTones, printPaperRow, resetPrinting, and the
 *                         CPU part of ProcessOpenGL: the settings, the clock's
 *                         dt, the image's size and offset, Static's window
 *                         and Printing's ring and tear
 *
 * JavaScript numbers are IEEE doubles, like the engine's; the order of every
 * operation is kept, and JavaScript has no fused multiply-add, which is what
 * the plugin's `-ffp-contract=off` asks of the compiler. Where the C++ stores
 * a float (a parameter, a tone, a density), this stores one too, through
 * Float32Array or Math.fround.
 *
 * `demo/tools/check_port.sh` compiles the plugin's own Printer.cpp and
 * Controls.cpp, and the feed CUT OUT OF Receipt.cpp's text, into a driver and
 * compares this file's fired bits and densities with it exactly. It says what
 * it covers. What is not ported: the Perturb test hooks and the forced slip
 * (always off in the plugin), the host clock's unit vote (the page declares
 * seconds), and everything GL.
 *
 * No DOM and no GL here, so node can import it.
 */

//===========================================================================
// Model.h
//===========================================================================

export const K_DOTS_PER_MM = 8.0;
export const WIDTH_NAMES = ['58 mm', '80 mm'];
export const WIDTH_DOTS = [384, 576];
export const K_BUDGET_DOTS = 96.0;
export const K_SPREAD = 0.22;
export const K_THRESHOLD = 0.35;
export const K_SATURATION = 1.0;
export const K_MAX_OD = 1.3;
export const K_STROBE_BLOCKS_MIN = 1;
export const K_STROBE_BLOCKS_MAX = 8;

export const K_BAYER = 0;
export const K_FLOYD_STEINBERG = 1;
export const K_ATKINSON = 2;
export const DITHER_NAMES = ['Bayer', 'Floyd-Steinberg', 'Atkinson'];

export const BAYER8 = [
  [0, 32, 8, 40, 2, 34, 10, 42],
  [48, 16, 56, 24, 50, 18, 58, 26],
  [12, 44, 4, 36, 14, 46, 6, 38],
  [60, 28, 52, 20, 62, 30, 54, 22],
  [3, 35, 11, 43, 1, 33, 9, 41],
  [51, 19, 59, 27, 49, 17, 57, 25],
  [15, 47, 7, 39, 13, 45, 5, 37],
  [63, 31, 55, 23, 61, 29, 53, 21],
];

export const K_STATIC = 0;
export const K_PRINTING = 1;
export const MODE_NAMES = ['Static', 'Printing'];

export const K_MAX_SLIP_ROWS = 4;
export const K_NO_SLIP = 0;
export const K_REPEAT = 1;
export const K_STALL = 2;

export const K_PAPER_ROWS = 4096;
export const K_LEAD_ROWS = 24;
export const K_MAX_FRAME_DELTA = 0.25;

export const K_LETTERBOX = 0;
export const K_ROTATE = 1;
export const K_TILE = 2;
export const FIT_NAMES = ['Letterbox', 'Rotate', 'Tile'];

export const TINT_NAMES = ['White', 'Ivory', 'Canary', 'Pink', 'Blue', 'Green'];
// `inline constexpr float` in the C++: each is a float, so each is rounded to
// one here before anything is computed from it.
const f = Math.fround;
export const TINT_RGB = [
  [f(0.955), f(0.955), f(0.945)],
  [f(0.965), f(0.935), f(0.860)],
  [f(0.975), f(0.925), f(0.620)],
  [f(0.975), f(0.840), f(0.875)],
  [f(0.800), f(0.890), f(0.965)],
  [f(0.820), f(0.945), f(0.830)],
];
export const INK_FRESH = [f(1.00), f(0.97), f(0.90)];
export const INK_AGED = [f(0.62), f(0.72), f(0.95)];
export const AGED_STOCK = [f(1.00), f(0.93), f(0.76)];

export const K_AGE_FADE = 0.75;
export const K_AGE_FADE_ROWS = 40.0 * K_DOTS_PER_MM;
export const K_AGE_HELD_OD = 0.18;
export const K_AGE_HELD_ROWS = 15.0 * K_DOTS_PER_MM;

/// C++ integer division of non-negative ints (truncation).
const idiv = (a, b) => Math.trunc(a / b);

export function pitchOf(outW, dots) {
  const p = idiv(outW, dots);
  return p < 1 ? 1 : p;
}

export function stripX0(outW, dots, pitch) {
  const spare = outW - dots * pitch;
  return spare >= 0 ? idiv(spare, 2) : -idiv(-spare + 1, 2);
}

export function windowRowsOf(outH, pitch) {
  return idiv(outH + pitch - 1, pitch);
}

/// The fleet's integer hash, in 32-bit unsigned arithmetic: Math.imul wraps
/// as uint32_t multiplication does, and >>> 0 is the unsigned view.
export function hashInt(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/// SlipAt: `row` is an integer (an int64_t in the C++; every row here is well
/// inside 2^53). The low and high words are taken as the C++ casts take them.
export function slipAt(row, rate) {
  const s = { kind: K_NO_SLIP, rows: 0 };
  if (rate <= 0.0) return s;
  const lo = row >>> 0;
  const hi = Math.floor(row / 4294967296) >>> 0;
  const key = (lo ^ Math.imul(hi, 0x9e3779b9)) >>> 0;
  const h = hashInt((key ^ 0x51f15eed) >>> 0);
  const u = (h >>> 8) * (1.0 / 16777216.0);
  if (u >= rate) return s;
  const g = hashInt(h);
  s.kind = g & 1 ? K_STALL : K_REPEAT;
  s.rows = 1 + ((g >>> 1) % K_MAX_SLIP_ROWS);
  return s;
}

//===========================================================================
// Controls.cpp. Every argument is the host's FLOAT: it is rounded to one
// first, as the plugin's params[] store it.
//===========================================================================

const clamp = (v, lo, hi) => (v < lo ? lo : hi < v ? hi : v);
/// std::lround: halves away from zero.
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
const unit = (value) => clamp(Math.fround(value), 0.0, 1.0);

export const controls = {
  optionIndex: (value, count) => clamp(lround(Math.fround(value)), 0, count - 1),
  strobeBlocks: (value) => clamp(lround(Math.fround(value)), K_STROBE_BLOCKS_MIN, K_STROBE_BLOCKS_MAX),
  energy: (value) => 0.5 + unit(value),
  carry: (value) => 0.9 * unit(value),
  history: (value) => unit(value),
  speedMmPerSecond: (value) => {
    const v = unit(value);
    return 2.0 + 248.0 * v * v;
  },
  rowsPerSecond: (value) => K_DOTS_PER_MM * controls.speedMmPerSecond(value),
  slipRate: (value) => {
    const v = unit(value);
    return 0.02 * v * v;
  },
  tearMm: (value) => {
    const v = unit(value);
    return 30.0 + 570.0 * v * v;
  },
  tearRows: (value) => Math.max(1, lround(K_DOTS_PER_MM * controls.tearMm(value))),
  age: (value) => unit(value),
};

//===========================================================================
// Printer.cpp
//===========================================================================

const K_PAD = 2;

export class Printer {
  constructor() {
    this.settings = { dots: 576, blocks: 4, dither: 1, energy: 1.0, carry: 0.0, history: 0.0 };
    this.theta = new Float64Array(0);
    this.energyRow = new Float64Array(0);
    this.error = [new Float64Array(0), new Float64Array(0), new Float64Array(0)];
    this.errorRow = 0;
  }

  configure(s) {
    const newHead = s.dots !== this.settings.dots || this.theta.length !== s.dots;
    this.settings = { ...s };
    if (!newHead) return;
    this.theta = new Float64Array(s.dots);
    this.energyRow = new Float64Array(s.dots);
    this.error = [0, 1, 2].map(() => new Float64Array(s.dots + 2 * K_PAD));
    this.errorRow = 0;
  }

  resetHeat() {
    this.theta.fill(0.0);
  }

  resetDither() {
    for (const row of this.error) row.fill(0.0);
    this.errorRow = 0;
  }

  /// tones: a Float32Array view of the row; bits: a Uint8Array.
  dither(tones, dataRow, bits) {
    const N = this.settings.dots;

    if (this.settings.dither === K_BAYER) {
      const phase = ((dataRow % 8) + 8) % 8;
      const m = BAYER8[phase];
      for (let x = 0; x < N; x += 1) {
        const dark = 1.0 - tones[x];
        const threshold = (m[x & 7] + 0.5) / 64.0;
        bits[x] = dark > threshold ? 1 : 0;
      }
      return;
    }

    // Offsets by K_PAD stand for the C++'s `data() + kPad` pointers.
    const here = this.error[this.errorRow];
    const next = this.error[(this.errorRow + 1) % 3];
    const after = this.error[(this.errorRow + 2) % 3];
    const fs = this.settings.dither === K_FLOYD_STEINBERG;

    for (let x = 0; x < N; x += 1) {
      const i = x + K_PAD;
      const v = (1.0 - tones[x]) + here[i];
      const b = v >= 0.5 ? 1 : 0;
      bits[x] = b;
      const e = v - b;
      if (fs) {
        here[i + 1] += e * (7.0 / 16.0);
        next[i - 1] += e * (3.0 / 16.0);
        next[i] += e * (5.0 / 16.0);
        next[i + 1] += e * (1.0 / 16.0);
      } else {
        const eighth = e * 0.125;
        here[i + 1] += eighth;
        here[i + 2] += eighth;
        next[i - 1] += eighth;
        next[i] += eighth;
        next[i + 1] += eighth;
        after[i] += eighth;
      }
    }

    // This row's buffer is spent: it becomes the one after next.
    here.fill(0.0);
    this.errorRow = (this.errorRow + 1) % 3;
  }

  /// density: a Float32Array view of the paper row (the C++ writes floats).
  strobe(bits, density) {
    const N = this.settings.dots;
    const c = this.settings.carry;
    const e0 = this.settings.energy;
    const h = this.settings.history;
    const blocks = clamp(this.settings.blocks, 1, N);
    const theta = this.theta;
    const energy = this.energyRow;

    for (let x = 0; x < N; x += 1) energy[x] = bits[x] ? Math.max(0.0, e0 - h * c * theta[x]) : 0.0;

    for (let k = 0; k < blocks; k += 1) {
      const x0 = idiv(k * N, blocks);
      const x1 = idiv((k + 1) * N, blocks);
      let firing = 0;
      for (let x = x0; x < x1; x += 1) firing += bits[x];
      if (firing <= K_BUDGET_DOTS) continue;
      const scale = K_BUDGET_DOTS / firing;
      for (let x = x0; x < x1; x += 1) energy[x] *= scale;
    }

    for (let x = 0; x < N; x += 1) theta[x] = c * theta[x] + energy[x];

    const span = K_SATURATION - K_THRESHOLD;
    for (let x = 0; x < N; x += 1) {
      const left = x > 0 ? theta[x - 1] : 0.0;
      const right = x + 1 < N ? theta[x + 1] : 0.0;
      const q = theta[x] + K_SPREAD * (left + right);
      const t = clamp((q - K_THRESHOLD) / span, 0.0, 1.0);
      density[x] = t * t * (3.0 - 2.0 * t); // a Float32Array store rounds as static_cast< float > does
    }
  }
}

//===========================================================================
// Receipt.cpp's CPU half: everything ProcessOpenGL does that is not GL.
//===========================================================================

const floorDiv = (a, b) => Math.floor(a / b);
const floorMod = (a, b) => a - b * floorDiv(a, b);

/// The parameter ids, in Receipt::ParamID order; the values are the host's.
export const PARAM_IDS = [
  'width', 'dither', 'density', 'heatCarry', 'history', 'strobeBlocks',
  'printSpeed', 'slip', 'mode', 'tearLength',
  'age', 'paperTint', 'fit', 'mix',
];

export class Engine {
  constructor() {
    this.printer = new Printer();
    this.imageTones = new Float32Array(0);
    this.whiteRow = new Float32Array(0);
    this.bits = new Uint8Array(0);
    this.extraBits = new Uint8Array(0);
    this.strike = new Float32Array(0);
    this.imageRows = 0;
    this.imageOffset = 0;
    this.fit = 0;

    this.ring = null;
    this.ringDots = 0;
    this.printed = 0;
    this.receiptStart = 0;
    this.uploadedTo = 0;
    this.pendingRows = 0.0;
    this.printingFeed = newFeed();
    this.wasPrinting = false;

    this.window = null;
    this.staticRows = 0;
    this.staticDots = 0;

    this.lastNow = -1.0;
    this.slipRate = 0.0;
  }

  /// Part one of a frame: the clock, the settings and the geometry, and every
  /// buffer sized. `p` maps PARAM_IDS to the host's values; `now` is the
  /// frame's time in seconds. Returns what the GL half needs before the
  /// sample pass, and leaves `imageTones` ready for the read-back
  /// (N x imageRows floats, row 0 printed first).
  begin(p, now, srcW, srcH, outW, outH) {
    let dt = 0.0; // the first frame primes the clock and prints nothing
    if (this.lastNow >= 0.0) dt = clamp(now - this.lastNow, 0.0, K_MAX_FRAME_DELTA);
    this.lastNow = now;
    this.dt = dt;

    const widthOption = controls.optionIndex(p.width, WIDTH_DOTS.length);
    const N = WIDTH_DOTS[widthOption];
    this.printer.configure({
      dots: N,
      blocks: controls.strobeBlocks(p.strobeBlocks),
      dither: controls.optionIndex(p.dither, DITHER_NAMES.length),
      energy: controls.energy(p.density),
      carry: controls.carry(p.heatCarry),
      history: controls.history(p.history),
    });

    this.rowsPerSecond = controls.rowsPerSecond(p.printSpeed);
    this.slipRate = controls.slipRate(p.slip);
    this.mode = controls.optionIndex(p.mode, MODE_NAMES.length);
    this.tearRows = controls.tearRows(p.tearLength);
    this.age = controls.age(p.age);
    this.tint = controls.optionIndex(p.paperTint, TINT_NAMES.length);
    this.fit = controls.optionIndex(p.fit, FIT_NAMES.length);

    const pitch = pitchOf(outW, N);
    const x0 = stripX0(outW, N, pitch);
    const windowRows = Math.min(windowRowsOf(outH, pitch), K_PAPER_ROWS);

    const rotate = this.fit === K_ROTATE;
    const unitLength = rotate ? srcH : srcW;
    const along = rotate ? srcW : srcH;
    this.imageRows = Math.min(8192, idiv(along * N + unitLength - 1, unitLength));
    this.imageOffset = this.mode === K_PRINTING ? K_LEAD_ROWS : floorDiv(this.tearRows - this.imageRows, 2);

    this.whiteRow = new Float32Array(N).fill(1.0);
    if (this.bits.length !== N) {
      this.bits = new Uint8Array(N);
      this.extraBits = new Uint8Array(N);
      this.strike = new Float32Array(N);
    }
    if (this.imageTones.length !== N * this.imageRows) this.imageTones = new Float32Array(N * this.imageRows);

    this.newRing = false;
    this.newWindow = false;
    if (this.mode === K_PRINTING) {
      if (this.ring === null || this.ringDots !== N) {
        // A new head: new paper. A change of OUTPUT size is not one.
        this.ring = new Float32Array(N * K_PAPER_ROWS);
        this.ringDots = N;
        this.uploadedTo = this.printed;
        this.wasPrinting = false;
        this.newRing = true;
      }
    } else if (this.window === null || this.staticRows !== windowRows || this.staticDots !== N) {
      this.window = new Float32Array(N * windowRows);
      this.staticRows = windowRows;
      this.staticDots = N;
      this.newWindow = true;
    }

    this.N = N;
    this.pitch = pitch;
    this.x0 = x0;
    this.windowRows = windowRows;
    return { N, pitch, x0, windowRows, imageRows: this.imageRows, unit: unitLength, rotate };
  }

  dataTones(dataRow) {
    const N = this.N;
    if (this.imageRows <= 0) return this.whiteRow;
    let row = dataRow - this.imageOffset;
    if (this.fit === K_TILE) row = floorMod(row, this.imageRows);
    if (row < 0 || row >= this.imageRows) return this.whiteRow;
    return this.imageTones.subarray(row * N, row * N + N);
  }

  printPaperRow(slipKey, feed, density) {
    const N = this.N;
    const printer = this.printer;

    // A repeat in progress: the line counter is stuck, the paper moves on.
    if (feed.repeatLeft > 0) {
      printer.strobe(feed.lastBits, density);
      feed.repeatLeft -= 1;
      return;
    }

    const slip = slipAt(slipKey, this.slipRate);

    printer.dither(this.dataTones(feed.dataRow), feed.dataRow, this.bits);
    feed.dataRow += 1;
    printer.strobe(this.bits, density);

    if (slip.kind === K_REPEAT && slip.rows > 0) {
      feed.lastBits = this.bits.slice(0, N);
      feed.repeatLeft = slip.rows;
    } else if (slip.kind === K_STALL) {
      // The paper sits still while the data moves on: the dye keeps the darkest.
      for (let k = 0; k < slip.rows; k += 1) {
        printer.dither(this.dataTones(feed.dataRow), feed.dataRow, this.extraBits);
        feed.dataRow += 1;
        printer.strobe(this.extraBits, this.strike);
        for (let x = 0; x < N; x += 1) density[x] = Math.max(density[x], this.strike[x]);
      }
    }
  }

  resetPrinting() {
    this.receiptStart = this.printed;
    this.printingFeed = newFeed();
    this.printer.resetDither();
  }

  /// Part two, after `imageTones` holds the read-back: the print. Returns the
  /// display pass's row uniforms and which rows to upload.
  print() {
    const N = this.N;
    const windowRows = this.windowRows;
    const tearRows = this.tearRows;
    let receiptTop = 0;
    let receiptBottom = windowRows;
    let rowBase = 0;
    let texRows = windowRows;
    let notchBottom = true;
    let rowsThisFrame = 0;

    if (this.mode === K_PRINTING) {
      if (!this.wasPrinting) this.resetPrinting(); // a new receipt out of the slot
      this.wasPrinting = true;

      this.pendingRows += this.rowsPerSecond * this.dt;
      const whole = Math.floor(this.pendingRows);
      this.pendingRows -= whole;
      rowsThisFrame = Math.min(whole, K_PAPER_ROWS / 2);

      for (let k = 0; k < rowsThisFrame; k += 1) {
        if (this.printed - this.receiptStart >= tearRows) this.resetPrinting(); // torn off at the length
        const at = floorMod(this.printed, K_PAPER_ROWS) * N;
        this.printPaperRow(this.printed, this.printingFeed, this.ring.subarray(at, at + N));
        this.printed += 1;
      }

      // The slot is the bottom row; row w shows paper row printed - R_w + w.
      const firstShown = this.printed - windowRows;
      const oldestHeld = Math.max(this.receiptStart, this.printed - K_PAPER_ROWS, 0);
      receiptTop = Math.max(oldestHeld - firstShown, -8);
      receiptBottom = windowRows;
      notchBottom = false;
      rowBase = floorMod(firstShown, K_PAPER_ROWS);
      texRows = K_PAPER_ROWS;
    } else {
      this.wasPrinting = false;

      // The receipt, centred on the window, printed afresh from its first
      // visible row with a cold head.
      receiptTop = floorDiv(windowRows - tearRows, 2);
      receiptBottom = receiptTop + tearRows;
      this.printer.resetHeat();
      this.printer.resetDither();
      const feed = newFeed();
      const first = Math.max(0, receiptTop);
      const last = Math.min(windowRows, receiptBottom);
      feed.dataRow = first - receiptTop;
      for (let w = first; w < last; w += 1) {
        this.printPaperRow(w - receiptTop, feed, this.window.subarray(w * N, w * N + N));
      }
      rowsThisFrame = Math.max(0, last - first);
    }

    // Printing's uploads: the rows printed since the last, in at most two runs
    // of the ring.
    const uploads = [];
    if (this.mode === K_PRINTING) {
      let from = Math.max(this.uploadedTo, this.printed - K_PAPER_ROWS);
      while (from < this.printed) {
        const row = floorMod(from, K_PAPER_ROWS);
        const count = Math.min(this.printed - from, K_PAPER_ROWS - row);
        uploads.push([row, count]);
        from += count;
      }
      this.uploadedTo = this.printed;
    }

    return { receiptTop, receiptBottom, rowBase, texRows, notchBottom, rowsThisFrame, uploads };
  }

  /// The display pass's colour uniforms, as ProcessOpenGL computes them (each
  /// rounded to a float, as glUniform takes them).
  colours() {
    const age = this.age;
    const paper = [0, 1, 2].map((ch) => Math.fround(TINT_RGB[this.tint][ch] * (1.0 + age * (AGED_STOCK[ch] - 1.0))));
    // kInkAged[ ch ] - kInkFresh[ ch ] is float minus float in the C++, so it
    // is a FLOAT subtraction; kAgedStock[ ch ] - 1.0 above is a double one.
    const ink = [0, 1, 2].map((ch) => Math.fround(K_MAX_OD * (INK_FRESH[ch] + age * Math.fround(INK_AGED[ch] - INK_FRESH[ch]))));
    return {
      paper,
      ink,
      fadeAmount: Math.fround(K_AGE_FADE * age),
      fadeRows: Math.fround(K_AGE_FADE_ROWS),
      heldOD: Math.fround(K_AGE_HELD_OD * age),
      heldRows: Math.fround(K_AGE_HELD_ROWS),
    };
  }
}

/// Every uniform ProcessOpenGL hands the sample pass, by name, as the page
/// sets them. check_port.mjs compares this with what the plugin's own
/// ProcessOpenGL sets, so the page's mapping is checked too.
export function sampleUniforms(geometry, srcW, srcH) {
  return { SrcW: srcW, SrcH: srcH, Dots: geometry.N, Unit: geometry.unit, Rotate: geometry.rotate ? 1 : 0 };
}

/// The same for the display pass. `mix` is the host's Mix, a float.
export function displayUniforms(engine, printed, outW, outH, mix) {
  const c = engine.colours();
  return {
    OutH: outH,
    Dots: engine.N,
    Pitch: engine.pitch,
    X0: engine.x0,
    FloatPitch: 0,
    PitchF: Math.fround(outW / engine.N),
    TexRows: printed.texRows,
    RowBase: printed.rowBase,
    ReceiptTop: printed.receiptTop,
    ReceiptBottom: printed.receiptBottom,
    NotchTop: 1,
    NotchBottom: printed.notchBottom ? 1 : 0,
    Paper: c.paper,
    Ink: c.ink,
    FadeAmount: c.fadeAmount,
    FadeRows: c.fadeRows,
    HeldOD: c.heldOD,
    HeldRows: c.heldRows,
    MixAmount: Math.fround(mix),
  };
}

function newFeed() {
  return { dataRow: 0, repeatLeft: 0, lastBits: new Uint8Array(0) };
}
