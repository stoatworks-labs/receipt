# Attributions

Receipt is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

This is a PROVISIONAL hand copy (2026-09-25), adapted from toner's. The fleet's
version is generated — the master lists live in the `stoatworks-backend` repo
and are pushed out by `scripts/sync-attributions.py`; the release step registers
this project and re-runs the sync, which replaces this file.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks filament, toner and slope

<https://github.com/stoatworks-labs/filament>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1; sliders ramp and options, booleans, integers and events step), the software-renderer pass, the negative-control pattern, --offline, check-shaders.sh, the verify script, the sweep and the CI workflows are filament's and toner's, by way of slope and clamp.

### The host clock's unit voting — Stoatworks readout, by way of filament

<https://github.com/stoatworks-labs/filament>  
Licence: MIT  
Copyright: Stoatworks Labs

Deciding whether the host's clock is in seconds or milliseconds from its rate against a steady clock, and running on the steady clock until it is decided.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of toner.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The dithers

Ordered dithering by the recursive index matrix is B. E. Bayer's ("An optimum method for two-level rendition of continuous-tone pictures", IEEE ICC, 1973). Error diffusion with the 7, 3, 5, 1 weights is R. W. Floyd and L. Steinberg's ("An adaptive algorithm for spatial greyscale", Proc. SID 17, 1976). The six-neighbour eighths that deliberately lose a quarter of the error are Bill Atkinson's, from the original Macintosh. The algorithms are the published ones; the bounds on their tone and the code are this repo's.

### Thermal printing

A line of resistive heaters against leuco-dye paper, history (heat) control in the firmware, strobing the head in blocks because the supply cannot fire every dot at once, and paper that fades with light and darkens with heat are the standard account of direct thermal printing, as thermal printhead makers describe their parts. Nothing was copied from any datasheet; the energy model, the budget law, the response curve and every constant here are this repo's own and are stated in `source/Model.h` as assumptions, not measurements.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.601** — The luma weights (0.299, 0.587, 0.114) the tone is read from the clip with.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
