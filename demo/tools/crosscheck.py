"""The browser demo checked in a real browser, and its VFD against the plugin's
harness: the same raster, the same settings, the same audio, two renderers.

    python3 demo/tools/crosscheck.py [--ndtest build/ndtest] [--keep DIR]

Needs Google Chrome, numpy and stoatworks-backend's `release/cdpshot.py`
(CDPSHOT=path overrides ~/Projects/infrastructure/stoatworks-backend/release/
cdpshot.py); the comparison also needs a built ndtest (cmake --build build
--target ndtest). Not run by tools/verify.sh: it needs a GPU and a browser.
(woofer's crosscheck, which is mirrorball's, with needle's clock and audio.)

demo/ is served from here on a free port WITH demo/_headers' policy applied to
every response, as Cloudflare applies it, so a Content-Security-Policy that
refuses the WebAssembly fails here and not only in production. Chrome runs
headless on the real GPU (ANGLE on Metal). Every getUserMedia and
enumerateDevices call is trapped from outside before the page loads; none may
happen.

Part 1, the page (always):

  - the four meters: each Type draws, each differs from the others, and the
    picture moves while playing;
  - Type = VFD: the compiled plugin draws, segments are lit, earshot reports
    the feed at 48 kHz, the lamp is green, no device was listed or selected;
  - a VFD control changes the picture: Bands (31 Thirds to 10 Octaves) and
    Shutter (the eye to 1/1000 s, which must add the scan's dark bands);
  - every VFD signal runs, "Silence" turns the lamp red after 3 s and
    "Nothing fed" leaves the display dark with the lamp amber;
  - back to the VU after the VFD, the meter still draws;
  - the photosensitivity warning is at the top; the caution line shows under
    the picture with the VFD only, and says Shutter is above 0 when it is; a
    link opening on the VFD with Shutter above 0 opens paused, one at Shutter
    0 plays, and in embed mode neither the warning nor that rule applies;
  - nothing logged an error or a warning, the panel check included (vfd.js
    compares the hand-written panel with the compiled declarations and logs
    each difference as an error), and no device API was called.

Part 2, the pictures (with --ndtest). Each case is rendered twice:

  - by the PAGE at ?size=960x540&type=4&vfdsignal=programme: paused, the
    case's settings applied, Restart pressed (a new instance at time 0, its
    feed begun), then Step pressed N times, one frame of 1/60 s each, so the
    page has fed 800 N samples of ndtest's programme at 48 kHz and its clock
    reads N/60. The canvas is read back (preserveDrawingBuffer forced on from
    outside; nothing in the page is changed for this).
  - by the PLUGIN: `ndtest --pipe --fps 60 --size 960x540 --level -18 --set
    ...` (the real NeedlePlugin in a GL 4.1 context, ForceSecondsClock), with
    every control as the page's own hostValue() sends it, the float32 printed
    to 9 digits. ndtest feeds frame f the samples up to (f + 1) x 800 before
    drawing it, so its frame N - 1 has had the same 800 N samples as the page;
    that frame is compared. Its clock reads (N - 1)/60 there, 1/60 s behind
    the page's. Only the scan's phase reads the clock (the dots hold in
    earshot's audio time), so the Shutter case uses a Multiplex Rate with a
    whole number of scans in 1/60 s, 300 Hz, where that 1/60 s cannot show.

The two outputs are compared byte for byte (RGB, and alpha separately). There
is no pass mark the plugin's own checks give for this; the script prints the
difference and fails only past max 8 codes or 1% of values off. A negative
control -- ndtest with Rotation one slider step (0.001, 0.36 degrees) from the
page's on the defaults case -- must differ from the page by more than the
defaults case does, so the comparison is shown to see a slip that small.

Exit 1 when a page check fails, a case is past that mark, the negative control
passes, or the page logs an error or a warning.
"""
import argparse
import base64
import functools
import http.server
import json
import math
import os
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, ".."))
sys.dont_write_bytecode = True

CDPSHOT = os.environ.get("CDPSHOT", os.path.expanduser("~/Projects/infrastructure/stoatworks-backend/release/cdpshot.py"))
sys.path.insert(0, os.path.dirname(CDPSHOT))
import cdpshot  # noqa: E402

