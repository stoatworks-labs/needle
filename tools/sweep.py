"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at both ends of its range against a context that makes
it mean something, and report any that made no difference at all.

    python3 tools/sweep.py

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**A meter with no audio does nothing.** Every render here injects a level with
`--level`, because the plugin's whole output is a function of one. A sweep run
without it would report two thirds of the controls dead, correctly.

**Half of these controls are only visible while something is MOVING.** Rise,
Fall, Overshoot, Standard and Persistence are ballistics: at steady state every
setting of them puts the pointer in the same place, because that is what steady
state means. They are swept during a transient -- `_burst` gives the level a
finite length and `_frames` decides how long after the onset the shot is taken.
A ballistic control swept on a held tone is provably dead and the sweep would be
right to say so.

**Most controls only act on one meter.** The hold bar belongs to the bargraph,
Persistence to the magic eye, the scale to the two dials. Every parameter is
swept with the `Type` that reads it, listed in `CONTEXT`; a parameter missing
from that table is swept on the defaults and will be reported dead if the
defaults do not read it. That is the table doing its job.

**Rotation's two ends are the same rotation.** -180 and +180 degrees produce an
identical picture, so it is swept from centre to one end.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match. `ndtest --names` fails on a duplicate for that reason.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press. `ndtest --list` marks them, and marks the
audio buffer, which is the host's to write and not the operator's.
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
BIN = str(ROOT / "build" / "ndtest")
SCRATCH = tempfile.mkdtemp(prefix="ndsweep")

WIDTH, HEIGHT = 640, 360
FRAMES = 90
LEVEL = -18.0          # dBFS: the default Reference Level, so 0 on the scale

SKIP = {}

# The four instruments. `_level`, `_frames` and `_burst` are harness flags, not
# parameters; anything starting with an underscore is stripped before --set.
DIAL = {"Type": 0}
PPM = {"Type": 1}
BAR = {"Type": 2}
EYE = {"Type": 3}

# A transient: loud for 150 ms, then nothing, with the shot taken while the
# movement is still going somewhere.
def moving(base, frames, burst=150):
    d = dict(base)
    d["_burst"] = burst
    d["_frames"] = frames
    return d

CONTEXT = {
    "Type": {"_high": 3},
    "Count": {},
    "Reference Level": DIAL,
    "Sensitivity": DIAL,
    # One bin carrying 0.126: read as a magnitude that is the level, read as a
    # power it is its square root. Any level but 0 and 1 separates them.
    "Bin Law": DIAL,

    # Ballistics. All five need the pointer to be in motion when the shot is
    # taken -- see the header.
    "Standard": dict(moving(DIAL, 14), **{"Rise": 0.05, "Overshoot": 0.95}),
    "Rise": dict(moving(DIAL, 10), **{"Standard": 0}),
    "Fall": dict(moving(DIAL, 40), **{"Standard": 0}),
    # 400 ms after the onset is where the overshoot peak lives.
    "Overshoot": dict(moving(DIAL, 24, burst=2000), **{"Standard": 0}),
    "Peak Hold": moving(BAR, 60),
    "Hold Decay": dict(moving(BAR, 90), **{"Peak Hold": 0.25}),

    "Face Red": DIAL,
    "Face Green": DIAL,
    "Face Blue": DIAL,
    "Needle Red": DIAL,
    "Needle Green": DIAL,
    "Needle Blue": DIAL,
    "Scale Style": DIAL,
    "Lamp": DIAL,
    "Glass": DIAL,
    "Wear": DIAL,
    # The phosphor lags what the tube is doing, so it is invisible unless the
    # tube is doing something -- and it is ALSO invisible once the shadow has
    # hit either end of its travel, because both ends are clamped and a lagged
    # clamp is the same clamp. So this one is swept on the ONSET, with the
    # shot taken 217 ms in, while the shadow is still part way across. Swept
    # after a burst instead it reported dead, correctly: by then the tube is
    # shut at one end whatever the phosphor is doing.
    "Persistence": dict(EYE, **{"_frames": 13, "_level": -22}),

    "Size": DIAL,
    "Position X": DIAL,
    "Position Y": DIAL,
    # -180 and +180 degrees are the same rotation.
    "Rotation": dict(DIAL, **{"_low": 0.5}),
    "Background": DIAL,
    "Back Red": dict(DIAL, **{"Background": 1}),
    "Back Green": dict(DIAL, **{"Background": 1}),
    "Back Blue": dict(DIAL, **{"Background": 1}),
    "Mix": dict(DIAL, **{"Background": 1}),
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
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
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides):
    frames = overrides.get("_frames", FRAMES)
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}",
            "--frames", str(frames), "--level", str(overrides.get("_level", LEVEL))]
    if "_burst" in overrides:
        args += ["--burst", str(overrides["_burst"])]
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
    pid, name, low, high, context = job

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run cut off by a CI timeout still
    # says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    args = ap.parse_args()
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind == "buffer":
            skipped.append((name, "the host's spectrum; the harness injects it with --level"))
            continue
        if kind == "text" or name in SKIP:
            skipped.append((name, SKIP.get(name, "free text")))
            continue
        work.append((pid, name, low, high, CONTEXT.get(name, {})))

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
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
