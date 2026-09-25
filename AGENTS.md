# AGENTS.md — Receipt

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A thermal receipt printer as an FFGL 2.1 effect (`RC01`, shown as `SW Receipt`) for
Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` (and a
Windows `.dll` that CI will build once there is a repo to run it). MIT; intended home
`github.com/stoatworks-labs/receipt`. Released v0.1.0 on 2026-09-25.

Built 2026-09-25 in one session (tranche five, an idea Allan picked) from
`specs/SPEC-receipt.md` with `BRIEF.md` and `BRIEF-ADDENDUM.md`: toner for a print
engine with a supply that runs short and the AGENTS.md shape; filament for the harness's
clock, the software-renderer pass and typed cues; slope for serial state along the scan
(and for the reason not to use its chunked scheme here, below); teletext for a 1-bit
device whose decisions are made on the CPU after a read-back; tinsel for `PassBuffer` and
the trap list; graticule for the notes and the provisional About.

---

## The one idea

**A thermal printer has no ink. A line of heaters sits on heat-sensitive paper and the
paper darkens where it gets hot enough, one dot row at a time, so heat is the whole
look.**

Per row (one strobe of the head), in order, all of it in `Model.h` and `Printer.cpp`:

| stage | what it does |
| --- | --- |
| dither | the row's tones t become fire bits b: Bayer 8×8, Floyd–Steinberg or Atkinson, raster order, over DATA rows (the host's lines), not paper rows |
| history control | a firing heater's strobe is shortened by the heat the firmware predicts is left in it: e = b·max(0, e0 − h·c·θ); at h = 1 a firing heater is always exactly e0 |
| budget | the head is strobed in S blocks; a block with n firing heaters sags, e ×= min(1, 96 / n) |
| heat | each heater is a one-pole: θ ← c·θ + e (c = Heat Carry) |
| paper | the paper under heater x sees q = θₓ + 0.22·(θₓ₋₁ + θₓ₊₁) and develops D = smoothstep(0.35, 1, q) |
| feed | a slip (seeded by row) repeats a data row on m more paper rows, or stalls the paper while m more data rows strike the same one (the darkest strike wins) |
| display | the stock times 10^(−1.3·D·k_rgb): Beer–Lambert on the dye's density; Age fades D away from the tear edge, develops the background near it, yellows the stock and browns the dye |

And what falls out, none of it drawn:

| what the head does | what comes out |
| --- | --- |
| a dot is on or off | tone is a dither pattern, at the head's real 8 dots/mm |
| a heater that fired is still warm on the next row | a vertical line thickens by a dot each side once θ·0.22 reaches the paper's half-density heat, and runs on below its end while the heat decays; dark areas block up |
| history control subtracts the predicted heat | no thickening; the run-on below a line's end stays (nothing can cool a heater that is not fired) |
| the supply sags with the dots a block fires | dense rows print paler; a row whose blocks differ in coverage bands at the block boundaries; dark footage prints a banded grey, not a black slab |
| the roller slips | a repeated row stretches the picture; a stall overprints a dark band and skips rows |
| the receipt comes out of a slot | in Printing each row is printed from the frame showing when the head reached it, so the paper is a slit-scan of time, torn off at the length |

### What does not fall out, and is the honest limit

- **Feed jitter is not modelled.** The spec asks for "feed jitter and slip events". Whole
  rows only: a sub-row displacement cannot land on a whole-pixel grid, and a jitter
  faked as a per-row density wobble would be a texture, not a mechanism. Slips are
  whole-row repeats and stalls.
- **The budget sags; it does not re-strobe.** The spec allows "printed with the
  predicted lower density, OR in the predicted number of strobes". This is the first:
  a fixed block schedule and a sagging supply. A firmware that splits a dense row into
  more strobes (full density, slower) is not here, and nothing slows down.
- **Heat Carry is per row, not per second.** A real head cools in time; the line time
  is set by the print speed. Here the carry is per strobe whatever Print Speed says, so
  Static and Printing heat alike.
- **The lateral spread reaches one heater each side.** Enough for a line to thicken by
  one dot a side (which is what `--history` predicts and measures); a wider bleed from
  a much hotter head is not in the model.
- **The paper's response, the budget and every constant are assumptions**, stated in
  `Model.h`, not measured from a printer: threshold 0.35, saturation 1, spread 0.22,
  budget 96 dots, OD 1.3. The shape (threshold, steep rise, saturation) is the real
  one; the numbers are chosen.
- **Rotate turns the clip one way** (its top to the paper's right edge) and there is no
  option for the other.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.h` | The printer, described; the constants; the `Perturb` bits; the option tables; the Bayer matrix; the head's layout on the output; the tear notch; the integer hash and the slip schedule. |
