# needle

Audio meters with the ballistics their standards actually specify, as an FFGL
**source** for Resolume Arena/Avenue, plus a fifth Type, a VFD spectrum
analyser on earshot's third-octave bank (`specs/SPEC-needle-vfd.md` in the
specs folder beside the repos; shipped in v0.2.0). C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before touching anything in `source/meter/`, the scale
mappings or a tolerance in the harness.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/ndtest --out /tmp/f.png --size 1920x1080 --level -18`
- Feed the VFD a recording: `ffmpeg -i in.wav -f f32le -ac 1 -ar 48000 a.f32`, then
  `NEEDLE_NO_DEVICES=1 ./build/ndtest --pipe --set Type=4 --pcm a.f32 | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mkv`
- Set anything by name: `--set "Type=2" --set "Reference Level=0.55"`
- Feed it a signal: `--level -18` (dBFS), `--burst 150` (ms, then silence)
- List parameters: `./build/ndtest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check; the VFD's
  `--negative` sweep alone is about 90 s)
- **The physics, with no GL context at all:**
  - `./build/ndtest --ballistics` — ANSI C16.5: 99 % in 300 ms, 1.0–1.5 % overshoot
  - `./build/ndtest --ppm` — IEC 60268-10 II: 20 dB of fall-back in 2.8 s
  - `./build/ndtest --steps` — the LM3915 ladder, 3 dB a rung, by bisection
  - `./build/ndtest --eye` — the shadow is monotonic and shuts at the overload
  - `./build/ndtest --prime` — frame one advances nothing; a clip trigger is not deaf
  - `./build/ndtest --rate` — the same answer at 24, 30, 50, 60 and 144 fps
  - `./build/ndtest --friction` — a worn pivot stops the pointer short, and differently
    depending on which way it came
  - `./build/ndtest --pair` — both meters of a Stereo Pair agree, bit for bit, after
    Count goes to Mono and back; its negative control is v0.1.0's frozen right meter
  - `./build/ndtest --defaults` — Free agrees with Standard at the shipped defaults; the
    level law is the FFGL SDK's `ffglqs::Audio` volume, and an SDK volume of −18 dB reads 0 VU
  - `./build/ndtest --free` — Free as trajectories: Overshoot exact for the faster direction and
    a bound for the slower, Rise and Fall exact, no coasting past the programme. Fails on v0.1.1
  - `./build/ndtest --visible` — the inspector hides what Type and Standard cannot reach
  - `./build/ndtest --names` — no name over 16 characters, none duplicated
- Pixels (the only check that needs a rasteriser): `./build/ndtest --pixels`
- **The VFD** (Type = VFD; `tools/ndtest/Vfd.inc`). Audio goes in through earshot's
  `feed()` on a scripted capture backend, so no check lists or opens a real device:
  - no GL: `--iec` (IEC 61260 class 1 centres, labels, edges, own-band and edge
    readings), `--hold` (hold and fall agree at 24, 30, 60, 144 fps; the dot waits)
  - pictures, at 1280x720 and 320x180: `--skirt`, `--octaves`, `--multiplex`, `--clock`,
    `--filament`, `--phosphor`, `--geometry`, `--resize`, `--devices`. Prefix
    `NDTEST_RENDERER=software` for Apple's software renderer; verify.sh runs both
  - `--coreaudio` — the REAL backend: with the VFD off, no Engine, no device list, no select
  - `--negative` — every planted bug (`NDTEST_NEGATIVE=<name>`) makes its check fail
  - `tools/mutate.sh` — one-character GLSL mutants of `source/VfdShaders.cpp`, each
    against the check that must catch it (not in verify.sh: a few minutes)