# The real GPU: SwiftShader rounds differently and is slow; --disable-gpu has no WebGL.
cdpshot.GL_FLAGS = ["--use-gl=angle", "--use-angle=metal", "--ignore-gpu-blocklist"]

SIZE = "960x540"
MULTIPLEX_300 = math.log10(6.0)  # 300 Hz: five whole scans in 1/60 s
SHUTTER_1000 = math.log(24.0 / 1000.0) / math.log(24.0 / 8000.0)  # Controls.h's ShutterParam(1/1000)

# name, frames N, settings by the page's ids (a number is the host's 0..1, an
# option its index).
CASES = [
    ("defaults, 31 Thirds, 4 s of the programme", 240, {}),
    ("10 Octaves", 180, {"bands": 1}),
    ("Shutter 1/1000 s at 300 Hz multiplex", 180, {"multiplex": MULTIPLEX_300, "shutter": SHUTTER_1000}),
    ("Reference -30 dBFS, Sensitivity +6 dB, Peak Hold 0", 150, {"reference": 0.25, "sensitivity": 0.75, "peakHold": 0.0}),
    ("Rotation 30 deg, Size 0.8, Background 1, lamp off", 120,
     {"rotation": 0.5 + 30 / 360, "instrumentSize": 0.8, "posX": 0.45, "background": 1.0, "statusLamp": 0}),
]

TRAP = """
(() => {
  const real = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (kind, attrs) {
    if (kind === 'webgl2') attrs = Object.assign({}, attrs || {}, { preserveDrawingBuffer: true });
    return real.call(this, kind, attrs);
  };
  window.__deviceCalls = [];
  const md = navigator.mediaDevices;
  if (md) {
    for (const name of ['getUserMedia', 'enumerateDevices', 'getDisplayMedia']) {
      if (typeof md[name] === 'function') {
        md[name] = function () { window.__deviceCalls.push(name); return Promise.reject(new Error('trapped')); };
      }
    }
  }
})();
"""

CANVAS = "document.querySelector('canvas.stage__gl')"
KEY = f"(() => {{ const c = {CANVAS}; return c ? `${{c.dataset.vfdFrames || 0}}:${{c.dataset.vfdInstances || 0}}:${{window.__frames || 0}}` : ''; }})()"

# A frame counter for the meters too (the VFD keeps its own in the dataset).
COUNT_FRAMES = """
(() => {
  window.__frames = 0;
  const tick = () => { window.__frames += 1; requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();
"""

HOST = """(() => {
  const d = window.needleDemo, out = [];
  for (const p of d.PARAMS) {
    const h = d.vfd.hostValue(p, d.mounted.params.get(p.id));
    if (h !== null) out.push([p.name, h]);
  }
  return out;
})()"""

STEP = """(async () => {
  const step = [...document.querySelectorAll('.transport .btn')].find((b) => b.textContent === 'Step');
  for (let i = 0; i < %d; i += 1) {
    step.click();
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
  }
  const d = window.needleDemo;
  return { time: d.mounted.state.time, stats: d.renderer.vfd.stats() };
})()"""

STATS = "(() => { const s = window.needleDemo.renderer.vfd.stats(); return s && { lit: s.columns.reduce((a, c) => a + c.lit, 0), dots: s.columns.filter((c) => c.dot >= 0).length, lamp: s.lamp, state: s.state, rate: s.rate, thirds: s.thirds, fed: s.fed, devices: s.devices, calls: s.calls, instances: s.instances }; })()"


