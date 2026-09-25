# receipt

A thermal receipt printer as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the model (`Model.h`, `Printer.cpp`), the feed
(`Receipt.cpp`), the shaders, the control laws, the defaults or the harness's
tolerances.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/rctest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Strobe Blocks=6" --set "Heat Carry=0.8" --set "Mode=1"`
  (0..1 for sliders, the element index for options, the integer for Strobe Blocks)
- List parameters, kinds, defaults and ranges: `./build/rctest --list`
- The exact GLSL the plugin compiles: `./build/rctest --dump-shaders DIR`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps seconds: Printing
  scrolls in that time) and an optional `--script` of `frame Parameter Name value`
  cues. A slider ramps linearly between a name's cues; an option, a boolean and an
  integer STEP (they hold the last cue at or before the frame); an event fires on its
  cue frame only; every track holds before its first cue and after its last. A cue
  naming no parameter exits 2 before any frame; a partial frame at the end of stdin
  ends the stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/rctest --pipe --size 1920x1080 --fps 30 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + reserved words + the
  offline checks + every rendered check at 320x180 AND 1280x720 AND on the software
  renderer + the --pipe contract + the sweep + the bench + the bundle; about 30 s here)
- The dither's tone on flat greys, within each dither's bound: `./build/rctest --dither`
- A line thickens by what the heat carry predicts, not under history control: `./build/rctest --history`
- A block over the budget sags as stated; banding on block boundaries: `./build/rctest --budget`
- A forced repeat or stall moves exactly the stated rows: `./build/rctest --slip`
- Every dot on its whole-pixel cell: `./build/rctest --grid`
- Printing in elapsed time at 60/50/30 fps, and the tear: `./build/rctest --printing`
- A mid-print resize keeps the paper: `./build/rctest --resize`
- The output alpha: `./build/rctest --alpha`
- The checks can fail: `./build/rctest --negative`; one perturbation verbosely:
  `./build/rctest --history --perturb 1` (bits in `Model.h`)
- No GL (what CI runs first): `./build/rctest --offline` = `--laws --names --cues`
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- CI's renderer on this Mac: `RCTEST_RENDERER=software ./build/rctest --dither --size 320x180`
- Shaders through glslc: `tools/check-shaders.sh build/rctest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost by stage: `./build/rctest --bench` (720p, 1080p, 4K; best of three; shared machine)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Receipt.bundle`

## Notes
- **The print is the CPU's.** The sample pass (GPU) fits the clip to the head's dots and
  is read back; `Printer.cpp` dithers, applies history control, the supply's budget, the
  heat and the paper's response, in double, row by row; the display pass (GPU) draws the
  dots. Floyd–Steinberg and Atkinson are serial in both directions and GLSL 4.10 has no
  image stores (AGENTS.md has the measurements). 1.2 ms a frame whatever the raster.
- **The core is built with `-ffp-contract=off`** so the arm64 and x86_64 slices dither
  alike (a fused multiply-add is one ulp, and one ulp is a different dot). Measured
  identical under Rosetta with it; also identical without it, on the frames tried — it
  is a precaution.
- **The printed paper lives at head resolution**, a ring of 4096 rows of N dots on the
  CPU and in a texture whose size depends only on the head. An output resize keeps it;
  a change of Width (a new head) clears it.
- **Printing's time is the host's, frame-relative, in double**, with the fleet's unit
  voting (seconds or milliseconds); a frame prints at most a quarter-second of rows; the
  first frame primes the clock and prints nothing. `SetClockScaleForTest(1)` declares
  the harness's unit.
- **Static centres the receipt, and the image on it, on the window; Printing starts the
  image 24 rows (3 mm) below the torn edge.** Fit's Letterbox and Tile print the same
  rows wherever the window lies inside the image (at 320x180 always), so the sweep
  compares Letterbox with Rotate.
- **The paper is opaque and off the paper is transparent black** (alpha 0), whatever the
  clip's alpha; Mix fades colour and alpha together. A transparent clip prints blank.
- **The display takes `mix` only below Mix 1**, so a full-wet frame never depends on
  `mix( a, b, 1.0 ) == b`, which GLSL does not promise.
- **A source-aligned dot edge needs c W / N to be an integer**: at 1280 and 320 wide and
  576 dots that is every ninth dot, so the harness's lines and bands are 9 dots wide.
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp`; Strobe Blocks is a real `FF_TYPE_INTEGER` with `SetParamRange(1, 8)`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `receipt_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include it by hand.
- Parameter names are unique and 16 characters or under (`History Control` is 15).
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `RC01`, display name `SW Receipt`.
- Git from a session: `git -C /Users/allansargeant/dev/receipt …`, literal path — the
  worktree guard reads the session's primary directory otherwise.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS, plus an
  `oxbow` load. Footage seen only through `--pipe` (eight of Resolume's demo clips), by eye.
- No Windows build has run (CI cannot run yet: no GitHub repo). No OpenFX port, no
  browser demo, no presets, no user guide.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are PROVISIONAL hand copies with `guide=""`
  (three About buttons); the release step registers the project and re-runs the syncs.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/receipt/receipt.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\receipt\logs\receipt.YYYY-MM-DD.log   (Windows)