- Cost: `./build/ndtest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)

## Notes
- **The ballistics are not a free choice.** `source/meter/Standards.h` quotes the
  published figures and *solves* ζ, ωn and the two time constants from them. If
  a movement looks wrong, the fix is in the quoted figure or in the algebra, not
  in a slider.
- **Never check a ballistic claim by reading pixels.** They are properties of a
  differential equation; a rasteriser has no opinion about them. Every physics
  check drives `source/meter/` directly at `kEngineRate` and opens no context.
- **The textbook `ts ≈ 4/(ζωn)` is the wrong question.** It is when the
  *envelope* enters a 2 % band. Here it says 364 ms where the true first crossing
  of 99 % is 300 ms.
- **The level is the RMS over the 64 bins, `sqrt( Σ b² / 64 )`** — the FFGL SDK's own volume.
  v0.1.1 took the root of the SUM, 18.06 dB hot, and pinned on a real rig (needle#1). The harness
  writes `--level` as every bin at that level, so `--level -18` is still 0 on every scale.
- **Free picks its spring in `DriveMovement`, never by `u >= x`.** At rest or heading for the
  target: the direction's own; carried past it: the stiffer, split at the turn. `u >= x` handed an
  overshooting pointer the fall's weak spring and it coasted (11.6 % at the defaults).
- **The VU has stops** at −0.02 and 1.035 (`kDialStopLow/High`), the drawing's old clamp.
- **Hidden is not dead.** `ApplyVisibility` hides Rise/Fall (Standard on), Overshoot (Standard on,
  or PPM/Bargraph), Peak Hold/Hold Decay (not Bargraph or VFD), Persistence (not Eye), the six VFD
  controls (not VFD) and, on the VFD, Count, Bin Law, the ballistics and the Look group. Never hide
  Type; Standard only on the VFD. Ids may move, names may not: compositions store names (vectrix's
  docs/NOTES.md).
- **Only `source/Vfd.cpp` includes earshot** (verify.sh greps it), and VFD off never reaches it:
  the Engine, the device list and any device open happen on the first VFD frame, never in the
  constructor (`--coreaudio`). `NEEDLE_NO_DEVICES=1` refuses listing and opening outright; verify.sh
  sets it for oxbow and the sweep. earshot is an unpatched snapshot (`external/earshot/SNAPSHOT`).
- **The VFD's peak hold is its own, not earshot's `Third::holdDb`.** earshot's wait starts at the
  level's last maximum; the display's at the frame the bar leaves the dot's segment (AGENTS.md).
- **`--pipe` exits 1 when the reader hangs up**, never SIGPIPE's 141; verify.sh checks it under bash
  with `NDTEST_NEGATIVE=pipe` (141) as its control.
- **A VU scale is linear in VOLTAGE; a PPM scale is linear in dB.** That is why
  0 VU sits at 71 % of the arc and a BBC PPM's marks are evenly spaced.
  `ScaleFor()` is the one place that knows.
- **The engine takes the host's clock, not a delta, and frame one takes no
  steps.** A clip triggered at t = 40 s would otherwise integrate 192,000 steps
  before anybody saw a frame. A zero interval must advance *nothing*.
- **Wear is one control doing two jobs.** The shader's half is dirt and dim
  phosphor; the engine's half is dry friction in the pivot, which stops the
  pointer *short* rather than slowing it down, and leaves it somewhere different
  depending on which way it came. At Wear 0 the movement is bit-identical to the
  frictionless one, and `--friction` asserts that — the ANSI claim is made
  there.
- **Count decides how many meters are DRAWN, never how many RUN.** The engine
  integrates both channels on every frame. v0.1.0 stepped only the visible ones,
  so after Mono and back the right meter resumed from where it had stopped and
  disagreed with the left for about two seconds. Fixed in v0.1.1; `--pair`.
- **The CPU computes every pixel coordinate**; the shader only decides coverage.
  That is what lets `--pixels` probe coordinates the plugin itself produced.
- **`quiet()` in the harness is graticule's burn-in plate.** Lamp, Glass, Wear,
  the scale numbers and the hold bar all draw over something a probe wants.
- **Parameter names must be unique and ≤ 16 characters** — `--set` and the sweep
  find them by name, and FFGL truncates silently.
- `needle_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no
  host can instantiate the plugin at all.
- `source/StoatworksAbout.h` is GENERATED by stoatworks-backend's
  `sync-about.py`, and `ATTRIBUTIONS.md` by `sync-attributions.py`. Never edit
  either by hand. The About block is five entries (About text, User guide,
  Project page, Source on GitHub, Support the work), so `--list` shows 43
  parameters: 35 swept controls, the Audio buffer, the About block, and the
  VFD's Audio Input and Rescan Inputs (never swept: they reach real devices).
  The VFD's six (ids 37–42) are appended after About, never renumbered.
- The user guide is `docs/USER-GUIDE.md` — the only copy anyone edits. The
  website's `build_guides.py` renders it to `docs/USER-GUIDE.pdf` and to
  https://stoatworks-labs.com/software/needle/guide/.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `ND01`.

## Released
- Public at github.com/stoatworks-labs/needle (created 2026-09-23); v0.1.0 and
  v0.1.1 released on 2026-09-23, v0.1.2 on 2026-10-11, v0.2.0 (the VFD) on
  2026-11-19. Registered in the website's `projects.json` (beta,
  guide) and in stoatworks-backend's sync-about TARGETS, `names.json`,
  `visibility.json` and `derived.json`.
- CI on GitHub: `ci.yml`'s macOS job runs on a runner with no GPU, so the
  harness falls back to Apple's software renderer. The physics checks passed
  on the runner; the pixel and sweep steps passed there too (`ndtest --pixels`, and the sweep at 160x90). The Windows x64 DLL compiles
  with MSVC on GitHub's Windows runner (passed), earshot and miniaudio included
  since e512e2b. From v0.2.0 the macOS job also runs `--iec --hold --coreaudio`.

