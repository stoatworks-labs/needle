# demo/ — the browser demo

Live at **<https://needle-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file, `tools/` and `wasm/` out of the upload.

    index.html        the shell
    plugin.js         this plugin's parameters, its ported meter engine, its meter shaders
    vfd.js            Type = VFD: loads needle-core, hosts it, its GLSL copy, its test signals
    needle-core.js    the plugin's C++ and earshot's, compiled by emscripten -- BUILT, committed
    needle-core.wasm
    wasm/             the build's C++ inputs (glue.cpp, gl_shim.cpp) and inputs.sha256
    needle.css        the page's own styles: the photosensitivity warning and the VFD's caution line
    vendor/           the shared kit, copied in from stoatworks-backend — DO NOT EDIT
    tools/            check_shaders.py (run by tools/verify.sh), build-wasm.sh, crosscheck.py
    _headers          CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

Two things. **The four meters are a port**, not a recording and not the plugin.
**Type = VFD is the plugin**, compiled to WebAssembly (below).

The shader is the plugin's, copied across unedited. `VERTEX_SHADER`,
`FRAGMENT_SHADER_A` and `FRAGMENT_SHADER_B` at the top of `plugin.js` are the
three raw strings in `source/Shaders.cpp` — the fragment shader is two adjacent
raw strings there because MSVC caps a literal at about 16 KB — and the page joins
A + B exactly as the C++ compiler does. `tools/check_shaders.py` compares every
raw string and the joined fragment text character for character, and
`../tools/verify.sh` runs it.

The CPU half is a port: `meter/Standards`, `meter/Movement`, `meter/Engine`,
`Audio::LevelFromSpectrum`, `Controls.h`, `NeedlePlugin::BuildFrame`,
`Renderer::Draw`'s uniform packing and `Font.cpp`. **Nothing checks the port
but a reader.** Change any of those in C++ and the page silently disagrees.

**There is no audio.** The plugin's only input is Resolume's 64-bin FFT buffer.
The page generates a test signal instead — chosen in the "Test signal" dropdown —
integrates its power over each frame's interval and writes it into 64 bins as
magnitudes (a tone into one bin; pink noise as 1/f with a per-bin exponential
scatter). The plugin's own level law runs on those bins. **It is not the host's
FFT**: Resolume's window, normalisation and bin law are unknown and are not
reproduced; the page says so in its banner, its disclosure, under the transport
and in the hints on Reference Level, Sensitivity and Bin Law.

Everything else is not the plugin either: no Resolume, no composition, no FFGL,
GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core. A pixel here is not a
measurement of a pixel there, and a needle here is not a measurement of the
ballistics — `ndtest` is.

## The VFD: the plugin, compiled

`needle-core.wasm` is `source/Needle.cpp` and everything it calls (every
`source/*.cpp` but `SourcePlugin.cpp`, the registration, whose job the page
does), earshot's snapshot with `EARSHOT_CAPTURE=OFF` (its `NoCapture.cpp`;
miniaudio is not compiled) and the FFGL SDK's plugin classes, **compiled
unmodified** by `tools/build-wasm.sh` (woofer's script). `wasm/glue.cpp` plays
the host: it constructs the plugin, reads its declarations back through the
SDK's getters, forwards SetFloatParameter, SetTime and ProcessOpenGL into the
page's framebuffer, and — as `ndtest` does — calls `ForceSecondsClock()` and
hands the VFD its audio through earshot's `feed()` on the plugin's own
`vfd::Input` (`Activate`, `FeedBegin(48000, 2)`, `Feed`). `wasm/gl_shim.cpp`
replaces four GL entry points: `glShaderSource`, which compiles only text equal
to one of the page's checked copies (`VERTEX_SHADER`, `FRAGMENT_SHADER` and
`VFD_FRAGMENT_SHADER`, spliced from `source/VfdShaders.cpp`) after the kit's
`port()`, and `glEnable`/`glDisable`/`glIsEnabled`, which ignore
`GL_PROGRAM_POINT_SIZE`.

Why the meters were not moved to the WebAssembly too: the port is checked by
`check_shaders.py` and has been on the site since September, and the VFD is the
new thing. The compiled plugin still advances its own meter engine on VFD
frames, as `ProcessOpenGL` does, but the meters on the page are drawn by the
port. Moving them over would be the obvious next step and would retire the
port's "only a reader checks it" caveat.