class Page(cdpshot.Chrome):
    """cdpshot's Chrome, keeping the events it would discard."""

    def __init__(self):
        self.events = []
        super().__init__()
        self.call("Runtime.enable")
        self.call("Log.enable")
        self.call("Page.addScriptToEvaluateOnNewDocument", {"source": TRAP})

    def call(self, method, params=None):
        self.msg_id += 1
        want = self.msg_id
        self.ws.send(json.dumps({"id": want, "method": method, "params": params or {}}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == want:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg)

    def run(self, expression):
        r = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True, "awaitPromise": True})
        if "exceptionDetails" in r:
            raise RuntimeError(f"the page threw: {json.dumps(r['exceptionDetails'])[:400]}")
        return r.get("result", {}).get("value")

    def load(self, url):
        self.call("Page.navigate", {"url": url})
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                if self.run("Boolean(window.needleDemo && window.needleDemo.renderer)"):
                    self.run(COUNT_FRAMES)
                    time.sleep(0.3)
                    return
            except RuntimeError:
                pass
            time.sleep(0.25)
        raise RuntimeError(f"{url} never mounted")

    def after(self, expression):
        """Run, then wait for the frame it causes (the page is paused)."""
        before = self.run(KEY)
        self.run(expression)
        deadline = time.time() + 20
        while time.time() < deadline:
            if self.run(KEY) != before:
                time.sleep(0.05)
                return
            time.sleep(0.03)
        raise RuntimeError("no frame after " + expression[:80])

    def pause(self):
        self.after("document.querySelector('.btn--play').click()")
        if self.run("window.needleDemo.mounted.state.playing"):
            raise RuntimeError("the page did not pause")

    def set(self, settings):
        for pid, value in settings.items():
            self.after(f"window.needleDemo.mounted.params.set({json.dumps(pid)}, {json.dumps(value)})")

    def restart(self):
        self.after("[...document.querySelectorAll('.transport .btn')].find((b) => b.textContent === 'Restart').click()")

    def canvas(self):
        """The canvas's bytes, rows top first."""
        r = self.run(f"""(() => {{
          const c = {CANVAS}; const gl = c.getContext('webgl2');
          const w = c.width, h = c.height, px = new Uint8Array(w * h * 4);
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
          const out = new Uint8Array(w * h * 4);
          for (let y = 0; y < h; y += 1) out.set(px.subarray((h - 1 - y) * w * 4, (h - y) * w * 4), y * w * 4);
          let s = '';
          for (let i = 0; i < out.length; i += 0x8000) s += String.fromCharCode.apply(null, out.subarray(i, i + 0x8000));
          return {{ w, h, b64: btoa(s) }};
        }})()""")
        w, h = r["w"], r["h"]
        return np.frombuffer(base64.b64decode(r["b64"]), np.uint8).reshape(h, w, 4).astype(np.int16)

    def problems(self):
        out = []
        for e in self.events:
            m, p = e["method"], e.get("params", {})
            if m == "Runtime.exceptionThrown":
                out.append(json.dumps(p.get("exceptionDetails", {}))[:300])
            elif m == "Runtime.consoleAPICalled" and p.get("type") in ("error", "warning", "assert"):
                out.append(" ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", []))[:300])
            elif m == "Log.entryAdded" and p.get("entry", {}).get("level") in ("error", "warning"):
                out.append(f"{p['entry'].get('source')}: {p['entry'].get('text')}"[:300])
        return out


def site_headers():
    """demo/_headers' rules for /*, as Cloudflare applies them to every response."""
    headers, inside = [], False
    with open(os.path.join(DEMO, "_headers")) as handle:
        for line in handle:
            if not line.strip() or line.startswith("#"):
                inside = False if not line.strip() else inside
                continue
            if not line[0].isspace():
                inside = line.strip() == "/*"
                continue
            if inside and ":" in line:
                key, value = line.strip().split(":", 1)
                headers.append((key.strip(), value.strip()))
    if not any(k == "Content-Security-Policy" for k, _ in headers):
        raise RuntimeError("demo/_headers has no Content-Security-Policy for /*")
    return headers


class Served(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".wasm": "application/wasm", ".js": "text/javascript", ".mjs": "text/javascript"}
    headers_for_site = []

    def end_headers(self):
        for key, value in self.headers_for_site:
            self.send_header(key, value)
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *args):
        pass


def serve():
    Served.headers_for_site = site_headers()
    handler = functools.partial(Served, directory=DEMO)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}/"


def save(keep, stem, array, scale=1):
    if not keep:
        return
    from PIL import Image
    os.makedirs(keep, exist_ok=True)
    Image.fromarray(np.clip(array[..., :3] * scale, 0, 255).astype(np.uint8)).save(os.path.join(keep, f"{stem}.png"))


#---------------------------------------------------------------------------
# Part 1: the page.
#---------------------------------------------------------------------------

