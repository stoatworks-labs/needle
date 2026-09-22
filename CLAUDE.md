# needle

Audio meters with the ballistics their standards actually specify, as an FFGL
**source** for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before touching anything in `source/meter/`, the scale
mappings or a tolerance in the harness.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/ndtest --out /tmp/f.png --size 1920x1080 --level -18`
- Set anything by name: `--set "Type=2" --set "Reference Level=0.55"`
- Feed it a signal: `--level -18` (dBFS), `--burst 150` (ms, then silence)
- List parameters: `./build/ndtest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check, ~9 s)
- **The physics, with no GL context at all:**
  - `./build/ndtest --ballistics` — ANSI C16.5: 99 % in 300 ms, 1.0–1.5 % overshoot
  - `./build/ndtest --ppm` — IEC 60268-10 II: 20 dB of fall-back in 2.8 s
  - `./build/ndtest --steps` — the LM3915 ladder, 3 dB a rung, by bisection
  - `./build/ndtest --eye` — the shadow is monotonic and shuts at the overload
  - `./build/ndtest --prime` — frame one advances nothing; a clip trigger is not deaf
  - `./build/ndtest --rate` — the same answer at 24, 30, 50, 60 and 144 fps
  - `./build/ndtest --friction` — a worn pivot stops the pointer short, and differently
    depending on which way it came
  - `./build/ndtest --defaults` — Free agrees with Standard at the shipped defaults
  - `./build/ndtest --names` — no name over 16 characters, none duplicated
- Pixels (the only check that needs a rasteriser): `./build/ndtest --pixels`
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
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `ND01`.

## Not done yet
- **Never loaded into Resolume, and not installed into it.** Everything here is
  the offline harness driving the real plugin class.
- Windows is CI-only and the CI has never run.
- No release tag, no website registration, no presets, no browser demo, no
  OpenFX port, no user guide. `StoatworksAbout.h` and `ATTRIBUTIONS.md` are
  provisional hand copies.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records the solved ballistic constants, the GL driver, what unit
the host's clock turned out to be, and whether any audio reached the layer.

    ~/Library/Logs/needle/needle.YYYY-MM-DD.log
