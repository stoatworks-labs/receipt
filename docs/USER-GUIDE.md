# Receipt user guide

Receipt is **the picture printed by a thermal receipt printer, for
[Resolume](https://resolume.com) Arena and Avenue**, as an FFGL effect. It does not draw a
"receipt look" over a clip. It prints each frame the way a thermal printer does: a line of tiny
heaters, eight to the millimetre, pressed against heat-sensitive paper, fired one dot row at a
time. There is no ink. The paper darkens where it gets hot enough, so the dither, the thickened
lines, the tails below every mark, the pale banded dark areas and the stretched rows are what the
heat does, not what somebody drew.

![The test card printed on an 80 mm receipt: dithered greys, vertical lines that run on below their ends, a full-width rule printed pale grey where the thin vertical lines stay black, a large solid printed as a mottled, sagging dark grey, and text-like marks each trailing a smear of heat below it](hero.png)

*The repo's test card through the plugin at its defaults, rendered by the offline harness rather
than captured from Resolume, over a dark background so the paper's edges show.*

> **Before you rely on this:** released at **v0.1.0**, and honestly early. The printer is
> measured rather than asserted, by a harness that drives the real plugin class and reads each
> claim back out of the picture, at two rasters and on Apple's software renderer: on ten flat
> greys the Bayer dither fires exactly its count of 64 dots, Floyd–Steinberg is within 0.0013 of
> the tone (against a derived bound of 0.0053) and Atkinson within 0.068 (bound 0.13), and
> Atkinson prints nothing at all below an eighth; at a heavy heat carry a 9-dot line widens to 11
> dots from the row the heat model predicts and runs on 11 rows below its end, and under full
> History Control stays 9 dots and runs on 3; a strobe block over the supply's budget prints at the
> stated sag to 10⁻⁷, and every step in density sits on a block boundary; forced slips move exactly
> the stated rows; every dot lands on its whole-pixel cell; Printing prints the same rows at 60, 50
> and 30 frames a second; and a resize mid-print keeps every dot. Seven deliberate faults are shown
> to make those checks fail, and all 14 controls are shown to change the picture.
>
> **The printer's constants are chosen, not measured from a real printer**: the paper's threshold
> and saturation, how far a heater's heat reaches the paper beside it, the supply's dot budget and
> the dye's optical density. The shape of the paper's response (nothing, then a steep rise, then
> saturation) is the real one; the numbers are this plugin's. The print runs on the CPU after a
> read-back from the GPU, about 1.2 ms a frame; **what that read-back's stall costs inside a real
> Resolume composition has not been measured.**
>
> It has **never been loaded into Resolume on macOS**: the one host it has run in there is the
> fleet's own test host, `oxbow`.
> On Windows, a build of this source loads, registers and renders in Resolume Arena 7.27.1, with
> every control matching what the plugin declares (the fleet's Arena gate, 9 of 9 checks) — on
> software rendering (win-lab, Mesa llvmpipe, no GPU), so that says nothing about a GPU or about
> speed. All 14 controls, and Arena's own Opacity, were shown moving the picture there (Print Speed
> and Tear Length with Mode set to Printing).
> Try it on a spare layer before you put it in a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Every download carries one effect, **SW Receipt**. Drop it into Resolume's effects folder and
restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. The effect then appears in the effects
browser as **SW Receipt**.

The macOS download is a universal build (Apple silicon and Intel), as a `.dmg` or a `.zip`.
It is **Developer ID-signed and notarised**, so the bundle simply loads. The Windows download is an x64 installer or a `.zip`. It is not code-signed,
so the installer trips SmartScreen once: **More info** → **Run anyway**.

---

## A thermal printer has no ink

The head of a receipt printer is a fixed row of resistive heaters: 576 of them across an 80 mm
roll, 384 across 58 mm, eight to the millimetre. The stepper motor pulls the paper past it one
dot row at a time, and for each row the firmware decides which heaters to fire. Where a heater
fires, the paper under it gets hot and its dye develops. So the physics of heat is the whole look,
and the plugin runs that physics row by row:

- **Tone is a dither.** A dot is on or off, so a grey has to be a pattern of dots: an 8 × 8 Bayer
  matrix, Floyd–Steinberg's error diffusion or Atkinson's, in raster order, as a printer's firmware
  does it.
- **A heater that fired is still warm on the next row.** Each heater keeps some of its heat, so
  the next dot there gets hotter than the first, and some of the heat reaches the paper beside it.
  Vertical lines thicken by a dot each side, every mark runs on in a tail below itself as its
  heaters cool, and dark areas block up. The firmware's **History Control** shortens a warm
  heater's strobe by the heat it predicts is left, which stops the thickening — but it cannot
  cool a heater that is not firing, so the tails below a mark's end stay.
- **The supply can only fire so many dots at once.** The head is strobed in blocks, and a block
  with many black dots draws more current than the supply can give, so its strobe sags. Dense rows
  print paler, and a row whose blocks differ in coverage bands at the block boundaries. Dark
  footage prints as a banded grey rather than a black slab.
- **The roller slips.** Now and then a row is repeated on the paper, stretching the picture, or
  the paper stalls while the data moves on and a dark band is overprinted.
- **Paper ages.** An old receipt is pale and yellowed, the print gone brown.

In **Static** mode each frame is printed as a whole receipt. In **Printing** mode the receipt
comes out of the slot in real time: each new row is printed from the frame on screen when the
head reached it, so the paper is a slit-scan of time, and it tears off at the tear length.

---

## Start here

Put SW Receipt on a layer with a clip that has **a subject on transparency** — the bundled demo
clips with the tank, the dancers, the astronaut, the rings, the metal sphere. Transparent areas
print as blank paper, so a subject on transparency prints as marks on a receipt. Busy full-frame
clips (the skulls) print well too. Out of the box you get an 80 mm roll, Floyd–Steinberg, a little
heat carried from row to row with half History Control, two strobe blocks, Static mode, a little
age and white paper.

Then:

1. **Mode → Printing, Fit → Tile.** The receipt now comes up out of the slot at about 42 mm/s,
   printed from whatever is on screen as the head reaches each row, so anything that moves smears
   down the paper. It tears off at about 121 mm and a new one starts. **Print Speed → 0.25** for a
   slow cheap printer (17.5 mm/s), or higher for a fast one. (In Letterbox, each receipt carries
   one copy of the frame just below its torn edge and blank paper after it; Tile prints the frame
   on every row, which is what you usually want in Printing.)
2. **Mode → Static, Dither → Bayer, then Atkinson.** Three kinds of dot pattern. Atkinson's
   throws a quarter of the error away, so it is punchier: pale greys go to white and dark greys to
   black.
3. **History Control → 0, Heat Carry → 0.95**, on a clip with thin lines (the rings). Every line
   thickens and trails a long tail below it. Then bring **History Control** back up to 1: the
   lines thin again, and the tails below their ends stay.
4. **Strobe Blocks** on a dark full-frame clip (the AV Beat loops). At 2, the dark prints as a
   banded grey. At 1 the whole head is one block and it sags nearly to white. At 4 it prints near
   black, and at 8 each block is under the budget and the dark prints solid.
5. **Age → 1**, then **Paper Tint → Canary** or **Pink**, for an old receipt out of a drawer.

**Every slider is declared to the host as 0 to 1.** The value each position stands for is given
with each control below.

---

## The Printer group

**Width** — **58 mm** or **80 mm**; 80 mm by default. The roll, and so the head: 384 or 576 dots.
The head spans the output's width at a whole number of pixels per dot, centred: at 1080p an 80 mm
head is 3 px dots (1,728 px wide) and a 58 mm head is 5 px dots (1,920 px). An output narrower than
the head (under 576 px) cuts off the head's ends. Changing Width starts new paper.

**Dither** — **Bayer**, **Floyd-Steinberg** or **Atkinson**; Floyd-Steinberg by default. How a
grey becomes dots. **Bayer**: an 8 × 8 ordered matrix, a regular crosshatch. **Floyd-Steinberg**:
each dot's error is spread to the next dot and the three below (7, 3, 5 and 1 sixteenths), the
firmware's usual. **Atkinson**: an eighth each to six neighbours, a quarter of the error thrown
away, so light and dark ends go clean. The tone is the clip's Rec. 601 luma over white paper by
its alpha, so a transparent area prints nothing.

**Density** — 0 to 1, default 0.5. The strobe's energy: half the paper's saturation at 0, one and
a half times it at 1. At the default a lone dot just saturates; lower and single dots print grey,
higher and the heat spreads further.

**Heat Carry** — 0 to 1, default 0.4. How much of a heater's heat is left at the next row: the
carry is 0.9 × the value (0.36 at the default). 0 is a head that cools completely between rows.
Toward 1, lines thicken, marks trail long tails, and dark areas block up.

**History Control** — 0 to 1, default 0.5. How much of the predicted leftover heat the firmware
takes off a firing heater's strobe. At 1 a firing heater always delivers exactly its nominal
energy, so lines stop thickening; nothing can cool a heater that is not firing, so the tail below
the end of a mark stays.

**Strobe Blocks** — 1 to 8, default 2. The head is strobed in this many blocks across its width;
a block that fires more than 96 dots sags in proportion (half its energy at 192 dots). Fewer
blocks, more sag: at 1 dark footage prints pale, at 2 it prints a banded grey, and there is a cliff
between 2 and 3 where the carried heat takes over and the dark blocks up to near black. Blocks are
a whole number of dots wide, so 5 and 7 blocks differ by a dot.

## The Feed group

**Print Speed** — 0 to 1, default 0.4. Printing only. The paper speed: 2 + 248 × value² mm/s,
about 42 mm/s at the default and 250 at the top, at 8 rows a millimetre. At 1080p the window is
360 rows, 45 mm of paper, so at the default a row crosses the screen in about a second.

**Slip** — 0 to 1, default 0.15. How often the roller slips: up to one row in fifty at 1
(0.02 × value²). Half the slips repeat a row on 1 to 4 more paper rows, stretching the picture;
half stall the paper while 1 to 4 more rows of data strike the same paper row, overprinting a dark
band and skipping those rows. The slips are fixed to the paper's rows, so in Static a still clip
has the same slips every frame. They show on edges and thin lines; in a solid black area a
repeated black row changes nothing.

**Mode** — **Static** or **Printing**; Static by default. **Static**: every frame is printed as a
receipt centred on the window, afresh from its first visible row with a cold head. **Printing**:
the receipt scrolls up out of the slot at the bottom at Print Speed; each new row is printed from
the frame on screen when the head reached it, the head's heat and the dither's error carried from
row to row; at Tear Length it tears (a new receipt, the dither reset, the head still warm). The
first frame after the effect starts prints nothing: it sets the clock.

**Tear Length** — 0 to 1, default 0.4. Printing only. The receipt's length before it tears:
30 + 570 × value² mm, about 121 mm at the default and 600 mm at the top. In Letterbox the frame
sits 3 mm below the torn edge, so a long receipt is mostly blank paper; use Tile.

## The Paper group

**Age** — 0 to 1, default 0.15. The print fades and turns brown, the stock yellows, and near the
end where it was held the background develops a little grey. 0 is a fresh receipt.

**Paper Tint** — **White**, **Ivory**, **Canary**, **Pink**, **Blue** or **Green**; White by
default. The stock's colour, as the coloured rolls sold for tills and tickets. The colours were
chosen by eye.

**Fit** — **Letterbox**, **Rotate** or **Tile**; Letterbox by default. How the clip lands on the
paper. **Letterbox**: the clip's width fits the head, so a 16:9 clip is 324 rows of an 80 mm
receipt. **Rotate**: the clip is turned so its width runs down the paper (its top to the paper's
right edge; there is no option for the other way), 1,024 rows long. **Tile**: Letterbox, with the
copies end to end along the receipt.

**Mix** — 0 to 1, default 1. Blends the whole output with the source. The paper is opaque and off
the paper is transparent, so the layer below shows round the receipt; Mix fades colour and alpha
together.

---

## How it works

1. **Sample** (GPU). The clip to the head's dots: each dot is the exact area average of the
   source pixels under it, as Rec. 601 luma over white paper by the clip's alpha.
2. **Read-back.** The head's image, 576 × 324 values for a 16:9 clip on 80 mm, to the CPU.
3. **Print** (CPU, in double precision, one dot row at a time). Dither the row to fire bits;
   shorten each warm firing heater's strobe by the History Control; sag each strobe block over
   the budget; heat each heater (a one-pole: the heat left from the last row times Heat Carry,
   plus the strobe); the paper under each heater sees its own heat plus 0.22 of each neighbour's
   and develops by a smooth step from 0.35 to 1; the slips repeat or stall rows.
4. **Upload** the rows to a texture.
5. **Display** (GPU). Each dot is a whole number of output pixels, drawn as the paper colour
   darkened by the dye's density (optical density up to 1.3), with Age, the tint and the torn
   edge's sawtooth.

The dither and the print are on the CPU because Floyd–Steinberg and Atkinson are serial in both
directions (a dot depends on the dot before it and on the row above), and GLSL 4.10, which FFGL
plugins are written in, cannot write anywhere but its own pixel. A GPU version would be over a
thousand dependent draws a frame.

---

## Performance

Measured by the offline harness on an M4 Max, best of three runs of 60 frames, `glFinish` both
sides, on a machine shared with other work:

| | 1280 × 720 | 1920 × 1080 | 3840 × 2160 |
| --- | --- | --- | --- |
| Static, Floyd–Steinberg (default) | 1.7 ms | 1.9 ms | 2.6 ms |
| Static, Bayer | 0.9 ms | 1.1 ms | 1.4 ms |
| Printing | 0.4 ms | 0.4 ms | 1.1 ms |

The print engine itself is about 1.2 ms a frame for error diffusion and 0.4 ms for Bayer whatever
the output's size, because it runs at the head's resolution; in Printing it only prints the new
rows. It runs **on the render thread**, after a read-back that waits for the GPU to finish
everything queued before it, the host's own layers included. **That stall inside a real Resolume
composition has not been measured**; on a busy composition it may cost more than these figures.
Nothing was timed on Windows.

---

## If it looks wrong

**Dark footage prints pale grey with bands.** That is the supply sagging at two strobe blocks.
Raise Strobe Blocks to 4 or 8 for black, or lighten the clip with a brightness effect ahead of this
one.

**Dark footage prints almost white.** Strobe Blocks is 1. Raise it.

**Everything is a black slab.** Heat Carry is high and History Control low, or Strobe Blocks is high
on a dark clip. Lower Heat Carry or raise History Control.

**Every mark has a smear below it.** That is the heat left in the heaters. Lower Heat Carry; History
Control does not remove it.

**Printing shows mostly blank paper.** Fit is Letterbox, which puts one copy of the frame at the top
of each receipt. Use Tile.

**Printing shows black, or the picture is not moving.** The receipt has just torn and a new one is
coming up from the slot, or Mode is Static.

**The picture is stretched in places, or has dark bands.** That is Slip. Lower it to 0.

**The receipt is narrower than the frame, with black either side.** An 80 mm head is 576 dots at a
whole number of pixels per dot; at 1920 px that is 1,728 px. 58 mm fills 1,920.

**SW Receipt is not in the effects browser.** Check the folder under Installing, and that Resolume
was restarted.

**The effect does nothing at all.** A shader that will not compile looks exactly like that, and the
real message is in the log:

```
macOS    ~/Library/Logs/receipt/receipt.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\receipt\logs\receipt.YYYY-MM-DD.log
```

---

## Known limits

- **Never loaded into Resolume on macOS**, and nothing has driven the controls in a host there. How
  Resolume's clock arrives, whether it leaves a pixel buffer bound, and what the read-back costs
  inside a busy composition are untested.
- **The printer's constants are chosen, not measured**: the paper's threshold 0.35 and saturation
  1, the heat's lateral spread 0.22, the budget of 96 dots a block, the optical density 1.3, and the
  paper and dye colours.
- **Feed jitter is not modelled**: slips are whole rows, repeats and stalls.
- **A dense row sags; it is never re-strobed.** Real firmware can split a dense row into more
  strobes (full density, slower); this does not.
- **Heat Carry is per row, not per second.** A real head cools in time, so a slow print carries
  less heat than a fast one; here Static and every Print Speed heat alike.
- **The heat reaches one heater each side**, enough to thicken a line by a dot a side; a wider bleed
  is not modelled.
- **Static starts every frame with a cold head** at the window's top: rows above the window are not
  printed, so their heat and error are not carried in.
- **Rotate turns the clip one way only.**
- **Only ever run on an Apple M4 Max**, although the macOS build contains an Intel slice.
- **No presets, no OpenFX version.**
- **There is a browser demo** at [receipt-demo.stoatworks-labs.com](https://receipt-demo.stoatworks-labs.com).
  It is a port to a web page, not the plugin: the shaders run in WebGL2, and the print engine is
  rewritten in JavaScript. The page lists what it does not reproduce.

---

## About

The last group, **About**, carries the plugin's name, version, licence and maker, and buttons that
open this user guide ([stoatworks-labs.com/software/receipt/guide/](https://stoatworks-labs.com/software/receipt/guide/)),
the project page, the source on GitHub and the support page in your browser.

## Reporting something

[github.com/stoatworks-labs/receipt/issues](https://github.com/stoatworks-labs/receipt/issues). A
screenshot, the Printer and Feed settings, and the composition's resolution and frame rate are
usually enough. If the effect did nothing, attach the log.
