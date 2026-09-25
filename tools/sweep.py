#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
control can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything.

This renders each parameter at both ends of its range against the same moving
test card and reports any that made no difference at all.

    python3 tools/sweep.py [--binary build/rctest] [--size WxH] [--jobs N] [--allow-no-gl]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**The card has to have greys, solids, lines and a moving bar.** Greys for the
dither, a big solid for the budget and the heat, thin lines for the history
control and the heat carry, text-like marks for the run-on below a mark.
`rctest --out` renders it.

**Some controls need another to mean anything.** Print Speed and Tear
Length only act in Printing (and Tear Length only once a receipt is long
enough to tear, so it runs at full speed); Slip needs rows to slip on, which
Static has; History Control only takes off heat that Heat Carry has left.
And Fit's two ends, Letterbox and Tile, differ only where the window is taller
than the image, which at 320x180 it is not, so Fit is compared with Rotate.
CONTEXT says what else has to be true for each, and uses nothing but the
plugin's own parameters.

**An option's range is its element count, an integer's range is real.** An
option parameter's range reads back 0..1 whatever its element count, so
`rctest --list` prints the count as the range, and the ends swept are the
first and last element; Strobe Blocks lists 1..8 and is swept at both.

**Every name must be unique.** `--set` finds a parameter by name.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 320, 180
FRAMES = 40

# What else has to be true for a parameter to have any effect at all. Keys
# beginning with "_" are HARNESS settings, not plugin parameters:
#   _frames     how many frames to render (default 40)
#   _low/_high  the two positions to compare, when not the range's ends
CONTEXT = {
    "Print Speed": {"Mode": 1},
    # 40 frames at full speed is 1333 rows: a 30 mm receipt (240 rows) tears
    # five times, a 600 mm one never.
    "Tear Length": {"Mode": 1, "Print Speed": 1},
    "History Control": {"Heat Carry": 0.8},
    # Tile is Letterbox with the margins above and below the image filled by
    # the next copies. At 320x180 the window (180 rows) sits wholly inside the
    # image (324 rows), so the two ends of the range print the same rows:
    # compare Letterbox with Rotate.
    "Fit": {"_high": 1},
}


def parameters(binary):
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)
    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def render(binary, path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [binary, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    for name, value in overrides.items():
        if not name.startswith("_"):
            args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    binary, scratch, pid, name, low, high, context = job
    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)
    a = render(binary, f"{scratch}/{pid}_lo.png", lo)
    b = render(binary, f"{scratch}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run cut off by a CI timeout
    # still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "rctest"))
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--allow-no-gl", action="store_true",
                    help="SKIP loudly, not FAIL, when the harness cannot create a GL context (CI)")
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(4, os.cpu_count() or 1)

    binary = str(pathlib.Path(args.binary).resolve())
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 1

    scratch = tempfile.mkdtemp(prefix="rcsweep")

    # One render first: a runner with no GL at all cannot sweep anything, and
    # that has to read as a skip, loudly, not as fourteen dead controls.
    probe = subprocess.run([binary, "--out", f"{scratch}/probe.png", "--size", "16x16", "--frames", "1"],
                           capture_output=True, text=True)
    if probe.returncode != 0 and "could not create an OpenGL" in (probe.stdout + probe.stderr):
        print("NO GL CONTEXT: the sweep could not render at all.")
        if args.allow_no_gl:
            print("SKIPPED (--allow-no-gl): no control was checked for liveness on this machine.")
            return 0
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters(binary):
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("buffer", "text"):
            skipped.append((name, "no scalar to sweep"))
            continue
        work.append((binary, scratch, pid, name, low, high, CONTEXT.get(name, {})))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1
    print(f"all {len(results)} swept parameters measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