**No device, no microphone.** earshot here has no capture backend at all, and
`nd_prepare` sets `NEEDLE_NO_DEVICES=1` (the plugin's verification switch), so
`vfd::Input::List()` — which the plugin runs once, on its first VFD frame —
never reaches `earshot::listDevices()`. **Audio Input** and **Rescan Inputs**
are left off the panel. The line under the picture prints the menu's device
count and the plugin's own `selectDevice` counter: 0 and 0. No served file
names `getUserMedia`, `enumerateDevices` or `mediaDevices`; `check_shaders.py`
fails if one does.

**The audio** is generated inside the WebAssembly, a function of the sample
index alone, fed up to the page's clock at 48 kHz on both channels (after a gap,
only the newest half second: earshot's own catch-up). The "VFD signal" menu
(`?vfdsignal=`):

| id | signal | whose |
|---|---|---|
| `track` (default) | 120 BPM, 8 s loop: kick, snare, hats, bass, held chord, arpeggio | the page's |
| `programme` | a sine at every third's centre, −18 dBFS total, each drifting ±9 dB | **ndtest's** `Programme(PerBandDb(-18), 9, 48000)`, copied verbatim |
| `tones` | the 31 base-ten midbands in turn, 0.5 s each, −18 dB | the page's |
| `sweep` | log sine sweep 20 Hz–20 kHz in 20 s, then 2 s silence, −18 dB | the page's |
| `pink` | periodic pink noise (2^18 samples, 1/f lines 17.8 Hz–22.4 kHz), −18 dB | the page's |
| `silence` | exact zeros: Silent after 3 s, lamp steady red | — |
| `host` | nothing fed: the plugin's default "None (host FFT)", dark, lamp amber | the plugin's default |

Levels are dB re a full-scale sine (earshot's AES17 reference). `kPi`,
`AmpFromDb`, `kFeedRate`, `Signal`, `BandHz`, `Fade`, `Programme` and
`PerBandDb` in `glue.cpp` are held to `tools/ndtest/` character for character
by `check_shaders.py`.

**The panel** is hand-written in `plugin.js` like the meters', and checked at
start-up against the compiled constructor (`vfd.checkPanel`): every declared
control but the Audio buffer, the About block, Audio Input and Rescan Inputs,
in order, with the same names, groups, types, elements and defaults; each
difference is a console error. The numbers beside Multiplex Rate and Shutter
are `Controls.h`'s conversions, compiled in (`nd_convert`).

**Shutter and Multiplex Rate are the plugin's.** The rolling-shutter bands are
computed in its shader from the scan rate, the exposure and the fixed 16 ms
readout; the page adds nothing. At Shutter 0 (the eye) the scan is invisible by
design.

**Photosensitivity.** With Shutter above 0 the scan beats against the frame
rate and a dark band rolls through the display: the release's flash check of the
plugin's own output (ndtest, 1280×720, 60 fps, on music) measured 10–30
flashes a second in a third of the frame from 1/60 s to 1/500 s, depending on
Multiplex Rate, and 1 or under at 1/1000 s and 1/8000 s; Shutter 0, the default, stayed
at 2 a second or under. No default is changed. The page warns at the top
(`index.html`, `needle.css`), repeats it in a caution line under the picture
whenever the VFD is shown and in Shutter's hint, and opens a link on the VFD
with Shutter above 0 paused. In embed mode (`?embed=1`) neither the warning nor
that rule applies: there is no reader and no Play button.

**What can differ from the Mac:** WebAssembly has no fused multiply-add and
uses musl's libm, so earshot's filters can differ in the last digits; GL is
emscripten's WebGL2 on ANGLE, not a GL 4.1 driver.

### The cross-check (`tools/crosscheck.py`)

Part 1 drives the page in headless Chrome on the real GPU, served with
`_headers`' CSP applied (as Cloudflare does): the four meters draw and move,
the VFD draws, Bands and Shutter change the picture, every VFD signal runs
(Silence turns the lamp red, Nothing fed leaves it amber and dark), nothing is
logged, and no device API is called (trapped from outside). Part 2
(`--ndtest build/ndtest`) renders the VFD on the page and in
`ndtest --pipe` at 960×540 after the same 800 N samples of the programme, with
the page's own host values, and compares the bytes. Results, 2026-11-18, Apple
M4 Max, ANGLE Metal, against ndtest arm64 Release:

| case | N | max diff (codes) | values off |
|---|---|---|---|
| defaults, 31 Thirds | 240 | 1 | 68 of 1 555 200 |
| 10 Octaves | 180 | 1 | 75 |
| Shutter 1/1000 s at 300 Hz multiplex | 180 | 1 | 2 |
| Reference −30 dBFS, Sensitivity +6 dB, Peak Hold 0 | 150 | 1 | 102 |
| Rotation 30°, Size 0.8, Background 1, lamp off | 120 | 0 | 0 |
| *negative control:* ndtest with Rotation +0.001 | 240 | 255 | 39 424 |

The same segments lit and the same dots in every case, alpha identical, and
at most one code of 255 in at most 102 values (edge coverage, rasterised by two
GL stacks); a 0.36° slip moves 39 424 values. ndtest's frame
N − 1 is compared (its clock 1/60 s behind the page's); only the scan's phase
reads the clock, so the Shutter case uses 300 Hz, five whole scans per 1/60 s.

## What is deliberately absent

- **The `Audio` buffer parameter.** A host writes it, never an operator; the
  test signal stands in for it.
- **Audio Input and Rescan Inputs** (the VFD's device menu and its button). The
  page never lists or opens an audio device.
- **Hiding.** Resolume shows only the controls the current Type reads; the
  kit's panel cannot hide one, so all 35 are always shown and the hints say
  which Types read them.
- **The clip picker and the "use my own file" button.** Needle is a source with
  zero inputs. The kit builds both for every demo and `plugin.js` removes them.
- **The About block.** A text line and four link buttons exist so a *host* has
  somewhere to put them. A web page has links of its own.
- **The plugin's `Clock`** in the meters' port. The kit's clock is already in
  seconds. The compiled VFD has it, told seconds by `ForceSecondsClock()`.

## Working on it

```bash
python3 -m http.server 8947          # from this directory (no CSP: crosscheck.py applies it)
python3 tools/check_shaders.py       # the copies, the harness's signals, no device API, the .wasm's inputs
tools/build-wasm.sh                  # rebuild needle-core.{js,wasm} (needs em++)
python3 tools/crosscheck.py --ndtest ../build/ndtest   # the page in Chrome, and the VFD against ndtest
../tools/verify.sh                   # everything, including check_shaders
```

The deploy has no build step: what is committed is what is served, the built
`needle-core.js` and `.wasm` included. **After changing anything under
`source/` or `external/earshot/` — a version bump and a regenerated
`StoatworksAbout.h` included — rerun `tools/build-wasm.sh`** and commit both
outputs and `wasm/inputs.sha256`; `check_shaders.py` fails until you do. The
script writes both outputs 644 (emscripten writes the .wasm 755).

**After changing `source/Shaders.cpp` or `source/VfdShaders.cpp`, copy it
across here too** (`plugin.js`, `vfd.js`) —
`check_shaders.py` will tell you which string and where the first difference
is. Do not edit the GLSL in `plugin.js` to make something compile in WebGL2:
`port()` in `vendor/gl.js` handles the version line and the precision qualifiers
and nothing else, and if a shader will not compile here the answer is to say so
on the page.

**`vendor/` is a copy.** Fix the kit in
`stoatworks-backend/resolume-demo/kit/` and re-run
`stoatworks-backend/resolume-demo/sync.sh needle`.

## Deploying

A push to `main` deploys it (`.github/workflows/deploy.yml`). By hand, from the
**repository root**, not from here:

```bash
cf-run npx wrangler deploy
curl -s 'https://needle-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.

## Embed mode

`?embed=1` renders the output and nothing else, so the page can be a video
source. `?size=1920x1080`, `?bg=black|checker|white` and any parameter id work
as query parameters (the Size control's id is `instrumentSize`, because `size`
is the raster). The meters' test signal is the kit's variant and is not in
the URL, so an embed runs the default 0 VU step; the VFD's is, as
`?vfdsignal=`.

    https://needle-demo.stoatworks-labs.com/?embed=1&size=1920x1080&type=1
    https://needle-demo.stoatworks-labs.com/?embed=1&size=1920x1080&type=4&instrumentSize=1&vfdsignal=pink
