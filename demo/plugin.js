/**
 * Receipt — browser demo.
 *
 * A thermal receipt printer. A line of heaters and no ink: the paper darkens
 * where it gets hot enough, one dot row at a time, so heat is the whole look —
 * tone is a dither, a heater that fired on the last row is still warm, the
 * supply sags with the dots a block fires, the roller slips, and an old
 * receipt fades and yellows.
 *
 * Like teletext and fax, this plugin is **not only a shader**, and the two
 * halves of the page are not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX_SRC`, `SAMPLE_SRC` and `DISPLAY_SRC`
 *   below are `kVertex`, `kSample` and `kDisplay` from `source/Shaders.cpp`,
 *   spliced in unedited by `demo/tools/splice_shaders.py` and compared
 *   character for character by `demo/tools/check_shaders.py`, which
 *   `tools/verify.sh` runs.
 *
 *   The print engine is a PORT. In the plugin the print is the CPU's: the
 *   sample pass is read back and `Printer.cpp` dithers, applies history
 *   control, the budget, the heat and the paper's response in double, and
 *   `Receipt.cpp` runs the feed. A browser cannot run that C++, so
 *   `demo/printer.js` is a hand translation of it, and
 *   `demo/tools/check_port.sh` compares it with the plugin's own C++ exactly
 *   (it says what it covers). This file's GL calls are a hand copy of
 *   `Receipt::ProcessOpenGL`'s; only a reader checks those.
 *
 * ------------------------------------------------------ the frame
 *
 *   1. sample     the clip to the head's image, N x R_img R32F tone (GPU)
 *   2. read-back  readPixels as FLOAT, every frame, as the plugin's
 *                 glReadPixels does; it stalls where the plugin's does
 *   3. print      printer.js: Static's window afresh, or Printing's new rows
 *                 on to the ring (CPU, in double)
 *   4. upload     the window, or the ring's new rows, into R32F textures
 *   5. display    the dots to pixels, the stock, the dye, Age, Mix (GPU)
 *
 * ------------------------------------------------------- the clock
 *
 * Printing scrolls in elapsed time. The plugin reduces the host's clock to
 * frame-relative seconds in double; the page hands the engine the kit's
 * `time` — seconds since the page started, paused by Pause, stepped a 60th by
 * Step — with its unit declared, as the plugin's harness declares it, so the
 * plugin's vote on Resolume's clock unit never runs here. The first frame
 * primes the clock, a frame prints at most a quarter-second of rows, and a
 * clock sent backwards (Restart) prints nothing until it moves on: all the
 * plugin's own lines, in printer.js.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture, GLError } from './vendor/gl.js';
import {
  Engine, controls, sampleUniforms, displayUniforms,
  WIDTH_NAMES, DITHER_NAMES, MODE_NAMES, TINT_NAMES, FIT_NAMES,
  K_PRINTING, K_PAPER_ROWS, K_STROBE_BLOCKS_MIN, K_STROBE_BLOCKS_MAX, K_DOTS_PER_MM,
} from './printer.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here: run
// demo/tools/splice_shaders.py.
//---------------------------------------------------------------------------

const VERTEX_SRC = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const SAMPLE_SRC = `#version 410 core

uniform sampler2D InputTexture;
uniform int SrcW;
uniform int SrcH;
uniform int Dots;
uniform int Unit;
uniform int Rotate;

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

//The source's tone at pixel ( x, y from the top ): luma, over paper white by
//the source's alpha; off the source is paper.
float toneAt( int x, int yTop )
{
	if( x < 0 || yTop < 0 || x >= SrcW || yTop >= SrcH )
		return 1.0;
	vec4 s = texelFetch( InputTexture, ivec2( x, SrcH - 1 - yTop ), 0 );
	return dot( s.rgb, kLuma ) * s.a + ( 1.0 - s.a );
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int u0, v0;
	if( Rotate == 0 )
	{
		u0 = p.x * Unit;
		v0 = p.y * Unit;
	}
	else
	{
		u0 = p.y * Unit;
		v0 = ( Dots - 1 - p.x ) * Unit;
	}
	int u1  = u0 + Unit;
	int v1  = v0 + Unit;
	int kx0 = u0 / Dots;
	int kx1 = ( u1 - 1 ) / Dots;
	int ky0 = v0 / Dots;
	int ky1 = ( v1 - 1 ) / Dots;

	float sum = 0.0;
	for( int ky = ky0; ky <= ky1; ++ky )
	{
		int oy    = min( ( ky + 1 ) * Dots, v1 ) - max( ky * Dots, v0 );
		float row = 0.0;
		for( int kx = kx0; kx <= kx1; ++kx )
		{
			int ox = min( ( kx + 1 ) * Dots, u1 ) - max( kx * Dots, u0 );
			row += float( ox ) * toneAt( kx, ky );
		}
		sum += float( oy ) * row;
	}
	fragColor = vec4( sum / ( float( Unit ) * float( Unit ) ), 0.0, 0.0, 1.0 );
}
`;

const DISPLAY_SRC = `#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D PaperTexture;
uniform ivec2 Origin;
uniform int OutH;
uniform int Dots;
uniform int Pitch;
uniform int X0;
uniform int FloatPitch;
uniform float PitchF;
uniform int TexRows;
uniform int RowBase;
uniform int ReceiptTop;
uniform int ReceiptBottom;
uniform int NotchTop;
uniform int NotchBottom;
uniform vec3 Paper;
uniform vec3 Ink;
uniform float FadeAmount;
uniform float FadeRows;
uniform float HeldOD;
uniform float HeldRows;
uniform float MixAmount;

out vec4 fragColor;

const float kLog2Ten = 3.32192809488736;

//Floor division and modulo, for operands of either sign.
int idiv( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}

int imod( int a, int b )
{
	return a - b * idiv( a, b );
}

//Model.h's TearNotch: a sawtooth of period 8 dots, 0..2 rows deep.
int notch( int c )
{
	int r = imod( c, 8 );
	int d = r < 4 ? 4 - r : r - 4;
	return d / 2;
}

void main()
{
	ivec2 pix = ivec2( gl_FragCoord.xy ) - Origin;
	vec4 src  = texelFetch( InputTexture, pix, 0 );
	int yTop  = OutH - 1 - pix.y;

	int c, w;
	if( FloatPitch != 0 )
	{
		c = int( floor( ( float( pix.x ) - float( X0 ) ) / PitchF ) );
		w = int( floor( float( yTop ) / PitchF ) );
	}
	else
	{
		c = idiv( pix.x - X0, Pitch );
		w = idiv( yTop, Pitch );
	}

	vec4 receipt = vec4( 0.0 );
	bool onPaper = c >= 0 && c < Dots;
	if( onPaper )
	{
		int top    = ReceiptTop + ( NotchTop != 0 ? notch( c ) : 0 );
		int bottom = ReceiptBottom - ( NotchBottom != 0 ? notch( c ) : 0 );
		onPaper    = w >= top && w < bottom;
	}
	if( onPaper )
	{
		float D     = texelFetch( PaperTexture, ivec2( c, imod( RowBase + w, TexRows ) ), 0 ).r;
		float dist  = float( ReceiptBottom - 1 - w );
		float faded = D * ( 1.0 - FadeAmount * ( 1.0 - exp( -dist / FadeRows ) ) );
		vec3 od     = Ink * faded + vec3( HeldOD * exp( -dist / HeldRows ) );
		receipt     = vec4( Paper * exp2( -od * kLog2Ten ), 1.0 );
	}

	//The paper is a sheet: opaque whatever the clip's alpha. Off the paper is
	//transparent black, so the layer below shows round the receipt.
	fragColor = MixAmount >= 1.0 ? receipt : mix( src, receipt, MixAmount );
}
`;

//===========================================================================
// The renderer: Receipt::ProcessOpenGL's GL half, in its order.
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { mode: 0, rows: 0, printed: 0, tearRows: 0, receiptRows: 0, dots: 0, pitch: 0, imageRows: 0, engineMs: 0 };

const INT_UNIFORMS = new Set([
  'SrcW', 'SrcH', 'Dots', 'Unit', 'Rotate',
  'OutH', 'Pitch', 'X0', 'FloatPitch', 'TexRows', 'RowBase', 'ReceiptTop', 'ReceiptBottom', 'NotchTop', 'NotchBottom',
]);

function setAll(program, uniforms) {
  for (const [name, value] of Object.entries(uniforms)) {
    if (Array.isArray(value)) program.set(name, value[0], value[1], value[2]);
    else if (INT_UNIFORMS.has(name)) program.setInt(name, value);
    else program.set(name, value);
  }
}

/// A plain R32F texture, Nearest, clamped: makeFloatTexture in Receipt.cpp.
function makeFloatTexture(gl, width, height, pixels) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, width, height, 0, gl.RED, gl.FLOAT, pixels);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return texture;
}

function createRenderer(gl, quad) {
  const sampleShader = new Program(gl, VERTEX_SRC, SAMPLE_SRC, 'sample');
  const displayShader = new Program(gl, VERTEX_SRC, DISPLAY_SRC, 'display');
  const image = new PassBuffer(gl, { filter: 'nearest' });

  // The ring is 4,096 rows of paper, and the image can be up to 8,192 (the
  // plugin's own cap). WebGL2 promises only 2,048.
  const maxTexture = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  if (maxTexture < K_PAPER_ROWS) {
    throw new GLError(`This GPU's largest texture is ${maxTexture} rows; Receipt's paper ring is ${K_PAPER_ROWS}. The plugin needs it and the page will not shorten it.`);
  }

  const engine = new Engine();
  let staticTexture = null;
  let ringTexture = null;
  let readBuffer = new Float32Array(0);

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const picture = input;
      const srcW = picture.width;
      const srcH = picture.height;
      const outW = vpW;
      const outH = vpH;

      // The host's values, as params[] holds them. Strobe Blocks is a
      // dropdown here; integerValue turns its index back into the integer.
      const p = {};
      for (const id of ['width', 'dither', 'density', 'heatCarry', 'history', 'printSpeed', 'slip', 'mode', 'tearLength', 'age', 'paperTint', 'fit', 'mix']) p[id] = params.get(id);
      p.strobeBlocks = integerValue('strobeBlocks', params.get('strobeBlocks'));

      //------------------------------------------------------------------
      // The clock, the settings, the geometry, the buffers.
      //------------------------------------------------------------------
      const geometry = engine.begin(p, time, srcW, srcH, outW, outH);
      const N = geometry.N;
      if (geometry.imageRows > maxTexture) {
        throw new GLError(`The clip is ${geometry.imageRows} dot rows long on the paper, and this GPU's largest texture is ${maxTexture}.`);
      }
      image.ensure(N, geometry.imageRows, gl.R32F);

      if (engine.newRing) {
        if (ringTexture) gl.deleteTexture(ringTexture);
        ringTexture = makeFloatTexture(gl, N, K_PAPER_ROWS, engine.ring);
      }
      if (engine.newWindow) {
        if (staticTexture) gl.deleteTexture(staticTexture);
        staticTexture = makeFloatTexture(gl, N, geometry.windowRows, engine.window);
      }

      //------------------------------------------------------------------
      // 1. sample: the clip -> the head's image; read it back as floats.
      //------------------------------------------------------------------
      image.bind();
      gl.disable(gl.BLEND);
      sampleShader.use();
      bindTexture(gl, 0, picture.texture);
      sampleShader.setSampler('InputTexture', 0);
      setAll(sampleShader, sampleUniforms(geometry, srcW, srcH));
      quad.draw();
      bindTexture(gl, 0, null);

      // RGBA / FLOAT is the read every float target must accept; the image is
      // R32F, so the tone is every fourth float.
      const count = N * geometry.imageRows;
      if (readBuffer.length !== count * 4) readBuffer = new Float32Array(count * 4);
      gl.pixelStorei(gl.PACK_ALIGNMENT, 4);
      gl.readPixels(0, 0, N, geometry.imageRows, gl.RGBA, gl.FLOAT, readBuffer);
      const tones = engine.imageTones;
      for (let i = 0; i < count; i += 1) tones[i] = readBuffer[i * 4];

      //------------------------------------------------------------------
      // 2. the print, on the CPU: printer.js.
      //------------------------------------------------------------------
      const started = performance.now();
      const printed = engine.print();
      telemetry.engineMs = performance.now() - started;

      //------------------------------------------------------------------
      // 3. the printed rows to the GPU.
      //------------------------------------------------------------------
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
      if (engine.mode === K_PRINTING) {
        gl.bindTexture(gl.TEXTURE_2D, ringTexture);
        for (const [row, rows] of printed.uploads) {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, row, N, rows, gl.RED, gl.FLOAT, engine.ring.subarray(row * N, (row + rows) * N));
        }
      } else {
        gl.bindTexture(gl.TEXTURE_2D, staticTexture);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, N, geometry.windowRows, gl.RED, gl.FLOAT, engine.window);
      }
      gl.bindTexture(gl.TEXTURE_2D, null);

      //------------------------------------------------------------------
      // 4. display, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      displayShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, engine.mode === K_PRINTING ? ringTexture : staticTexture);
      displayShader.setSampler('InputTexture', 0);
      displayShader.setSampler('PaperTexture', 1);
      gl.uniform2i(displayShader.location('Origin'), 0, 0);
      setAll(displayShader, displayUniforms(engine, printed, outW, outH, p.mix));
      quad.draw();

      // Unbind, so nothing reads a framebuffer's own texture next frame.
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      telemetry.mode = engine.mode;
      telemetry.rows = printed.rowsThisFrame;
      telemetry.printed = engine.printed;
      telemetry.tearRows = engine.tearRows;
      telemetry.receiptRows = engine.printed - engine.receiptStart;
      telemetry.dots = N;
      telemetry.pitch = geometry.pitch;
      telemetry.imageRows = geometry.imageRows;
    },
  };
}

//===========================================================================
// The controls, read out of Receipt::Receipt(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// Strobe Blocks is FF_TYPE_INTEGER with SetParamRange( 1, 8 ), exempt from
/// the 0..1 clamp, so the plugin stores the integer itself. The kit has no
/// integer control, so -- as copperlist and teletext did -- it is a dropdown
/// of every value in the plugin's range; `integerValue` turns the dropdown's
/// index back into it.
const INTEGER_RANGES = { strobeBlocks: [K_STROBE_BLOCKS_MIN, K_STROBE_BLOCKS_MAX] };
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: value - INTEGER_RANGES[id][0], group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });

const oneIn = (rate) => (rate <= 0 ? 'no slips' : `a slip every ${Math.round(1 / rate).toLocaleString('en-GB')} rows`);

const demo = mountDemo({
  name: 'Receipt',
  pluginId: 'RC01',
  kind: 'effect',
  tagline:
    'A thermal receipt printer. A line of heaters and no ink: the paper darkens where it gets hot enough, one dot row at a time. Tone is a dither, a heater that fired on the last row is still warm so lines thicken and dark areas block up, the supply can only fire so many dots at once so dense rows print paler and band by strobe block, the roller slips, and an old receipt fades and yellows. Static prints the frame as a receipt; Printing scrolls it out of the slot and tears it off.',
  repo: 'https://github.com/stoatworks-labs/receipt',
  page: 'https://stoatworks-labs.com/software/receipt/',

  // The stock sentence says "same maths", which is only half true here: the
  // shaders are the plugin's, the print engine between them is a port.
  blurb:
    'It is Receipt’s own GLSL, ported from the repository to WebGL2, with the print engine — the dither, history control, the strobe budget, the heat, the paper and the feed, which the plugin runs on the CPU in C++ — ported to JavaScript because a browser cannot run the C++; a script in the repository checks that port against the plugin’s C++. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // The head's image is R32F and read back as floats: the engine dithers the
  // tones in double, and an 8-bit read-back would quantise them first.
  needFloat: true,
  // Off the paper is transparent black, so the layer below shows round the receipt.
  showBackdrop: true,

  params: [
    opt('width', 'Width', WIDTH_NAMES, 1, 'Printer',
      'The roll: 58 mm is a 384-dot head, 80 mm a 576-dot head, at 8 dots/mm. A new head is new paper.'),
    opt('dither', 'Dither', DITHER_NAMES, 1, 'Printer',
      'How the firmware turns tone into dots: an 8 × 8 Bayer matrix, Floyd–Steinberg or Atkinson (which throws a quarter of every error away), raster order over the data rows.'),
    std('density', 'Density', 0.5, 'Printer', {
      display: (v) => `e₀ ${controls.energy(v).toFixed(2)}`,
      hint: 'The nominal strobe energy, 0.5 + v: at the default a lone fresh dot just saturates the paper.',
    }),
    std('heatCarry', 'Heat Carry', 0.4, 'Printer', {
      display: (v) => `c ${controls.carry(v).toFixed(3)}`,
      hint: 'The fraction of a heater’s heat left at the next row, 0.9 v. Carried heat thickens vertical lines and blocks up dark areas.',
    }),
    std('history', 'History Control', 0.5, 'Printer', {
      display: (v) => `h ${controls.history(v).toFixed(2)}`,
      hint: 'How much of the predicted leftover heat the firmware takes off a firing heater’s strobe. At 1 a firing heater always reaches exactly e₀; nothing cools a heater that does not fire.',
    }),
    integer('strobeBlocks', 'Strobe Blocks', 2, 'Printer',
      'The head is strobed in this many blocks, and a block firing more than 96 dots sags by 96 / n. Fewer blocks: dense rows print paler, banded at the block seams.'),

    std('printSpeed', 'Print Speed', 0.4, 'Feed', {
      display: (v) => `${controls.speedMmPerSecond(v).toFixed(1)} mm/s`,
      hint: 'Printing mode only: 2 + 248 v² mm/s, at 8 dot rows per mm, in elapsed time.',
    }),
    std('slip', 'Slip', 0.15, 'Feed', {
      display: (v) => oneIn(controls.slipRate(v)),
      hint: 'The roller slips at 0.02 v² per row: half the slips repeat a row on 1–4 more rows (the picture stretches), half stall the paper while 1–4 more data rows strike it (a dark band, rows skipped).',
    }),
    opt('mode', 'Mode', MODE_NAMES, 0, 'Feed',
      'Static prints the frame as a receipt, afresh every frame. Printing scrolls the receipt out of the slot in real elapsed time, each new row from the frame showing when the head reaches it, and tears it off at Tear Length.'),
    std('tearLength', 'Tear Length', 0.4, 'Feed', {
      display: (v) => `${controls.tearMm(v).toFixed(0)} mm (${controls.tearRows(v).toLocaleString('en-GB')} rows)`,
      hint: 'The receipt’s length, 30 + 570 v² mm.',
    }),

    std('age', 'Age', 0.15, 'Paper', {
      display: (v) => controls.age(v).toFixed(2),
      hint: 'Fades the dye away from the tear edge, develops the background near it where the receipt was held, yellows the stock and browns the dye.',
    }),
    opt('paperTint', 'Paper Tint', TINT_NAMES, 0, 'Paper', 'The paper stock.'),
    opt('fit', 'Fit', FIT_NAMES, 0, 'Paper',
      'Letterbox fits the clip’s width to the head; Rotate turns it so its width runs down the paper; Tile is Letterbox with the copies end to end along the receipt.'),
    std('mix', 'Mix', 1.0, 'Paper'),
  ],

  // Tone first (the ramps show each dither), then the general case, then
  // lines (the heat) and dark footage (the budget).
  sources: ['scene', 'ramp', 'grid', 'spot', 'bars', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Printing out of the slot': { mode: 1 },
    'Fast, short receipts': { mode: 1, printSpeed: 0.85, tearLength: 0.12 },
    'Hot head, no history control': { heatCarry: 0.9, history: 0 },
    'Four strobe blocks': { strobeBlocks: 3 },
    'Bayer on a 58 mm roll': { dither: 0, width: 0 },
    'Atkinson': { dither: 2 },
    'A slipping roller': { slip: 1 },
    'An old canary receipt': { age: 0.9, paperTint: 2 },
    'Rotated': { fit: 1 },
  },

  differences: [
    'The print engine is a PORT, not the plugin’s own code. Receipt reads its sample pass back to the CPU and runs the firmware and the head there in C++ — the dither, history control, the strobe-block budget, the heat, the paper’s response, the slips, Static’s window and Printing’s ring and tear (Printer.cpp and Receipt.cpp). A browser cannot run that C++, so demo/printer.js is a hand translation of it, in doubles, in the same order of operations, with no fused multiply-add (the plugin is built with -ffp-contract=off). demo/tools/check_port.sh compiles the plugin’s own Printer.cpp and Controls.cpp, and the feed and ProcessOpenGL cut out of Receipt.cpp’s own text, and compares the port with them exactly: the control laws, every fired bit, density and heater’s heat in runs of the engine, and 473 frames of Static and Printing — the paper, the uploads and every uniform. Equality on those cases is evidence about the port, not a proof; the GL calls on this page only a reader checks.',
    'The GPU half is not a port. The sample pass and the display pass are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of either (or of the vertex shader) drifts.',
    'The head’s image is rendered into an R32F target and read back as floats every frame, as the plugin’s glReadPixels does, and it stalls where the plugin’s does. The tones come from this browser’s GPU, not the plugin’s: a tone one float step different can move a dither decision, and error diffusion carries that along the row, so a dot pattern here is not evidence of the dot pattern the plugin prints from the same frame.',
    'Printing runs on the page’s clock in seconds, frame-relative (Pause stops the feed, Step feeds a 60th of a second, Restart sends the clock back and nothing prints until it moves on). The plugin votes on the host clock’s unit (seconds or milliseconds) over its first frames; that vote never runs here.',
    'Strobe Blocks is an FF_TYPE_INTEGER from 1 to 8 in the plugin. The kit has no integer control, so it is a dropdown of every value in that range.',
    'No clip with transparency is offered. The plugin prints tone over paper white by the clip’s alpha, assuming straight alpha; the kit’s clips, and any file dropped in, are premultiplied on the way in, so a soft edge would print darker here than in Resolume. The generated clips offered are opaque.',
    'The canvas is drawn at the Composition size, with every dot a whole number of pixels as the plugin draws it, and then the browser scales the canvas to fit the page, which resamples the dots. Embed mode (?embed=1&size=WxH) shows it unscaled.',
    'The plugin’s Perturb test hooks and its forced-slip hook are not ported: none of them is part of what the plugin does in a host. The About block is absent, as on every page in this suite.',
    'There is no audio caveat on this page: Receipt has no audio path.',
    'The plugin’s proof — the dither’s tone within each dither’s bound, the line thickening by what the heat carry predicts, the budget’s sag, the slips, the grid, the elapsed-time feed and the tear — is the offline harness rctest in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported engine did.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas: the ported engine's own numbers. Skipped in
// embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.id = 'receipt-status';
    stage.append(line);
    setInterval(() => {
      const t = telemetry;
      if (t.dots === 0) return;
      const head = `${t.dots}-dot head at ${t.pitch} px a dot; the clip is ${t.imageRows} rows (${(t.imageRows / K_DOTS_PER_MM).toFixed(0)} mm) of paper.`;
      const what = t.mode === K_PRINTING
        ? `Printing: ${t.rows} row${t.rows === 1 ? '' : 's'} this frame, ${t.receiptRows.toLocaleString('en-GB')} of ${t.tearRows.toLocaleString('en-GB')} on this receipt, ${t.printed.toLocaleString('en-GB')} since the page started.`
        : `Static: ${t.rows} rows printed this frame, afresh, with a cold head.`;
      line.textContent = `${what} ${head} ${t.engineMs.toFixed(1)} ms in the ported engine.`;
    }, 250);
  }
}