## Not done yet
- **Resolume on macOS: one REST session** (2026-11-19, v0.2.0, Arena 7.29.0).
  It loads, Arena honours the hiding (31 params listed on a VU, 26 on the VFD),
  and the meters move to `Beat 001.wav`, but low at the defaults (the VU near
  -20, the PPM on its stop). The inspector itself hasn't been looked at.
- Windows in Resolume Arena: the v0.2.0 release zip's DLL passed the fleet's Arena gate 9 of 9 on 2026-11-19 (Arena 7.27.1, win-lab, llvmpipe): loads, registers as `SW Needle` / `ND01` / source, the 31 params listed at the defaults as declared, renders, 21 controls move the picture; the 5 audio-driven controls were skipped because the box has no sound device. The gate never selects Type = VFD (it sweeps a choice's first three options). Type = VFD was then run by hand in the same Arena (REST, 2026-11-19, the release zip's DLL): it renders (lamp where the Mac puts it, scaled), stays dark with its amber lamp with no device (0 listed, none opened, Audio Input on None (host FFT)), Arena lists the same 26 controls, and every VFD control at both ends or through every option, plus one Rescan Inputs press, left Arena up as one process with a clean log; back on the VU the frame matched the pre-switch one exactly.
- Real audio in a host: the needle#1 rig (v0.1.1) and Arena's demo loop on this Mac
  (above). Resolume's 64-bin FFT mapping is
  assumed (`Bin Law`), not measured. The RMS footing (needle#1) is a better guess, not a number.
- The universal build has never run on an Intel Mac. Cost figures are
  Apple Silicon only.
- No presets, no OpenFX port.
- **The VFD has never been in a host with a real device.** In Arena (no device chosen) it was
  dark with the amber lamp, as designed. Everything else is the harness on a scripted backend,
  plus oxbow with `NEEDLE_NO_DEVICES=1`. The mic prompt hasn't been seen, Rescan's menu refresh
  in Resolume is unknown. On Windows it has run only with no sound device (win-lab, 2026-11-19).
- **Shutter > 0 flashes** (up to 30 a second in part of the frame, at 60 fps): the README, the
  guide and the demo carry a warning; the default (0) is 2 a second or under. AGENTS.md,
  "Flashing".

## Browser demo
- Live at https://needle-demo.stoatworks-labs.com (Cloudflare Worker `needle-demo`,
  `wrangler.toml`, assets from `demo/`). A push to main deploys it (`deploy.yml`); by hand,
  from the repo root: `cf-run npx wrangler deploy`, then verify by content:
  `curl -s 'https://needle-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.
- `demo/plugin.js` carries `source/Shaders.cpp`'s GLSL verbatim as three constants
  (vertex, and the fragment shader's two raw strings joined A + B).
  `demo/tools/check_shaders.py` compares them character for character and
  `tools/verify.sh` runs it. **Change a shader, copy it across.**
- Everything else in `plugin.js` is a hand port (Standards, Movement, Engine,
  Audio's level law, Controls.h, BuildFrame, Render's uniform packing, Font).
  Change the C++ and the page silently disagrees; only a reader checks it.
- No audio: a generated test signal (the "Test signal" dropdown) is written into
  64 bins by the page. Not Resolume's FFT.
- **Type = VFD is the plugin compiled to WebAssembly** (`demo/needle-core.{js,wasm}`,
  built by `demo/tools/build-wasm.sh`, committed). **Changed anything under `source/` or
  `external/earshot/` — version bump and regenerated `StoatworksAbout.h` included? Rerun
  `demo/tools/build-wasm.sh`**; `check_shaders.py` checks `demo/wasm/inputs.sha256` and fails
  until you do. Its audio is generated test signals through earshot's `feed()`; no device
  is ever listed or opened (`NEEDLE_NO_DEVICES=1`, no capture backend), and Audio Input and
  Rescan Inputs are not on the panel. `demo/tools/crosscheck.py --ndtest build/ndtest`
  checks the page in Chrome and the VFD's bytes against ndtest.
- **Photosensitivity:** Shutter above 0 makes the VFD flash (up to 30/s in a third of the
  frame). The page warns at the top, under the picture and in Shutter's hint, and a link
  opening on the VFD with Shutter above 0 opens paused. Warn, never change a default.
- `demo/vendor/` is the shared kit — never edit it; fix it in
  `stoatworks-backend/resolume-demo/kit/` and re-run its `sync.sh needle`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records the solved ballistic constants, the GL driver, what unit
the host's clock turned out to be, and whether any audio reached the layer.

    ~/Library/Logs/needle/needle.YYYY-MM-DD.log