| `source/Controls.{h,cpp}` | Every slider to its physical unit. |
| `source/Printer.{h,cpp}` | **The print engine**: dither, history control, budget, heat, paper — one data row at a time, on the CPU, in double. |
| `source/Shaders.{h,cpp}` | The sample pass (the clip to the head's dots, an exact area average) and the display pass (dots to pixels, the stock, the dye, Age, Mix). |
| `source/Receipt.{h,cpp}` | The plugin: parameters, the clock, the feed (slips, Static's window, Printing's ring and tear), the read-back and upload, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed, for the head's image. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/rctest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters and on the software renderer, plus the release-time checks. |

Per frame:

1. **sample** (GPU) — the host's picture to the head's image, N × R_img R32F tone. Each
   dot is the exact area average of the source pixels under it: its footprint is
   [c·U, (c+1)·U) in units of 1/N source pixel (U = the source's width for Letterbox and
   Tile, its height for Rotate), every overlap an integer. Tone is Rec. 601 luma over
   paper white by the source's alpha.
2. **read-back** — `glReadPixels` of the whole image (576 × 324 floats for a 16:9 clip).
3. **print** (CPU) — Static: the receipt, centred on the window, printed afresh from its
   first visible row with a cold head. Printing: `floor( accumulated rows/s × dt )` new
   rows on to the ring, the head's heat and the dither's error carried from the last
   frame, a tear (new receipt, dither reset, heat kept) at Tear Length.
4. **upload** — the window's rows (Static) or the new rows into the ring (Printing).
5. **display** (GPU) — output pixel (X, Y) shows dot floor((X − x0)/p) of window row
   floor(Y/p), p = max(1, floor(W/N)), x0 = floor((W − N·p)/2), in integers.

### Why the CPU, measured

Floyd–Steinberg and Atkinson are serial in BOTH directions: a dot's bit depends on the
dot before it in the row and on three (Atkinson four) in the rows above. GLSL 4.10 has
no image stores, so a fragment can only write itself. The two GPU forms on the table:

- **A wavefront**: dot (x, y) is ready at step x + 2y, so a 576 × 360 window is
  576 + 720 = 1,296 dependent draws a frame, each writing one anti-diagonal and reading
  the three before it. Not built; at the ~10 µs a dependent draw costs through a driver
  that is ~13 ms before any work, against the 1.2 ms below. An estimate, not measured.
- **slope's chunked re-run** (a draw per chunk of 32 along the scan, each fragment
  re-running its chunk from the true state the last draw wrote) is exact along ONE axis
  because slope's lines are independent. Here the rows are not: the error carried down
  is the whole row above. Chunking both axes is the wavefront again.

The CPU runs the whole engine at head resolution — a fifth of a megapixel whatever the
output's size — and the bench (`rctest --bench`, the plugin's own stage timers, M4 Max,
shared machine, best of three) says what it costs:

| case | 720p | 1080p | 4K | of which: sample + read-back | engine | upload |
| --- | --- | --- | --- | --- | --- | --- |
| Static, Floyd–Steinberg (default) | 1.68 ms | 1.94 ms | 2.62 ms | 0.37 / 0.62 / 1.29 | 1.22 | 0.08 |
| Static, Bayer | 0.92 | 1.13 | 1.36 | 0.41 / 0.62 / 0.87 | 0.40 | 0.08 |
| Static, Rotate (576 × 1024 image) | 1.71 | 1.85 | 3.12 | 0.46 / 0.63 / 1.86 | 1.15 | 0.08 |
| Printing | 0.40 | 0.39 | 1.09 | 0.36 / 0.36 / 1.05 | 0.02 | 0.00 |