def page_checks(page, base, keep):
    failures = 0

    def check(ok, what, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'}  {what}{f' -- {detail}' if detail else ''}")

    # The four meters.
    page.load(f"{base}?size={SIZE}")
    a = page.canvas()
    time.sleep(1.0)
    b = page.canvas()
    check(np.abs(a - b).max() > 0, "the VU moves while playing", f"max {np.abs(a - b).max()} codes between two reads 1 s apart")
    page.pause()
    pictures = {}
    for index, name in enumerate(["VU", "PPM", "Bargraph", "Magic Eye"]):
        page.set({"type": index})
        pictures[name] = page.canvas()
        drawn = int((pictures[name][..., 3] > 0).sum())
        check(drawn > 1000, f"{name} draws", f"{drawn} pixels with alpha")
        save(keep, f"page-{name.replace(' ', '-').lower()}", pictures[name])
    names = list(pictures)
    distinct = all(np.abs(pictures[x] - pictures[y]).max() > 0 for i, x in enumerate(names) for y in names[i + 1:])
    check(distinct, "the four meters are four different pictures")
    status = page.run("document.querySelector('.stage__readout')?.textContent || ''")
    check(status.startswith("This frame’s spectrum"), "the meters' line under the picture", status[:90])
    warning = page.run("(() => { const w = document.querySelector('#photosensitivity'); return w && w.getBoundingClientRect().height > 0 ? w.textContent.replace(/\\s+/g, ' ').trim() : ''; })()")
    check("30 flashes a second" in warning and "Shutter at 0" in warning, "the photosensitivity warning is at the top", warning[:70])
    caution = page.run("(() => { const c = document.querySelector('#vfd-caution'); return c ? c.getBoundingClientRect().height : -1; })()")
    check(caution == 0, "no VFD caution line under the meters", f"height {caution}")
    shown = page.run("[...document.querySelectorAll('.transport [data-half]')].map((f) => `${f.dataset.half}:${f.getBoundingClientRect().width > 0}`).join(' ')")
    check(shown == "meters:true vfd:false", "the meters' test signal menu is on screen, the VFD's is not", shown)

    # The VFD, playing.
    page.after("document.querySelector('.btn--play').click()")
    page.set({"type": 4})
    time.sleep(2.0)
    s = page.run(STATS)
    vfd = page.canvas()
    save(keep, "page-vfd-track", vfd)
    check(s is not None and s["lit"] > 20, "Type = VFD draws lit segments", json.dumps(s))
    check(s and s["state"] == "Running" and s["rate"] == 48000 and s["thirds"] == 31,
          "earshot runs on the feed at 48 kHz, 31 thirds", s and f"{s['state']}, {s['rate']} Hz, {s['thirds']} thirds")
    check(s and s["lamp"] == "green", "the status lamp is green", s and s["lamp"])
    # vfd::Input::List runs once, on the first VFD frame, and NEEDLE_NO_DEVICES
    # stops it before earshot::listDevices (which, built with no backend, has
    # nothing to list anyway): the menu holds no device, and nothing is selected.
    check(s and s["devices"] == 0 and s["calls"]["lists"] <= 1 and s["calls"]["selects"] == 0,
          "no device listed or selected", s and json.dumps(s["calls"]) + f", {s['devices']} devices in the menu")
    shown = page.run("[...document.querySelectorAll('.transport [data-half]')].map((f) => `${f.dataset.half}:${f.getBoundingClientRect().width > 0}`).join(' ')")
    check(shown == "meters:false vfd:true", "the VFD signal menu replaces the meters' test signal on screen", shown)
    note = page.run("document.querySelector('.transport + .stage__status')?.textContent || ''")
    check("no microphone" in note, "the note under the picture says there is no microphone", note[:80])
    CAUTION = "(() => { const c = document.querySelector('#vfd-caution'); return c && c.getBoundingClientRect().height > 0 ? c.textContent : ''; })()"
    caution = page.run(CAUTION)
    check(caution.startswith("Photosensitivity: with Shutter above 0"), "the VFD's caution line is under the picture", caution[:70])

    # A VFD control changes the picture (paused, so only the control differs).
    page.pause()
    base_picture = page.canvas()
    page.set({"bands": 1})
    octaves = page.canvas()
    d = np.abs(octaves - base_picture)[..., :3]
    check(d.max() > 64, "Bands to 10 Octaves changes the picture", f"max {d.max()} codes, {int((d > 0).sum())} values")
    save(keep, "page-vfd-octaves", octaves)
    # Bands rebuilds the columns, so the eye's picture is taken again after it.
    page.set({"bands": 0})
    eye = page.canvas()
    page.set({"multiplex": MULTIPLEX_300, "shutter": SHUTTER_1000})
    caution = page.run(CAUTION)
    check(caution.startswith("Photosensitivity warning: Shutter is above 0"), "with Shutter above 0 the caution says so", caution[:70])
    banded = page.canvas()
    d = np.abs(banded - eye)[..., :3]
    check(d.max() > 64, "Shutter 1/1000 s adds the scan's bands", f"max {d.max()} codes, {int((d > 0).sum())} values")
    save(keep, "page-vfd-shutter-1000", banded)
    page.set({"multiplex": 0.30103, "shutter": 0.0})
    back = page.canvas()
    d = np.abs(back - eye)[..., :3]
    check(d.max() == 0, "and back to the eye is the eye's picture again", f"max {d.max()} codes")

    # Every signal; Silence's lamp; Nothing fed.
    page.after("document.querySelector('.btn--play').click()")
    options = page.run("[...document.querySelector('#vfd-signal').options].map((o) => o.value)")
    for signal in options:
        page.after(f"(() => {{ const s = document.querySelector('#vfd-signal'); s.value = {json.dumps(signal)}; s.dispatchEvent(new Event('change')); }})()")
        time.sleep(4.0 if signal in ("silence", "host") else 1.2)
        s = page.run(STATS)
        save(keep, f"page-vfd-{signal}", page.canvas())
        if signal == "silence":
            check(s["lamp"] == "red" and s["state"] == "Silent", "Silence: after 3 s earshot says silent and the lamp is red", f"{s['state']}, {s['lamp']}")
        elif signal == "host":
            check(s["lamp"] == "amber" and s["lit"] == 0 and s["fed"] == 0, "Nothing fed: dark, amber, no samples",
                  f"{s['lit']} lit, {s['lamp']}, {s['fed']} fed")
        else:
            check(s["lit"] > 0 and s["lamp"] == "green", f"signal {signal} lights segments, lamp green", f"{s['lit']} lit, {s['dots']} dots, {s['lamp']}")
    page.after("(() => { const s = document.querySelector('#vfd-signal'); s.value = 'track'; s.dispatchEvent(new Event('change')); })()")

    # Back to a meter.
    page.set({"type": 0})
    time.sleep(0.5)
    vu = page.canvas()
    check(int((vu[..., 3] > 0).sum()) > 10000, "back on the VU after the VFD, the meter draws")
    status = page.run("document.querySelector('.stage__readout')?.textContent || ''")
    check(status.startswith("This frame’s spectrum"), "and the line under the picture is the meters' again", status[:60])

    # A link that opens on the VFD with Shutter above 0 opens paused; at 0 it plays.
    page.load(f"{base}?type=4&shutter=0.5")
    playing = page.run("window.needleDemo.mounted.state.playing")
    label = page.run("document.querySelector('.btn--play').textContent")
    check(playing is False and label == "Play", "a link opening on the VFD with Shutter above 0 opens paused", f"playing {playing}, button {label}")
    page.load(f"{base}?type=4")
    check(page.run("window.needleDemo.mounted.state.playing") is True, "a link opening on the VFD at Shutter 0 plays")
    page.load(f"{base}?type=4&shutter=0.5&embed=1")
    check(page.run("window.needleDemo.mounted.state.playing") is True
          and page.run("document.querySelector('#photosensitivity').getBoundingClientRect().height") == 0,
          "in embed mode neither the warning nor the paused rule applies")

    calls = page.run("window.__deviceCalls")
    check(calls == [], "no getUserMedia, enumerateDevices or getDisplayMedia call", json.dumps(calls))
    return failures


#---------------------------------------------------------------------------
# Part 2: the pictures against ndtest.
#---------------------------------------------------------------------------

def ndtest(binary, host, frames, override=None):
    """The last of `frames` frames."""
    sets = []
    for name, value in host:
        value = (override or {}).get(name, value)
        sets += ["--set", f"{name}={value:.9g}"]
    w, h = map(int, SIZE.split("x"))
    size = w * h * 4
    proc = subprocess.Popen([binary, "--pipe", "--fps", "60", "--size", SIZE, "--level", "-18", "--frames", str(frames), *sets],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    last, got = None, 0
    while True:
        chunk = proc.stdout.read(size)
        if not chunk:
            break
        if len(chunk) < size:
            chunk += proc.stdout.read(size - len(chunk))
        if len(chunk) == size:
            last, got = chunk, got + 1
    err = proc.stderr.read().decode()
    if proc.wait() != 0 or got != frames:
        raise RuntimeError(f"ndtest {' '.join(sets)}: {got} of {frames} frames; {err[:400]}")
    return np.frombuffer(last, np.uint8).reshape(h, w, 4).astype(np.int16)


def crosscheck(page, base, binary, keep):
    failures = 0
    for name, count, settings in CASES:
        page.load(f"{base}?size={SIZE}&type=4&vfdsignal=programme")
        page.pause()
        page.set(settings)
        before = page.run(STATS)["instances"]
        page.restart()
        if page.run(STATS)["instances"] != before + 1:
            raise RuntimeError("Restart did not start a new instance")
        stepped = page.run(STEP % count)
        if abs(stepped["time"] - count / 60) > 1e-9:
            raise RuntimeError(f"the page's clock reads {stepped['time']} after {count} steps")
        ours = page.canvas()
        host = page.run(HOST)
        theirs = ndtest(binary, host, count)

        diff = np.abs(ours - theirs)
        rgb = diff[..., :3]
        off = int((rgb > 0).sum())
        ok = rgb.max() <= 8 and off <= 0.01 * rgb.size and diff[..., 3].max() <= 1
        failures += not ok
        s = stepped["stats"]
        lit = sum(c["lit"] for c in s["columns"])
        dots = sum(1 for c in s["columns"] if c["dot"] >= 0)
        print(f"{'ok  ' if ok else 'FAIL'}  {name:<52} max {rgb.max()} codes, {off} of {rgb.size} values off"
              f" (mean {rgb.mean():.2e}), alpha max {diff[..., 3].max()} | page: {lit} segments lit, {dots} dots,"
              f" fed {s['fed'] / 48000:.3f} s, earshot {s['state']}, lamp {s['lamp']}")
        stem = "".join(c if c.isalnum() else "-" for c in name)
        save(keep, f"{stem}-page", ours)
        save(keep, f"{stem}-ndtest", theirs)
        save(keep, f"{stem}-diff-x40", rgb, 40)
        if name.startswith("defaults"):
            defaults_mean = rgb.mean()
            step = dict(host)["Rotation"] + 0.001
            slip = np.abs(ours - ndtest(binary, host, count, {"Rotation": step}))[..., :3]
            caught = slip.mean() > defaults_mean
            failures += not caught
            print(f"{'ok  ' if caught else 'FAIL'}  {'negative: Rotation one step (0.001) off':<52} max {slip.max()} codes,"
                  f" mean {slip.mean():.2e} against the defaults' {defaults_mean:.2e}, {int((slip > 0).sum())} values off")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ndtest", help="a built ndtest: compare the VFD's pictures with it")
    ap.add_argument("--keep", help="write the pictures (and each case's difference, x40) here as PNG")
    ap.add_argument("--skip-page", action="store_true", help="only the comparison")
    args = ap.parse_args()

    server, base = serve()
    page = Page()
    failures = 0
    try:
        if not args.skip_page:
            print("-- the page")
            failures += page_checks(page, base, args.keep)
        if args.ndtest:
            print("\n-- the VFD against ndtest")
            failures += crosscheck(page, base, args.ndtest, args.keep)
        problems = page.problems()
        for p in problems:
            print(f"FAIL  the page logged: {p}")
        failures += len(problems)
        renderer = page.run("(() => { const gl = document.createElement('canvas').getContext('webgl2'); const x = gl.getExtension('WEBGL_debug_renderer_info'); return x ? gl.getParameter(x.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER); })()")
        print(f"\nthe page rendered on: {renderer}; served with demo/_headers' Content-Security-Policy")
    finally:
        page.close()
        server.shutdown()
    print("all checks pass" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    os.environ.setdefault("TMPDIR", tempfile.gettempdir())
    sys.exit(main())