The engine is flat across rasters (1.2 ms for error diffusion, 0.4 ms for Bayer): it runs
at head resolution. What grows with the raster is the sample pass, whose read waits for
it. **Chosen: the CPU pass over a read-back.** The cost the numbers do not show is the
stall: `glReadPixels` into client memory waits for the GPU to finish everything queued
before it, the host's own layers included. A pixel-buffer read one frame late would hide
it at the price of a frame of latency; not done (open questions).

---

## Traps

Roughly in the order they will bite.

### ☠️ Dark footage printed as a black slab at four strobe blocks

The first defaults (Strobe Blocks 4, Heat Carry 0.4) turned six of the eight demo
clips into near-solid black receipts with a few white features: a solid row in a
144-dot block sags to 2/3 energy, the lateral spread lifts an interior dot to
q = 0.67 × 1.44 = 0.96, and the heat carried saturates it within a row or two. At two
blocks a solid row sags to a third, q = 0.48 steady before the carry, the dark areas
print as a banded grey with the dither's texture in it, the block seam shows down the
middle, and the clips' detail comes back. There is a cliff between 2 and 3 blocks for
the same reason. The default is 2 — the cheap printer's look, and the budget is the
spec's own mechanism for it.

### ☠️ Printing centred a Letterbox image on a 600 mm receipt

Static centres the receipt on the window and the image on the receipt. Printing did
the same, so at a long Tear Length the first 2,238 rows of every receipt were blank
paper and `--printing`'s black frame printed nothing at all (6 rows wrong at 60 fps).
Printing now starts the image `kLeadRows` (24, 3 mm) below the torn edge; Static still
centres it. The printing checks run on Tile, where every row is the frame.

### The flat grey is only on the image

`--dither` and `--budget` at 1280×720 first measured the whole window, which is 360
rows of a 324-row image: 18 rows of blank receipt margin top and bottom, and every
mean came out 10% low (0.45 for 0.5). A harness bug, found because the tolerance was
derived and not fitted. The regions are the image's rows now.

### Letterbox and Tile are the same picture at 320×180

Tile is Letterbox with the margins filled by the next copies. At 320×180 the window
(180 rows) lies wholly inside the image (324), so the sweep reported Fit dead. It
compares Letterbox with Rotate.

### The mix that is not a mix

The display takes `mix( src, receipt, Mix )` only below Mix 1: GLSL does not promise
`mix( a, b, 1.0 ) == b` (an implementation may compute a + t(b − a)), and the grid
check holds every cell's pixels identical.

### A host can leave a pixel buffer bound

Not hit, guarded: a bound `GL_PIXEL_UNPACK_BUFFER` turns `glTexSubImage2D`'s pointer
into an offset into it and a bound pack buffer takes the read-back; a row length left
set tears every row. `ClientTransfer` in `Receipt.cpp` sets all of that for each
transfer and puts back what the host had.

### Two slices, one dither

The engine runs in double and a dither decides on `v >= 0.5`: one ulp is a different
dot and a different pattern for the rest of the row. Clang contracts `a * b + c` into a
fused multiply-add on arm64 and not on x86_64, so the core builds with
`-ffp-contract=off` (MSVC `/fp:precise`). Measured under Rosetta: the two slices'
`--pipe` output on three frames of IntoTheGlow_02 is byte-identical — and it was also
identical with contraction ON, so on these frames nothing was contracted that mattered.
The flag is a precaution, not a fix for a divergence seen.

### The printing negative control fails at 60 fps too

`kPerturbClockPerFrame` clocks every frame at a 60th, including the priming frame the
plugin otherwise leaves at dt = 0, so even at 60 fps it prints one frame's rows early.
The control was meant to bite at 30 and 50 fps; it bites everywhere, which is still a
failed check.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the display, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` and every texture allocation
happens before anything binds a texture, and the passes use raw binds they clear
themselves; `FFGLFBO::Release()` leaks the colour texture (`PassBuffer::Destroy()`
deletes it first); `SetParamInfo` clamps a STANDARD default into 0..1 and
`SetParamInfof` reads its default out of `params[]`; an option's range reads back 0..1
whatever its element count; the core is an **OBJECT** library; `SetTextParameter` must
return `FF_SUCCESS` for the About block; Resolume's clock overflows a float, so time is
frame-relative and in double; `nm | grep -q` fails under pipefail when grep succeeds; a
closed stdout must be a failed write, so `--pipe` ignores SIGPIPE; zsh has no
`PIPESTATUS`, so `verify.sh` is bash; `packed` (and the rest of the 4.10 reserved list),
`near` and `far` are never identifiers — `verify.sh` greps the dumped shaders for all
of them; `kPi`, not `M_PI`.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at 320×180
and 1280×720 in `verify.sh`, and at 320×180 on Apple's software renderer.

What makes them rasteriser-proof by construction: **the print is the CPU's, in double,
with no contraction** — no GPU arithmetic touches a bit or a density; **every GPU
coordinate is an integer computed in integers** (`gl_FragCoord`, never an interpolated
uv) and every read is `texelFetch`; **the sample pass's overlaps are integers**, so a
dot wholly inside black (or white) source pixels is exactly 0 (or 1) — integer sums
below 2²⁴ are exact in float; the only GPU transcendentals on a checked path are the
display's `exp2` (3 ulp, GLSL 4.10 §8.2) and two `exp`s that Age 0 multiplies by zero.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--dither` Bayer | fired fraction over whole 8×8 tiles of a flat grey | exact: #{k : (k + ½)/64 < d}/64, an integer count; the greys sit ≥ 1/640 from every threshold, far outside the sample pass's few-ulp tone error | tiles aligned to the head's column and the data row's phase; 320×180 sees 320×176, 1280 sees 576×320 |
| `--dither` F–S | fired fraction over the visible image | ½·(22h/16 + 18w/16 + 2)/(w·h) + 10⁻⁶: every error is within ±½ by induction for d in [0, 1], so only what crosses the region's edges moves the sum (weights in and out per edge row/column), plus the tone's float error | the bound shrinks with the region: 0.0053 at 320×180, 0.0029 at 576×324; measured 0.0013 and 0.0009 |
| `--dither` Atkinson | the same | ⅛ + ½·(10h + 10w)/(8·w·h) + 10⁻⁶: a quarter of every error is thrown away (at most A/8) plus the edge pairs; and **exactly 0** below d = ⅛ (the carried value is at most d + ¾ of itself, 4d < ½) and exactly 1 above ⅞ | none in the exact half; the bound is loose (0.13 against 0.068 measured) and says so |
| dot reading (all) | fired or not, from the output | exact: a fired dot is at q ≥ e0 = 1 (D = 1) and an unfired one at q ≤ 2 × 0.22 (D ≤ 0.053) on the clean printer (no carry, 8 blocks of 72 dots under the 96 budget) | none |
| density reading (all) | D from a dot's red channel, −log₂(R/paper)/(1.3 log₂ 10) | 10⁻⁶ on D: `exp2` 3 ulp + the paper multiply ½ ulp → 4 × 2⁻²³ relative on R → 1.2 × 10⁻⁷ in D; the R32F store of D, 2⁻²⁴; six-fold margin | none |
| `--history` | per row, the dark dots across a 9-dot line; every density against the closed form | widths exact (integers); densities 10⁻⁶ (above); θⱼ = e0(1 − cʲ⁺¹)/(1 − c) and the decay cᵏθ in double | the line and its rows are source-aligned (9 dots: 576/gcd(W, 576) at 320 and 1280), so the input is exactly 0/1 at both |
| `--budget` | every visible dot of four bands against the stated sag; where the density steps | 10⁻⁶ on D; a "step" is a change > 10⁻³ between interior fired neighbours, 1,000 × the D tolerance, so reading noise cannot make one; a step must sit within one dot of a block boundary (the lateral spread reaches one dot) | 320×180 sees dots 128..447, so block 0 of S = 2..4 is partly out of view; the bands are source-aligned |
| `--slip` | every row of a slipped print against the unslipped one, shifted as stated | 10⁻⁶ on D across two renders: the engine's doubles are identical (heat carry 0 makes a row a function of its own bits); only the display could differ, by an ulp, on a renderer that is not bit-repeatable | none |
| `--grid` | every pixel classified off / paper / ink from the stated layout; every cell uniform; the paper's extent | exact: ink R = 0.048, paper R = 0.955, threshold at half the paper; uniformity is float equality within a cell (the display is a function of the cell alone at Mix 1) | at pitch 1 (320×180) uniformity is trivially true and says so; the placement half is not (x0 = −128) |
| `--printing` | the frame's rows and the receipt's top, in whole rows, at 60/50/30 fps; the tear | exact integers; floor(r·t) in double, asserted ≥ 10⁻⁹ from any whole row at every frame used | window rows differ; the rows printed do not (time, not raster) |
| `--resize` | every dot in view before and after a resize | 10⁻⁶ on D (the same floats through the same shader) | the two rasters' visible dots overlap differently; > 1,000 compared at both |
| `--alpha` | alpha on and off the paper at Mix 1, 0.25, 0 | 3 × 2⁻²⁴: the mix's two products and a sum at values ≤ 1; Mix 0 colour exact | none |
| `--laws`, `--names`, `--cues` | control laws, constants, layout laws, Bayer recursion, slip statistics, names, cue kinds | 10⁻¹²; exact; slip counts within 6σ of the binomial (a false alarm 2 × 10⁻⁹) | none (no GL) |
| sweep | any subpixel differs | ≥ 1 | 320×180 here, 160×90 in CI |

Deliberately NOT relied on: `mix( a, b, 1 ) == b` (branched around); `exp2( 0 ) == 1`
(paper is read through the same tolerance as ink); an interpolated varying; a texture
unit's filtering; round-to-nearest in the shaders; the 8-bit read-back (the harness
reads floats). What might still differ elsewhere: a compiler that contracts the engine's
arithmetic despite the flag (MSVC's `/fp:precise` is not a hard guarantee) would move a
dither decision now and then — the checks' inputs are exact 0/1 and would not see it,
footage would; and a driver whose `exp2` is worse than 3 ulp eats into the D tolerance's
six-fold margin.

### The negative controls

`rctest --negative` runs seven; `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin* — a `Perturb` bit the shipped plugin carries at zero — never
the harness's expectation.

| perturbation | what fails, at 320×180 and 1280×720 |
| --- | --- |
| no heat carried (c = 0 whatever Heat Carry says) — the spec's "zero Heat Carry" | `--history`: 2 assertions — no thickening, no run-on |
| no supply budget — the spec's "remove the budget" | `--budget`: all 3 block counts |
| error diffusion that throws its error away | `--dither`: 2 — F–S and Atkinson at a plain ½ threshold |
| a slip one row shorter than it says | `--slip`: all 4 |
| a display pitch of W / N pixels | `--grid`: 2 — pixels misclassified, cells non-uniform |
| Printing advancing a 60th of a second a frame | `--printing`: all 4 (see the trap above for why 60 fps too) |
| a resize that clears the paper | `--resize`: 1 |

### The mutation

One character of the shipped GLSL, on a clean committed tree (cc5a111): in the display
pass's `notch()`, `return d / 2;` → `return d / 4;` — the torn edge's sawtooth half as
deep. Caught by **`--alpha`** at both rasters (200 pixels wrong at 320×180, 1,440 at
1280×720, in the Mix 1 and Mix 0.25 cases: it states which pixels are paper, notch and
all). Correctly not caught by `--printing`'s tear (measured in a column whose notch is
0), `--grid` (a 600 mm receipt's ends are far outside the window), nor anything that
reads dots rather than edges. Reverted with `git checkout source/Shaders.cpp` and a
`touch`; the tree was clean before and after, and `--alpha` passes again on the rebuilt
binary.

---

## Decisions taken without asking

- **The print engine is on the CPU**, over a read-back at head resolution (above, with
  the measurements).
- **The head spans the output's width at a whole pitch**, p = max(1, floor(W/N)),
  centred; the window is ceil(H/p) rows from the top. At 1080p an 80 mm head is 3 px
  dots, 1,728 px wide, 360 rows tall. Narrower than the head (320 wide), p = 1 and the
  head's ends are cut off.
- **Fit**: Letterbox fits the clip's width to the head (a 16:9 clip is 324 rows on
  80 mm); Rotate turns it so its width runs down the paper (1,024 rows); Tile is
  Letterbox with the copies end to end along the receipt.
- **Static prints the window afresh every frame with a cold head**, from the first
  visible row of a receipt centred on the window. Rows above the window are not printed,
  so their heat and error are not carried in.
- **Printing's image starts 3 mm below the torn edge**; its rows come from the frame on
  screen when the head reaches them; a frame prints at most a quarter-second of rows;
  a tear drops the dither's error and keeps the head's heat.
- **The torn edge** is a sawtooth, period 8 dots, 0–2 rows deep, at both ends of a
  Static receipt and the top of a Printing one (the bottom is the slot).
- **Tone is Rec. 601 luma of the clip over paper white by its alpha** (straight alpha
  assumed), so a transparent region prints nothing. Dithered in the signal, not in
  linear light, as a printer driver hands an 8-bit grey to the head.
- **Output alpha**: the paper is opaque (alpha 1 whatever the clip's), off the paper is
  transparent black, and Mix fades colour and alpha together. Resolume's demo clips
  carry alpha (three of the eight surveyed): the rings and the particles print as marks
  on white paper, and round the strip the layer below shows through.
- **Defaults**: 80 mm, Floyd–Steinberg (the firmware's usual), Density 0.5 (a lone dot
  just saturates), Heat Carry 0.4 (c = 0.36), History Control 0.5, **Strobe Blocks 2**
  (above), Print Speed 0.4 (~42 mm/s), Slip 0.15, Static, Tear Length 0.4 (~121 mm),
  Age 0.15, White, Letterbox, Mix 1. Chosen on eight demo clips through `--pipe`.
- **Slips are hashed per row** (receipt row in Static, absolute row in Printing): at Slip
  1 one row in fifty, half repeats and half stalls, 1–4 rows. A repeat in progress is
  not interrupted by another event.
- **History Control acts on the heater's own predicted heat**, not on the last row's bits
  (the other common firmware scheme); at 1 it is perfect for firing heaters.
- **No feed jitter, no re-strobing** (the limits above).
- **Parameter names** are the spec's, all ≤ 16 characters (`History Control` is 15).
- **Strobe Blocks is a real integer 1..8**; blocks are floor(kN/S) wide, so 5 and 7
  blocks are unequal by a dot.
- **About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) began as hand copies
  adapted from toner's; at release the project was registered and both were generated
  by the fleet's syncs (four About buttons, the guide's URL in).
- **The FFGL submodule was dissociated from the reference clone** (`repack -a -d`, the
  alternates file removed) so this repo does not depend on a path in `~/Projects`.
- **`--cues`** holds the step rules for booleans and events on the cue code itself: this
  plugin has no boolean, and its only events are the About buttons, which open a
  browser. verify.sh steps a real option and a real integer through `--pipe`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

Every number is `tools/verify.sh` against a fresh universal Release build, at 320×180
and 1280×720 (and 320×180 on the software renderer, all passing).

- **Dither.** Bayer's fired fraction is exactly count/64 at ten greys; Floyd–Steinberg
  within 0.0013 (bound 0.0053) at 320×180 and 0.0009 (0.0029) at 1280×720; Atkinson
  within 0.068 (bound 0.13), and exactly blank below d = ⅛ and solid above ⅞.
- **History.** At Heat Carry 0.81 with History Control 0 a 9-dot line is 11 dots from its
  row 4 on, counting from 0 (predicted row 4), and runs on 11 rows below its end
  (predicted 11); with History Control 1 it stays 9 and runs on 3 (predicted 3); every
  row's width right and every density within 1.0 × 10⁻⁷ of the closed form.
- **Budget.** At 2, 3 and 4 blocks every visible dot of four bands within 1.1 × 10⁻⁷ of
  the stated sag; the palest fired dot in view prints at D = 0.021 (2 blocks), 0.35–0.37 (3, by raster: which dots are in view), 0.80 (4); every
  density step between interior fired dots within one dot of a block boundary.
- **Slip.** Repeats of 1 and 3 and stalls of 1 and 3: every row of the window (177 to
  360) where the stated shift says, 0 wrong, and every row moved distinct from its
  neighbour.
- **Grid.** 57,600 pixels (320×180, pitch 1, head at −128) and 921,600 (1280×720, pitch 2,
  head at 64) classified with 0 wrong; every 2×2 cell uniform; the paper spans exactly
  the stated columns.
- **Printing.** 150, 153 and 155 rows after 0.45, 0.46 and 0.47 s at 60, 50 and 30 fps,
  equal to the plugin's own count; the black frame's rows exactly where stated; the
  receipt tore at 240 rows and the new one's top is where stated.
- **Resize.** 11,160 dots (2,981 ink) at 320×180 → 240×136 and 77,760 (29,122 ink) at
  1280×720 → 960×540, all unchanged across the resize.
- **Alpha.** Paper alpha exactly 1 and off-paper 0 from a half-transparent clip, 0.625
  and 0.375 at Mix 0.25, the source itself at Mix 0; a transparent black clip prints
  blank paper.
- **Negative controls.** All seven fail their check, at both rasters and on the software
  renderer.
- **Mutation.** Caught by `--alpha` (above).
- **No dead controls**, all 14, with the four About entries skipped.
- **Every shader compiles** through `glslc`, all 3, as the plugin hands them to the
  driver; no reserved word, `near` or `far` in them.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue with
  2, exits 1 on a failed render and on a closed stdout (`| head -c 1`, status read with
  bash's PIPESTATUS), steps an option and an integer, ramps a slider; `--cues` holds
  booleans and events.
- **The two slices agree**: under Rosetta the x86_64 harness passes the checks and its
  `--pipe` output on footage is byte-identical to arm64's.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.receipt` and version 0.1.0, ad-hoc signs, and `oxbow` reports
  `SW Receipt` / `RC01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost** (above): 1.7 / 1.9 / 2.6 ms at 720p / 1080p / 4K at the defaults.
- **On footage, by eye only:** eight of Resolume's bundled demo clips (Beat 001, Bass 003,
  Synth 004, Trinity_09, IntoTheGlow_02, OrganicMotions_06, BattleWeapon_Tank_09,
  FogAndDust_3), one frame each at 1280×720 through `--pipe` at the defaults, and four
  seconds of IntoTheGlow_02 in Printing on Tile. Bright-on-black clips print as a banded
  dark-grey receipt with the lit shapes in paper white and the block seam down the
  middle; the three with alpha print their shapes as marks on white paper; Printing
  grows the receipt from the slot and tears it at about 2.9 s. The first defaults'
  black slabs were found this way.

### Assumed, or not done

- ☠️ **Never loaded into Resolume.** Everything was compiled, rendered and measured
  offline against the real plugin class in a headless CGL context, plus an `oxbow` load.
  How Resolume's clock arrives, whether a host leaves a PBO bound, what the read-back
  stall costs inside a real composition: all unmeasured.
- **The printer's constants are chosen, not measured** (above).
- **Feed jitter and re-strobing** are not modelled.
- **Windows: gated, not shown.** CI's MSVC build compiled first time (vcpkg GLEW); the
  release DLL passed the fleet's Arena gate 9/9 on win-lab (Arena 7.27.1, Mesa
  llvmpipe, no GPU) with all 14 controls and Opacity live (Print Speed and Tear Length
  under Mode Printing; `plugin-bench/arena/expect/receipt.json`). Nothing about a GPU
  or speed on Windows.
- **Footage judged by eye**, not measured; eight clips.
- **Not verified at 4K**, only benchmarked there.
- **No OpenFX port, no presets.** The user guide is `docs/USER-GUIDE.md`
  (the website builds its page and `docs/USER-GUIDE.pdf` from it).
- **The GPU wavefront's cost is an estimate**, not a measurement.
- **Nothing has been through a show.**

---

## The browser demo

`demo/` is served at https://receipt-demo.stoatworks-labs.com/ (a Worker route over a
proxied AAAA `100::` record; `deploy.yml` redeploys on a push to main). It is a page, not
the plugin, and its two halves are not equally faithful:

- **The shaders are the plugin's.** `kVertex`, `kSample` and `kDisplay` are spliced into
  `demo/plugin.js` unedited (`demo/tools/splice_shaders.py`); `demo/tools/check_shaders.py`
  compares them character for character and `tools/verify.sh` runs it.
- **The print engine is a JavaScript PORT** (`demo/printer.js`) of `Model.h`, `Controls.cpp`,
  `Printer.cpp` and the CPU half of `Receipt::ProcessOpenGL`, because a browser cannot run
  the C++. Doubles, the same order of operations, no fused multiply-add (matching
  `-ffp-contract=off`), floats wherever the C++ stores one.
- **The port is checked against the C++, exactly.** `demo/tools/check_port.sh` builds
  `refprint.cpp` against the unchanged `Printer.cpp` and `Controls.cpp` and against text it
  cuts out of `Receipt.h` (Stats, ParamID, every private member) and `Receipt.cpp`
  (floorDiv/floorMod, dataTones, printPaperRow, resetPrinting, and all of ProcessOpenGL, with
  GL stubbed), then compares: 12 control laws at 1,012 host values, HashInt, SlipAt over
  600,000 rows; every fired bit, float density and double heat over 566,784 dots of Printer;
  and 473 frames of Static and Printing (every block count, all three dithers and fits, both
  widths, slips at 1, the ring wrapped, a tear every few frames, a resize, a head change, a
  clock that stalls, jumps and runs backwards) — the paper, the uploaded rows and every
  uniform. All identical (2026-09-25). Mutations of printer.js it catches: Bayer's `>` as
  `>=`, a stall keeping its last strike, 23 lead rows, a tear keeping the dither's error, a
  slip-hash constant, heat reset each Printing frame, history control reassociated. Not
  caught: two one-ulp reassociations (Floyd–Steinberg's tone plus error, the lateral
  spread) that moved no bit and no float density on these cases, and three that are exact
  equivalents. What it cannot see: the sample pass (its tones are the script's), and the
  page's own GL calls.
- **Gaps the page states:** its tones come from the browser's GPU, so an ulp can move a
  dither decision and the dots are not evidence of the plugin's; Printing runs on the page's
  clock with its unit declared (no unit vote); Strobe Blocks is a dropdown of 1..8; no clip
  with transparency (the kit premultiplies, the plugin assumes straight alpha); the browser
  scales the canvas; no Perturb hooks, forced slip or About block; no audio path.

---

## Open questions

- **Should the read-back be a frame late?** A pixel-buffer read of last frame's image
  would take the stall out of the host's pipeline for a frame of latency on one layer.
  Worth measuring inside Resolume before deciding.
- **Should Heat Carry be per second** (a cooling time constant) rather than per row, so
  that a slow print carries less heat than a fast one, as a real head does?
- **Should dark footage have a driver curve?** Two blocks make it a banded grey, which
  is the budget's truth; a printer driver would also lighten the image for the dot gain.
  That would change what `--dither` states (the tone after the curve, not the clip's),
  and the spec asks for the input tone, so it is not here.
- **Should the budget re-strobe** (a dense row in more strobes, full density) as an
  option beside the sag?
- **Should Static carry the heat and error of the receipt above the window** instead of
  starting cold at the window's top?
- **Should Rotate turn either way**, and should Tile's copies have a gap (a cut between
  receipts)?

---

## Siblings

- **toner** — a print engine with a supply that runs short; the AGENTS.md shape; the
  alpha decision (the demo clips carry alpha).
- **filament** — the harness's clock and unit voting, the software-renderer pass, typed
  cues that step.
- **slope** — serial state along the scan, and why its chunked scheme does not reach a
  2-D error diffusion.
- **teletext** — a 1-bit device whose decisions are the CPU's, after a read-back.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **graticule** — the notes, and the provisional About.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
