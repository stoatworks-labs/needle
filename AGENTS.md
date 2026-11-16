# Working on needle

An FFGL **source** plugin (`ND01`) that draws four audio meters — a VU, a peak
programme meter, an LED bargraph and a magic eye — each moving the way its
standard says it must. A fifth Type, a VFD spectrum analyser on earshot's
third-octave bank, is built and unreleased: see "The VFD spectrum analyser".

`CLAUDE.md` is the command reference; this file is the *why*.

## Status

Built 2026-09-22. Every check in `tools/verify.sh` passes on macOS (Apple
Silicon, macOS 26.4, Metal-backed GL 4.1): the ballistics against ANSI C16.5,
the fall-back against IEC 60268-10 type II, the LM3915 ladder recovered by
bisection, the magic eye's shadow, frame-one priming, frame-rate independence,
the dead band a worn pivot leaves, the defaults, the names, the font, the pixel checks at two rasters, the
dead-control sweep, registration, lipo, plist, ad-hoc codesign and oxbow.

Released at v0.1.0 on 2026-09-23, v0.1.1 the same day (the Stereo Pair fix,
below) and v0.1.2 on 2026-10-11 (needle#1, below), public at github.com/stoatworks-labs/needle
(created the same day), and registered with the website (`projects.json`, beta,
guide) and with stoatworks-backend's sync-about TARGETS, `names.json`,
`visibility.json` and `derived.json`. The user guide is `docs/USER-GUIDE.md`,
rendered to `docs/USER-GUIDE.pdf` and to
https://stoatworks-labs.com/software/needle/guide/ by the website's
`build_guides.py`; the `.md` is the only copy anyone edits.

CI has run on GitHub, 2026-09-23. `ci.yml`'s macOS job runs on a runner with no
GPU, so the harness falls back to Apple's software renderer — a second
rasteriser. The physics step (`--ballistics --ppm --steps --eye --prime --rate
--friction --defaults --names --font`) passed on the runner; the pixel and
sweep steps passed there too (`ndtest --pixels`, and the sweep at 160x90). The Windows job compiled the x64 DLL with MSVC and passed.

**It has never been loaded into Resolume on macOS, and it has not been
installed into Arena there.** On Windows: a CI build passed the fleet's Arena gate 9 of 9 on 2026-09-23 (Arena 7.27.1, win-lab, llvmpipe): loads, registers as `SW Needle` / `ND01` / source, all 37 host parameters as declared, renders, 21 controls move the picture; the 11 audio-driven controls were skipped because the box has no sound device. No real audio has reached
it in a host except on the needle#1 rig (v0.1.1, below), so Resolume's 64-bin FFT mapping is still assumed (`Bin Law`).
The universal build has never run on an Intel Mac, and every render-cost figure
is Apple Silicon only. There are no factory presets and no OpenFX port.
The browser demo went live on 2026-09-24 at
https://needle-demo.stoatworks-labs.com — see "The browser demo", below.

**v0.1.2, released 2026-10-11 (needle#1).** The first report from a
real Resolume rig (VJPandaAU) said the needle pinned at the default Reference
Level and the Ballistics controls did nothing. Both were real, and three changes
answer them:

- the level law is now the **RMS over the bins** (the FFGL SDK's own volume),
  not the root of the sum, which read 18.06 dB hot — see "Which audio input is
  read";
- **Free switched springs on the wrong variable**, so Overshoot stopped meaning
  the overshoot whenever Fall differed from Rise (11.6 % at the shipped defaults,
  83 % at Fall 20 s); the VU now has **stops**; and
- the inspector **hides** what the current Type and Standard cannot reach, with
  Standard moved to head the Ballistics group (id unchanged).

`--defaults`, `--free`, `--visible` and two new `--ballistics` checks hold
them, and every one of them fails on v0.1.1's rule (checked by rebuilding with
each old rule put back). All of `tools/verify.sh` passes. **None of it has been
in Resolume**: the calibration is a better-grounded guess, not a measurement, and
the hiding is vectrix's Arena result, not needle's.

**The VFD (Type = VFD), 2026-11-16, unreleased.** Built on branch `claude/vfd`
with every check below passing on this Mac, GPU and Apple's software renderer;
the four meters render byte-identically to 840193c. Never in Resolume, never in
any host with a real audio device, earshot never compiled by MSVC, and
`ci.yml` runs none of its checks. See "The VFD spectrum analyser".

## The one idea

**The movement is not a free choice.**

A VU meter is a d'Arsonval movement: a mass on a spring with viscous damping,
which is a second-order low-pass with exactly two free parameters. ANSI C16.5
states two things about it — 99 % of a step in 300 ms, and an overshoot between
1.0 % and 1.5 % — and two statements about a two-parameter system leave nothing
to taste. So `source/meter/Standards.cpp` *solves* for them:

- ζ from the overshoot in closed form, `ζ = -ln(Mp)/√(π² + ln²Mp)`, at the
  midpoint of the permitted band: **ζ = 0.812716986**;
- ωn by bisection on the exact step response for the first crossing of 99 % at
  300 ms: **ωn = 13.5119 rad/s, 2.1505 Hz**.

The same for the rest. A PPM's 20 dB of fall-back in 2.8 s *is* a time constant,
because a one-pole decay in amplitude is a straight line in decibels:
`T = 2.8/ln 10 = 1.2160 s`. Its 5 ms integration figure gives
`T = 3.1616 ms`. The LM3915's ladder is 3 dB a rung, ten rungs.

Nothing in this plugin was tuned until it looked right, and the harness exists
to make that checkable rather than claimable.

### Do not use the textbook settling time

`ts ≈ 4/(ζωn)` is when the exponential *envelope* enters a 2 % band. It is not
when the response reaches any particular value, it ignores that the response
leaves and re-enters the band, and here it is wrong by a fifth: it says 364 ms
where the true first crossing of 99 % is 300 ms by construction. `--ballistics`
prints both, side by side, so nobody reaches for the approximation again.

## Where the truth lives, and what is only recollection

Two different kinds of number are quoted in `source/meter/Standards.h`, and the
difference matters.

**Given in the spec this repo was built from**, and therefore taken as the brief:

- the VU's 300 ms to 99 % and its 1.0–1.5 % overshoot;
- the PPM's 20 dB in 2.8 s, and that a 5 ms burst reads about 80 %;
- the bargraph's 3 dB per step over ten steps;
- that a magic eye's shadow angle follows grid voltage.

**Added here from background knowledge, and NOT checked against a document in
this session.** Each is plausible and each is load-bearing, so each is flagged:

- **"2 dB below steady for a 5 ms burst"** is used in place of the spec's
  informal "about 80 %". They are the same requirement —
  `10^(-2/20) = 79.43 %` — and the decibel form is the one IEC 60268-10 uses,
  but the standard has not been read here. If it turns out to be 4 dB (which is
  type I's figure), `kPpmBurstDownDb` is the single constant to change.
- **The BBC PPM's scale**: seven marks, 4 dB apart, mark 4 the alignment level.
  This decides where the pointer sits, not how it moves, so it cannot affect any
  ballistic claim.
- **The 6U5's 100-degree shadow angle at zero signal.** One number, in
  `kEyeOpenDegrees`, from the tube's published figure as it is usually carried.
  No data sheet was opened.
- **That a VU scale is linear in voltage.** This one is self-evident from any VU
  face rather than remembered: the scale runs to +3 VU, and the 0 mark sits
  about 71 % along, which is `1/10^(3/20) = 0.708`. It is asserted in
  `--ballistics` for that reason.

Everything about the magic eye other than the 100 degrees — the map from level
to angle, the 4-second heater, the phosphor persistence, the wear — is this
repo's own model. Nothing standardises a magic eye, and the code says so where
it makes each choice.

## Which audio input is read, and what is assumed about it

**The host's `FF_USAGE_FFT` buffer parameter, 64 bins, once per rendered
frame.** That is the entire audio surface FFGL 2.1 offers. There is no RMS
parameter, no peak parameter and no sample stream.

`source/Audio.h` carries the long version. The short version:

- **The level is the RMS over ALL 64 bins**, `sqrt( Σ b² / 64 )`. The spec warns
  that the fleet assumes those bins are linear 0→Nyquist and nobody has measured
  it. That assumption is not made here and does not need to be: by Parseval, a
  sum over every bin is the signal's power however the bins are laid out in
  frequency. Reorder them, warp them, space them logarithmically — the answer is
  the same. Only something that weighted bins against one another would care,
  and there is nothing like that in this plugin. Dividing by 64 changes none of
  that; it only decides where 1.0 is.
- **Why the mean and not the sum (needle#1).** v0.1.0–v0.1.1 took the root of the
  SUM, on the assumption that a full-scale sine sums to 1.0 across the
  spectrum. That is 20·log10(√64) = **18.06 dB** above the RMS, and the first
  real rig pinned the needle at the default −18 dBFS until Reference Level was at
  its 0 dBFS stop — the default plus the same 18 dB. The FFGL SDK's own
  `ffglqs::Audio::Update` (external/ffgl/source/lib/ffglquickstart/FFGLAudio.cpp)
  takes `sqrt( Σ b² / n )` and calls it the volume "between 0.0 and 1.0", so the
  plugin now stands on that footing and the −18 dBFS default puts an SDK volume
  of −18 dB on 0 VU. `--defaults` holds the law to `ffglqs::Audio` itself.
  **The ranges were not widened**, because they did not need to be and changing
  one remaps every saved composition: Reference −40..0 and Sensitivity ±12 still
  put 0 VU anywhere from an SDK volume of −52 dB to +12 dB, which covers the
  SDK's footing (programme around −18) and a Parseval-normalised one (a
  −18 dBFS tone in one bin reads −36 now) with room either side.
- **Whether a bin is a magnitude or a power is genuinely unknown**, and the
  fleet disagrees with itself about it (regauss takes the square root of each
  bin; spasis does not). So it is a control, `Bin Law`, defaulting to Magnitude.
  Turning an unmeasured assumption into a switch the operator can flip is a
  better answer than picking one and hoping.
- **The absolute calibration is unknown** — window, normalisation and headroom
  are the host's. Hence `Reference Level` (dBFS that reads zero on the scale,
  default −18 dBFS, EBU R68's alignment level) and `Sensitivity` (±12 dB trim).
  The README says the calibration is nominal.
- **It is not a sample-peak detector and cannot be.** A magnitude spectrum has
  no phase, so the time-domain peak inside the block is unrecoverable. The PPM
  and the bargraph are therefore driven by a block RMS: on a sine they differ
  from a real PPM by the crest factor of a sine (3.01 dB), and on anything
  peakier they under-read by that material's crest factor. **The ballistics are
  the standard's, exactly; the detector is not, and no check here pretends
  otherwise.** A real peak needs a real audio device — spasis has that code and
  it is a v0.2 job.
- **One spectrum per frame is the input's resolution.** A 5 ms transient inside a
  16.7 ms video frame is already averaged before the plugin sees anything, so
  the PPM's 3.16 ms integration time constant cannot be exercised by real
  programme material through this path. The oversampled engine cannot fix that;
  it is a property of taking audio from a video host.
- **Stereo Pair is two instruments on one mono spectrum.** FFGL has one buffer to
  give. A pair of VUs is the instrument people recognise, and both of them read
  the programme. The README says so.

## Frame one, and the priming trap

The spec warns that a detector is deaf for about a second and a half after a clip
trigger unless it is primed on the first frame. Needle's version of that trap is
in `Engine::Frame`, and it has two halves:

- **A clip triggered 40 seconds into a composition hands the plugin a first
  `SetTime` of 40.** An engine that reads that as elapsed time steps 192,000
  times before anybody has seen a frame: every meter settles instantly, the hold
  bar is stale before it was ever fresh, the eye is at full temperature. So
  `Frame` takes the host's clock *reading*, not a delta, and the first call after
  a `Reset` adopts the origin and takes no steps.
- **A zero interval must advance nothing.** The fleet's habit is to write a
  one-pole as `coefficient = dt > 0 ? 1 - exp(-dt/T) : 1`, which snaps the
  detector to full on the one frame it has no information about. `Follower::Step`
  and `Movement::Step` return immediately on `h <= 0`.

`--prime` checks both, and checks the thing that actually matters: the 99 %
crossing measured through the plugin's own frame path is the same from a clock
starting at 0 and one starting at 40. It is 300.24 ms either way, and the two
agree to 2.5 µs.

What is *not* primed, deliberately: the meters themselves start at rest and swing
up. That is not the detector being deaf, it is the instrument being a meter.

## The oversampled engine

`kEngineRate` is 4800 Hz, and `Engine::Frame` advances in whole `1/4800 s` steps
with the remainder carried. Three things set that rate, and none of them is
taste — the note in `meter/Engine.h` has the arithmetic. The one that matters
most: **a host frame is 16.7 ms at best and the standard's figure is 300 ms**,
so a frame is a twentieth of the quantity being specified. You cannot measure
the specification on that grid, and the integrator's answer would depend on the
host's frame rate.

`--rate` proves it did not: half a second of the same signal at 24, 30, 50, 60
and 144 fps all give a deflection of 0.712908590, spread exactly zero, against a
tolerance derived at run time from the movement's own steepest slope.

4800 rather than 4000 because it divides 24, 30, 48, 60, 120 and 240 exactly.

RK4 rather than a matrix exponential for the second-order movement, on purpose:
an integrator that is honest about being an integrator makes `--ballistics`
comparing it against the closed form a real test rather than a tautology. It
tracks to better than 1e-12 of full scale.

## Tolerance review — would this hold on another rasteriser, at another raster?

This is the section the round was for. Every numeric check in the harness is
listed with the reasoning for its tolerance, and the question asked of each is
the same: *could this pass here and fail on a GPU-less runner, or at a different
size?* The VFD's checks answer it in their own table, under "The VFD spectrum
analyser"; what follows is the four meters'.

**The structural answer, first.** Every check group but `--pixels` opens **no GL
context at all** — `--ballistics`, `--ppm`, `--steps`, `--eye`, `--prime`,
`--rate`, `--friction`, `--pair`, `--defaults`, `--free`, `--visible`, `--names` and `--font` drive `source/meter/` and the
parameter list directly. They cannot depend on a rasteriser or a raster because
neither exists while they run. That is the single most important decision in the
harness and it is why `verify.sh` groups them under "physics (no GL)".

### The physics checks (no rasteriser, no raster)

| Check | Tolerance | Where it comes from |
|---|---|---|
| VU reaches 99 % at 300 ms | ±5 ms | The spec's own allowance. The measurement resolves to ~1e-7 s (a 208 µs step, interpolated across a smooth curve), so the check is four orders coarser than its own resolution — it passes by construction, not by luck. Measured 300.000 ms. |
| VU overshoot | 1.0 %–1.5 % | The standard's band. Designed at its midpoint, 1.25 %, so there is a quarter of a percentage point of margin at each end. |
| RK4 against the closed form | 1e-9 of full scale | Derived: RK4's local error is `(ωn·h)⁵/120 = 1.5e-15` per step; over four seconds of a stable system the accumulation stays far under 1e-11. 1e-9 is a hundredfold margin on the bound. Measured < 1e-12. |
| 0 VU settles at 1/10^(3/20) | 1e-6 | Exact arithmetic; the only error is the movement's asymptotic approach, which after three seconds is `e^-33`. The 1e-6 is a deliberate floor so shortening the settle time cannot make it flaky. |
| PPM falls 20 dB | ±0.05 s of 2.8 s | The spec's allowance. The one-pole step is the *exact* solution over a constant target, so the only error is the step grid, interpolated. Measured 2.800000 s. |
| 5 ms burst reads 2 dB down | 1e-6 dB | Exact: 5 ms at 4800 Hz is 24 whole steps and the step is exact, so only double rounding in `exp`/`log` is left. |
| PPM at the reference reads 0 dB, deflects 0.5 | 1e-4 | After two seconds the follower equals its target to the last bit; 1e-4 covers `log10` rounding and leaves room if the settle time is shortened. |
| Bargraph steps are 3 dB apart; top at 0; bottom at −27 | 1e-3 dB | Forty bisections over a 72 dB bracket resolve to 7e-11 dB. 1e-3 dB is eight orders looser than the measurement and three orders tighter than the error worth catching (half a step is 1.5 dB). |
| Every scale's two maps are inverses | 1e-12 | The round trip is one `pow` and one `log10` in double, so its error is a few ulp of a number near one. 1e-12 is three orders above that. It is here because the two maps are used in opposite directions in different files, and a disagreement would leave the pointer right and every number on the face wrong — the one way a meter can be broken and still look plausible. Measured exactly zero. |
| Eye shadow is monotonic | 1e-9 slack | An ordering, not a measurement. The slack stops a *flat* region — both ends of the travel are clamped — being read as a rise. |
| Eye is 100° wide open | exact equality | The clamp makes it exact and 100 is representable. |
| Eye is open half a decibel below the overload | > 1e-3 ° | See the row below; the same floor, used as a lower bound so "shut" and "open" are separated by the same yardstick. |
| Eye is shut **at** the overload | < 1e-3 ° | **Derived from the raster, and this is the check that was wrong first.** A second-order movement approaches its target asymptotically, so the shadow angle at the overload is a small positive number for ever; `== 0` demands that an exponential arrive at its asymptote, and it duly failed at 2.4e-8 °. The honest claim is that it is shut *to anything that can be drawn*: the largest eye this plugin can draw is Size 1.0 on a 3840×2160 raster, radius 778 px, where one pixel of arc subtends 0.0736 °. A thousandth of a degree is seventy times finer than that, at the largest raster in play. |
| Eye is shut **past** the overload | exact equality | Here the model really is exact — `ShadowDegrees` clamps — so equality is the right claim. |
| Overlap is zero at the overload, positive past it | exact / ordering | `max(0, shown−1)` with `shown < 1` is exactly zero. |
| Frame one takes no steps; a repeated host time takes none; nothing moved | exact integers | No tolerance to get wrong. |
| Sixty frames integrate one second | one engine step (208 µs) | The whole-step accumulator guarantees the residual is under one step. |
| Two clock origins agree | one engine step (208 µs) | **The second check that was wrong first.** It asked for 1e-9 s, which is asking floating point to be exact: `40.0 + n/60.0` is simply not the same sequence of doubles as `n/60.0`, and the carried residual can cross a step boundary a frame earlier in one run. At most one step can separate them. Measured 2.5 µs. |
| The frame-path crossing is 300 ms | ±12 ms | The measurement is sampled on the host's 16.7 ms frame grid, so its resolution is a frame; 12 ms is inside one and still fails a movement out by a whole frame. The ±5 ms claim is made by `--ballistics`, where it can be. Measured 300.24 ms. |
| A worn pointer comes to a dead stop | exact equality on the velocity | It either sticks or it does not; there is nothing to round. |
| ...inside the dead band the friction implies | `deadBand × (1 + 1e-12)` | Not a fitted number: the step's own stopping condition *is* `|ωn²(u − x)| ≤ friction`, which is `|u − x| ≤ deadBand`, so a pointer that has stopped satisfies it by construction. The 1e-12 is a double-arithmetic allowance on a comparison against the same quantity. Measured 0.0201 against a band of 0.03, from both directions. |
| ...short of the target, and at a different place each way | orderings | The signature of dry friction as against damping: a damped movement arrives, a dry one stops. Measured 0.0402 apart. |
| Wear 0 is bit-identical to no friction | exact equality, every step for two seconds | The headline ANSI C16.5 claim is made at Wear 0, so this is the check that stops the friction path reaching it. Not a tolerance: `==` on both state variables at every one of 9,600 steps. |
| Five frame rates agree | `2 × maxSlope × 1/rate`, computed at run time | Nothing is hard-coded: the harness measures the movement's steepest slope from its own trajectory and multiplies by one step, which is the most two frame rates can differ by. Measured spread exactly zero against an allowance of 2.4e-3. |
| Both meters of a Stereo Pair agree after Mono and back | exact equality on every published field, every paired frame | Both channels are fed the same programme at the same instants and stepped by the same code, so there is nothing for a tolerance to absorb. Its negative control is v0.1.0's rule rebuilt from the public API (a second engine whose clock only runs while paired); it must fail all 8 scenarios, and it does, with the same numbers as a build of the real v0.1.0 source. |
| Free agrees with Standard at the defaults | 1e-4 relative | The controls are floats (~7 significant digits) and the rise map amplifies relative error by `ln 100 = 4.6`, so the floor is ~5e-7. 1e-4 is two hundred times that and still far tighter than the gap between any two values anybody would confuse. |
| Reference, trim, hold time, hold decay | 1e-4 / 1e-3 absolute | Exact linear maps of exactly-representable constants; the tolerance is a float-rounding allowance. |
| Names, duplicates, blank and duplicate glyphs | counts | No tolerance. |
| The level law is `ffglqs::Audio`'s volume | 1e-3 dB | The SDK sums float squares into a float: at most 64 ulps of the power, 3.8e-6 relative, 1.7e-5 dB of level. 1e-3 dB is fifty times that and eighteen thousand times below the 18.06 dB it exists to catch. Measured 1e-6 dB at worst, on four spectra. |
| The same power in one bin as in 64 reads the same | 1e-3 dB | Same float argument. It is the Parseval claim, now exercised where `inject()` used to exercise it. |
| SDK volume −18 dB reads 0 VU at the default, Magnitude and Power | 1e-5 of full scale | Float bins are 6e-8 relative, so about 4e-8 of deflection; three seconds leaves the movement e^-33 from its target. v0.1.1 settles at 5.66. |
| The stop: 24 dB over 0 VU never passes 1.035, and leaves within a frame of the level going | exact / one frame | The clamp makes the first exact. From the stop at rest, one 16.7 ms frame of the spring moves it about 0.026 off. Measured: off on the very frame the level goes. v0.1.1: 11.36, and 13 frames. |
| Free, engine rate: a step in the faster direction overshoots by exactly Mp(ζ), the slower by no more | `2·ω²·Mp·h²` of the step, per case, + 1e-9 | The sampled extreme misses the true one by half the curvature times a step squared, and the curvature at an extreme is the stiff spring's pull, ω² times the excursion. 1e-7 at the defaults, 7e-4 at a 20 ms Rise and 40 %. A sample can only fall short of the peak, so "no more than" needs only the RK4 floor. v0.1.1: 11.6275 % at the defaults, 15 of 36 steps wrong. |
| Free, engine rate: 99 % in exactly Rise (up) and Fall (down) | 2 µs | Linear interpolation between two steps on a nearly straight stretch of curve; the 99 % crossing is before the target crossing, so it is entirely on the direction's own spring and the solved ωn is exact. |
| Free, engine rate: a beat during a slow (20 s) fall reaches 99 % within the 20 ms Rise + 5 % | 5 % | Not derived: an ordering with room. The stiff spring stops the slide in microseconds; measured 20.21 ms. A pure "choose by velocity" rule would take seconds here. |
| On Standard `DriveMovement` is `Movement::Step` | exact equality, both state variables, 19,200 steps | One spring, early return. The ANSI figures are measured on `Movement::Step`. |
| Free through the plugin at 60 fps: the shipped defaults overshoot Mp on the VU and the eye | `−2·ω²·Mp/60²` | The frame grid instead of the step grid, same argument. Measured 1.2500 % and 1.2481 %. v0.1.1: 11.63 % and 11.24 %. |
| Free on programme: never past the programme's range by more than `2·Mp/(1−Mp)` of its span | that bound + 1e-9 | For a linear second-order movement driven anyhow between two levels, the worst excursion is the impulse response's absolute integral less one, `2·Mp/(1−Mp)`. Braking on the stiffer spring only shortens an excursion. Four Rise/Fall settings, beats and dips. v0.1.1 failed 4 of 8. |
| Visibility at construction, on all 4 Types × Standard, and the events raised | exact | Flags, not measurements. |

### The pixel check (one rasteriser, two rasters)

`--pixels` is the only check that reads a rendered frame, and everything in it
is arranged so that a different rasteriser cannot change the answer:

- **Every expected colour is computed from the parameters, never read off a
  screen.** The face and pointer are set to 0.8/0.4/0.2 and 0.2/0.6/1.0, which
  land on 204, 102, 51 and 51, 153, 255 with three parts in a million to spare.
  The tolerance is **±1 code value**, which is the float-to-unorm allowance the
  GL spec permits and nothing else. A default like 0.90 would land on 229.5,
  where round-to-nearest is a coin toss two GPUs may call differently — which is
  exactly how a check gets calibrated to one Mac.
- **Every probe is a flat interior.** Coverage in this shader is
  `clamp(0.5 − d, 0, 1)`, a one-pixel linear ramp, so any point half a pixel
  inside a feature has coverage of *exactly* 1.0 on any hardware. `smoothstep`
  was rejected for this: its two-pixel shoulder would put every nearby probe at
  the mercy of the GPU's own `smoothstep`.
- **The one probe that sits on a line has its precondition asserted, not
  assumed.** The worst-case distance from a pixel centre to a line through its
  pixel is `√2/2 = 0.707 px`, so a centreline probe needs a half-width of at
  least 1.21 px. `--pixels` runs at Size 1.0, where the half-width is 2.16 px at
  640×360 and 6.48 px at 1920×1080 — and the check *tests that* before using it.
- **Every coordinate comes from the `Frame` the plugin produced.** The CPU
  computes all pixel geometry and the shader only fills it, so a probe is never a
  transcription of the layout into the test; if the layout moves, the probe moves
  with it. `NeedlePlugin::LocalToScreen` is the one supported way to turn that
  geometry into somewhere to look.
- **Everything runs at 640×360 *and* 1920×1080**, and the raster is stated in the
  output. Nothing in the file is true only at the size it was written at.
- **Several checks are relational rather than absolute**, which removes the
  question entirely: a lit bargraph step is asserted to differ from a known-dark
  one and every dark step to match it, byte for byte, with no colour rule
  transcribed into the test at all.
- **The one restatement of a rule** — how many steps a given level should light —
  is three lines of the LM3915 law, and `--steps` recovers the same thresholds
  independently by bisection without transcribing anything. If the two ever
  disagree, one of them is wrong and it will be obvious which.
- **graticule's burn-in plate, four times over.** `Instance::quiet()` turns off
  the Lamp, the Glass, the Wear, the scale numbers *and* the hold bar before any
  probe. Every one of them draws over the face and would move a flat-interior
  probe. Three of the five were found the way graticule found its plate: by a
  probe reading the wrong thing.

Every assertion in `--pixels`, and which of the above it rests on:

| Assertion | Class |
|---|---|
| Both corners are the background colour | flat interior, ±1 |
| The dial's face is the face colour | flat interior, ±1, probed 1.6 arc radii from the pivot and a fifth of the half-height inside the corner |
| The pointer is at least 1.21 px of half-width | the derived precondition, asserted before it is used |
| The pointer's centreline is the pointer colour | flat interior on a line, ±1 |
| The mirror of that angle is the face colour | flat interior, ±1 |
| 0 VU moves the pointer right by more than 0.05 rad | ordering; the real move is 0.667 rad |
| ...and the pointer is drawn at the new angle too | flat interior, ±1 |
| With Background off the face is still opaque and unchanged | flat interior ±1, plus `alpha == 255` exactly |
| ...and the corner is `alpha == 0` | exact integer |
| −7.5 dB lights seven of ten steps | integer equality against the LM3915 law, cross-checked by `--steps` |
| Every lit step differs from a known-dark one | exact 3-byte comparison, no colour rule transcribed |
| Every unlit step matches it | exact 3-byte comparison |
| A burst leaves a hold bar; the column has fallen; the bar is not the unlit colour | orderings and exact comparisons |
| The eye's shadow is part open at 20 dB down | a sanity bracket, not a measurement — the measurement is `--eye` |
| The lit sector and the shadow are different colours | exact 3-byte comparison of two flat interiors |
| Past the overload the shadow is shut and the wings overlap | exact equality and an ordering, on CPU state |
| ...and the bottom of the target is lit | exact 3-byte comparison |
| Stereo draws two units, side by side, both probes on the raster, both faces exact | orderings, bounds and flat interiors ±1 |

Three bugs in `--pixels` were found and fixed during that pass, and all three
were the test being wrong: an expected step count off by one (the law says seven
at −7.5 dB, not eight), the hold bar sitting over the segment a probe wanted,
and a stereo pair at Size 1.0 hanging off a 16:9 raster so the probe read the
out-of-bounds sentinel. The last of those now has an explicit "both probes land
on the raster" assertion in front of it, so a future layout change fails loudly
instead of quietly reading zeros.

### The sweep

`tools/sweep.py` compares two renders byte for byte and calls a control dead if
*nothing* changed. There is no tolerance at all, so there is nothing to
calibrate. What it does need is context, and the interesting case is
`Persistence`: swept after a burst it reported dead, **correctly** — by then the
eye's shadow is clamped at one end of its travel and a lagged clamp is the same
clamp. It is swept on the onset instead, 217 ms in, while the shadow is part way
across.

## The traps actually hit

- **A nested `struct Channel` and a member `Channel()` accessor** do not coexist:
  clang reports "must use 'struct' tag" at four unrelated lines. The struct is
  `Models` now.
- **Asserting `== 0.0` on an asymptote.** See the eye, above.
- **`sign(v)` inside an RK4 stage is meaningless.** Dry friction is
  discontinuous at zero velocity, so it is applied after the step rather than in
  the derivative, semi-implicitly: if the step's own velocity decrement would
  reverse the pointer, it either stops dead or the spring drags it through.
- **A hidden meter that stopped.** v0.1.0's `StepAll` looped over `channels`,
  which was Count, so while Count was on Mono the right meter took no steps at
  all, and on Stereo Pair again it resumed from the level it had when it
  stopped — for about two seconds it disagreed with the left one, as seen on
  the bargraph while filming with `ndtest --pipe` on 2026-09-23. Re-seeding it
  from the left meter on reappearance was the other option and was rejected: a
  re-seed is a jump the model did not make, and it would need its own rule for
  every state variable (the hold bar, the eye's heater, a worn pointer's
  resting place). A meter that is simply always running needs no rule at all.
  So since v0.1.1 **Count decides how many meters are drawn, never how many
  run**: `Engine::Frame` integrates and publishes every channel on every frame,
  and a channel past the inputs supplied reads the last one. The cost is a
  second set of four scalar models at 4800 Hz, which `--bench` cannot see.
  `ndtest --pair` checks it through the plugin's own parameter list, on all
  four Types, with the level falling and rising while the right meter is
  hidden, and carries its own negative control.
- **Asserting that two floating-point clock origins agree bit for bit.** See
  `--prime`, above.
- **The level law's footing (needle#1).** The sum over 64 bins assumed a
  full-scale sine sums to 1.0; Resolume's bins evidently sit on the SDK's
  per-bin 0..1 footing, and the sum read 18.06 dB hot. Every harness check
  injected exactly what the law expected, so nothing here could see it — the
  check that does now compares against the SDK's own code, not against a
  restatement of the law.
- **Switching a second-order movement on the wrong variable (needle#1).** Free
  chose the rise's or the fall's spring by `u >= x`. A rising pointer that passed
  its target got the fall's weak spring and weak damping and coasted: 11.6 %
  overshoot at the defaults, 83 % at Fall 20 s, and a beat programme read well
  above its loudest moment. `--defaults` compared the resolved constants and
  passed throughout, because the constants were right; only a trajectory check
  sees it. Two tempting fixes are wrong too. Choosing by the sign of the velocity
  ignores a beat that arrives during a slow fall for seconds (the fall's spring
  has to turn the pointer round). Braking on the stiffer spring for a whole step
  hands a pointer creeping the wrong way at rest a kick of `ω²·(u−x)·h` — four
  full scales a second at a 20 ms Rise — so the step is split at the turn.
- **A dial's proportions are not free either.** The first version put the pivot
  inside the face with a radius slightly under its half-height, and the result
  was a tiny arc floating in a big blank rectangle. A VU's arc is *shallow*
  because the sweep is about 54° and the arc has to span most of the window —
  which forces a radius near three times the window's half-height and puts the
  pivot well below the glass. `drawDial` therefore masks everything to the face,
  and the hub is simply not there, as it is not on a real meter.
- **`Peak Hold` of zero used to mean "release instantly", which still draws a
  bar.** It now means no bar. That is both the better control and the hook the
  pixel check needed.
- **The GLSL reserved words** cost nothing this time because they were known
  going in: `patch`, `sample`, `input`, `output`, `filter`, `common`, `active`,
  `half`, `layout`, `flat`. The local for a fractional part here is `fp`.
- **The fragment shader is over MSVC's 16 KB literal cap**, so it is two adjacent
  raw strings and `verify.sh`'s extractor joins them. The extractor now fails if
  it finds fewer than two shaders, because "extracted nothing" used to pass.

## Design decisions, made rather than asked

- **Standard locks the ballistics out of the model, not merely over it.** On
  Standard, `Resolve()` does not read Rise, Fall or Overshoot at all.
- **Peak Hold and Hold Decay stay live on both settings**, because no standard
  specifies them and calling them one by association would be a lie.
- **Free is deliberately unphysical**: a rise that differs from a fall is a
  switched system and no mass on a spring does that. It is there because an
  operator wants a meter that looks good on a beat, and saying so is better than
  pretending the result is still a VU. Which spring runs is decided in
  `DriveMovement` (Engine.h has the long version): at rest or heading for the
  target, the direction's own; carried past it, the stiffer, until it turns
  round. That keeps Rise and Fall exact (both are 99 % times, reached before the
  target), keeps Overshoot exact for the faster direction and an upper bound for
  the slower, and catches a new rise at once.
- **The VU has stops**, inelastic, at −0.02 and 1.035 of full scale — the same
  numbers the drawing has always clamped to (`kDialStopLow/High`). A real pointer
  meets a pin; without one the model ran on to eleven times full scale and sat
  pinned for 217 ms after the level went. The ANSI checks never reach either
  stop, and `--rate` still reads 0.712908590. The eye has none, on purpose: its
  wings are meant to cross past closure, and the PPM has no movement.
- **The inspector shows only what can act** (`ApplyVisibility`, the rules in
  Controls.h, "Hidden is not dead"): Rise and Fall with Standard off, Overshoot
  with Standard off on the VU and the eye, Peak Hold and Hold Decay on the
  bargraph, Persistence on the eye. Standard and Type are never hidden — the
  switch that brings a thing back must stay (vectrix's rule). Hidden at
  construction without events, then events raised on every Type or Standard
  change, which is exactly what vectrix measured Arena honouring. Standard heads
  the Ballistics group so that, on Standard, the group says where the rest went.
  Its id did not change (5: the last of Meter, now the first of Ballistics), and
  a composition stores names, not ids, anyway (vectrix's docs/NOTES.md).
- **The bargraph shares the PPM's detector.** An LED meter and a moving-coil PPM
  off the same rectifier differ in the display, not in the detector.
- **The PPM has no movement of its own**, only a detector: IEC 60268-10
  specifies the *indication*, not the mechanism, so all its ballistics live in
  the follower and the pointer simply follows. One consequence is that Wear does
  not stick a PPM — there is no pivot in this model to stick. A real BBC PPM
  does have one, and modelling it would mean a second-order stage after the
  detector with constants nobody publishes.
- **Wear is one control doing two jobs.** The shader's half is dirt on the glass
  and a dim phosphor; the engine's half is dry friction in the pivot, which is
  a different animal from damping — it stops the pointer *short*, anywhere in a
  dead band of `friction/ωn²`, and leaves it somewhere different depending on
  which way it came. The band is expressed as a fraction of full scale (3 % at
  Wear 1) and converted to an acceleration per movement, so it means the same
  thing whatever the ballistics are. At Wear 0 — the default — it is bit-identical
  to no friction, and `--friction` asserts that rather than assuming it.
- **The magic eye borrows the VU's movement on Standard.** Nothing specifies an
  eye; the VU's is at least a published pair of numbers.
- **The eye's fluorescence is a fixed tube green** and `Face` colours the bezel,
  so `Needle Colour` does nothing on the eye. The sweep knows.
- **A BBC PPM has no red band.** The sentinel for "none" is a `redFrom` at or
  above the arc's half angle.
- **Text is pixel-exact only at Rotation 0.** At any other angle the glyph grid
  is resampled nearest-neighbour and the labels get ragged edges. Filtering them
  would soften every other edge on the instrument.
- **`StoatworksAbout.h` is GENERATED by stoatworks-backend's `sync-about.py`,
  and `ATTRIBUTIONS.md` by `sync-attributions.py`.** Until registration on
  2026-09-23 they were hand copies with `guide=""`; the first real sync added
  the user-guide link, so the About block is now five entries (About text, User
  guide, Project page, Source on GitHub, Support the work) — `PT_ABOUT_BUTTON_4`
  in `Controls.h` — and the plugin had 37 parameters (`ndtest --list`: 31
  swept controls, the Audio buffer, and the About block). The VFD appends six
  after it (ids 37–42), so there are 43: 35 swept, the Audio buffer, the About
  block, and Audio Input and Rescan Inputs, which are never swept because they
  reach real devices. The `static_assert`
  in `Needle.cpp` ties `Controls.h` to the generated header's `kParamCount`, so
  a future sync that changes the button count will not compile until
  `Controls.h` follows it. Never edit either file by hand.

## The VFD spectrum analyser (Type = VFD)

A fifth element of Type: a 1980s hi-fi vacuum-fluorescent spectrum display.
The design, every source and the reasoning behind each number is
`specs/SPEC-needle-vfd.md` (the Stoatworks specs folder, beside the repos);
this section is what was built, what it measures, and what bit.

**Unreleased.** On branch `claude/vfd`, 2026-11-16. No version bump, no
release notes, never in a host with a real device, never in Resolume.

### What it is

- **The bank is earshot's**, an unpatched snapshot (`external/earshot/`,
  `SNAPSHOT` names commit 8124191): IEC 61260 base-ten third-octave centres
  x = −17 … 13, Butterworth band-pass with pre-warped edges, Fast (125 ms)
  weighting, as many bands as fit under 0.95 Nyquist — 31 at 48 kHz, 30 at
  44.1 kHz, where the 20 kHz column stays dark. Resolume's 64 bins are not
  used: nobody knows what they are (Bin Law), and a linear 64-bin spectrum
  has no bin inside the low thirds.
- **Octaves** (Bands = 10 Octaves) are the power sum of three thirds, which
  IEC 61260's base-ten midbands are designed to allow (IS 6964 note 1 to
  3.5). Whether the sum meets the octave mask is measured (O1), not assumed.
- **12 segments at 3 dB**, the bargraph's LM3915 step: the top one lights 3 dB
  under Reference Level, the bottom 36 dB under. Reference Level and
  Sensitivity keep their meanings.
- **The peak hold** is the display's own, in earshot's audio time: the dot
  stays while the bar reaches its segment, waits Peak Hold after the bar
  leaves it, then falls at Hold Decay. Not earshot's `Third::holdDb` (traps,
  below).
- **The multiplexed scan**: one grid per column at the Multiplex Rate,
  20 µs blanking (inside Noritake's 10–50 µs). The eye sees the time average,
  so on the eye the rate changes nothing, by design. Shutter > 0 is a
  rolling-shutter camera (pitch's row model, 16 ms readout): each row's
  exposure catches its own slice of the scan, in closed form.
- **Four filament wires**, 37.5 µm (Futaba US6803709's conventional filament:
  24.5 µm core plus 2 × 6.5 µm oxide), drawn by exact box coverage, so they
  are faint because they are thin.
- **The phosphor** is Noritake's standard green, CIE (0.24, 0.41), taken to be
  ZnO:Zn. It is outside sRGB, so it is mapped to the gamut boundary along the
  line to D65 and normalised to a peak of 1: (0, 255, 198).
- **The status lamp** (woofer's): green delivering, red Silent, red blinking
  Missing or failed, amber on no device — and on amber the display is dark.
- **Code**: `source/Vfd.*` (`vfd::Input`, the only code that calls earshot;
  `vfd::Display`; `vfd::Picture`), `source/VfdShaders.cpp` (its own program,
  so `Shaders.cpp` and the demo's copy are untouched), `source/VfdRender.*`,
  and `NeedlePlugin`'s VFD branch. The meter engine never sees Type 4.

### Devices and CoreAudio

- **VFD off never touches CoreAudio**, not even to list devices (rubens found
  earshot's lookup starts CoreAudio for a name it then fails to match). The
  Engine, the device list and any open happen on the first VFD frame, never in
  the constructor. With the VFD off the Audio Input menu is None, System
  default and fourteen "(none)", and a saved device key waits.
- **A device opens only when the user picks one.** Leaving the VFD closes it;
  coming back reopens it. Rescan Inputs lists again, and only on the VFD.
- **Menu values are `earshot::deviceKey(name)`**, so a composition remembers a
  device by name, whatever order the machine lists them in.
- `NEEDLE_NO_DEVICES=1` skips listing and refuses a device name before
  earshot sees it. verify.sh sets it for oxbow and the sweep, and for every
  VFD check except `--coreaudio`, which needs the real path to prove it is
  never taken. `--devices` clears it for itself (its backend is scripted, and
  its case (j) sets the gate to prove the refusal).

### The checks, and would each hold on another rasteriser, at another raster?

Audio goes in through earshot's `feed()` on a scripted capture backend
(`tools/ndtest/Vfd.inc`), so no check lists or opens a real device. Every
picture check runs at 1280×720 and at 320×180 (Size 1), and verify.sh runs
each on the GPU and on Apple's software renderer (`NDTEST_RENDERER=software`,
renderer chosen by id): every one passed on both, most with identical numbers.
Every check has a negative control (`ndtest --negative`: 20 of 20 caught).
Expected values come from the standard, the sources or the spec's millimetres,
and the measured side from earshot's output or the picture: never a second
copy of the formula under test.

| Check | Measured (2026-11-16) | Tolerance, and where it comes from | Another rasteriser, another raster? | Negative control |
|---|---|---|---|---|
| I1 `--iec` (no GL): centres, labels, edges | worst centre 0.377 units of Table A.1's fifth figure; 0 labels wrong; edges within 47.9e-9 relative | half a unit of the printed figure; 1.2e-7, float's rounding (the Frame carries float) | No GL and no raster: it cannot depend on either | earshot `thirdBase2` |
| I1: a sine at each exact midband, in its own band | worst 0.001 dB (48 kHz), 0.006 dB (44.1 kHz) | 0 ± 0.3 dB, class 1 at Ω = 1 (IS 6964 Table B.1); averaged over whole Fast ripple periods, so the 2 kHz ripple is not in the number | No GL | earshot `thirdNoPrewarp` |
| I1: a sine at each shared edge, in both bands | 3.010 dB down, all 62 (48 kHz) and 60 (44.1 kHz) | 2.0–5.0 dB, class 1 at the edge (Table B.1) | No GL | as above |
| I2 `--skirt`: earshot's neighbours | least margins: k±1 1.50 dB, k±2 3.70 dB, k±3 2.61 dB | the class 1 minimum at Ω = G^(1/3), G^(2/3), G: 14.488, 30.759, 44.299 dB, from Table B.1's breakpoints by eq. (12), linear in lg Ω | No GL in this half | earshot `thirdOrder2` |
| I2: the picture, every segment probed | own column 12 lit (31/31); 0 over the mask, 0 off the segment law, both rasters | counts: no tolerance. Each probe sits a quarter of a segment below its centre, inside it by 1.1 px at 320×180 and 2.8 px at 1280×720, so coverage at the probe is 1 on any rasteriser | Yes: a probe is a flat interior, the threshold is half scale against 0 or 255 | `shift` (column c shows band c + 1) |
| I2 at 44.1 kHz | the 20 kHz column dark, the 16 kHz column full | counts | as above | — |
| O1 `--octaves` | own octave worst 0.128 dB; neighbours ≥ 35.90 dB down; 20 edge readings 2.997–3.010 dB; picture 0 wrong | 0 ± 0.3 dB, ≥ 17.5 dB, 2.0–5.0 dB: IS 6964 Table 1, class 1 | earshot's half has no GL; the picture is counts at flat interiors | `amplitudeSum`, `octaveWrong` |
| H1 `--hold` (no GL): 24, 30, 60 fps against 144 fps at 30 common audio times | 0.0028, 0.3000, 0.1000 dB | Hold Decay × one frame + the 2 kHz ripple, 10·lg(1 + r), r = 1/√(1 + (4πfτ)²): 0.5028, 0.4028, 0.2028 dB | No GL. And not the frame rate either: that is what it measures | `holdInFrames` |
| H1: how long the dot held after the bar left it | 1.4931, 1.4833, 1.4667, 1.4583 s (144, 60, 30, 24 fps) | Peak Hold 1.5 s ± one frame: the leave is seen on a frame | No GL | `holdNoWait`, `holdAtPeakOnly` |
| H1: the fall's slope | 12.000000 dB/s at every rate | Hold Decay within 1e-6: it is Hold Decay × dt in double | No GL | as above |
| H2 `--hold`: an 8 s note | the hold never dropped a frame before the bar left it (0.000000 dB); then waited 1.493, 1.483, 1.467, 1.458 s | no drop at all; Peak Hold less one frame at least | No GL | `holdAtPeakOnly` (waits 1.375–1.433 s) |
| M1 `--multiplex`: a 2-scan exposure is the eye's picture | 0 code values different, 372/372 segments lit, both rasters | ±1 code value, the float-to-8-bit step | Yes: the claim is a ratio of closed-form overlaps that is exactly 1 for a whole number of scans; no coverage is involved beyond what both pictures share | `normFullSlot` |
| M2: 1/8000 s, the lit column advances down the picture | 0.06864 columns a row at 1280×720 (want Tr·N·f/H = 0.06889, limit ±0.00867); 0.27554 at 320×180 (want 0.27556, ±0.02113); never more than 2 columns lit in a row | each row's centroid can sit half a column off; a least-squares slope over rows spanning L moves at most 3e/L for errors bounded by e | Yes: the expected slope is computed for the raster in hand, and the tolerance scales with it | `readoutUp` |
| M3 `--clock`: host 499 000 s draws what host 0 s draws | 0 code values, both rasters, while consecutive frames differ by 132 390 and 30 240 subpixels | ±1 code value. At Multiplex Rate 500 Hz (the top of the travel, exactly) both clocks are whole numbers of scans | Yes: two pictures from one rasteriser compared with each other | `phaseFloat` |
| F1 `--filament`: darkening summed over the rows a wire crosses | 0.2769 px against 0.2779 at 1280×720 (37.5 µm × 7.410 px/mm); 0.1054 against 0.1125 at 320×180 | the rows' 8-bit rounding, summed: at most 0.0126 and 0.0176 px | Yes: box coverage is exact on any rasteriser that samples pixel centres, and the limit is the rows' own rounding | `wireCoreOnly`, `wireTen` |
| P1 `--phosphor`: a lit interior's chromaticity | (0, 255, 198) in all 31 columns; xy (0.2455, 0.4039), on the D65 line with 0 excess, 92.5 % of the way to (0.24, 0.41) | half a code value in each channel, propagated to xy | Yes: an interior pixel is the encoded constant, no coverage | `willemite` |
| G1 `--geometry`: the bottom segment's four edges in every column, both layouts | within 0.0020 px of the spec's millimetres | each partly covered pixel's half-code rounding, + 0.001 px of float: at most 0.0088 px | Yes, and this is the one that asked: the GPU measured 6.596 px for a 6.600 px segment at 320×180, the software renderer 6.600–6.604; both inside. The coverage ramp is exactly a pixel's overlap with a half-plane, so the sum is the edge's position on any rasteriser that samples pixel centres | `segFull` |
| R1 `--resize`, both directions mid-hold | every column's bar and dot equal a fresh run at the new raster, and the picture byte-identical | exact | Compared with a fresh render on the same rasteriser | `resetOnResize` |
| D1 `--devices` (a)–(j) | the menu before and after the first VFD frame, keys by name, each state and lamp colour, Rescan, leaving and returning, a saved key, `NEEDLE_NO_DEVICES` | flags and colours from the plugin's constants; the lamp probe is its flat centre | Yes, at both rasters and on both renderers | `menuByIndex` |
| C1 `--coreaudio` (real backend) | VU, PPM, bargraph and eye, every control at 0, 1 and 0.5, Audio Input set to two keys, Rescan pressed, 20 frames: Engines 0, lists 0, selects 0 | must be 0 | Counters, not pixels | `listAtConstruction` |
| V1 `--visible` | to the VFD and back: every visibility change raises its event; a control set while hidden is read when shown | exact | No GL | `showAllOnVfd` |
| X1 `--pipe` (verify.sh, bash) | `\| head -c 1` exits 1 with "the reader closed the pipe after 0 frame(s)"; a 3-frame VFD clip is exactly 691 200 bytes | exact | — | `NDTEST_NEGATIVE=pipe` gives 141 |

**Cost** (`ndtest --bench`, arm64, fastest of five passes of 200 frames): the
VFD 0.583 ms/frame at 1280×720 and 0.593 at 1920×1080, the meters 0.015 and
0.013. Flat across rasters, so it is earshot's analysis and the harness's
`feed()`, not the shader. Apple Silicon only.

**C1 counts calls at earshot's three CoreAudio-capable entry points**
(Engine built, devices listed, device selected) inside `vfd::Input`, and
verify.sh greps that nothing else in `source/` includes earshot. Nothing
observes CoreAudio from outside the process: dtrace needs root. That is the
limit of the claim.

**Existing behaviour, measured.** The origin/main build (840193c) and this
branch rendered 72 VFD-off scenes (four Types × nine settings × 1280×720 and
320×180) byte-identical on the GPU, and the same 36 at 320×180 byte-identical
on Apple's software renderer (the baseline built with the same
`NDTEST_RENDERER` switch, harness-only). Every pre-existing verify.sh step is
unchanged and passes.

### The mutants (`tools/mutate.sh`)

One character of `source/VfdShaders.cpp` changed per run, rebuilt, and run
against the check that must notice, under a 300 s perl alarm (a hang counts as
caught). Run 2026-11-16, arm64, on the committed tree (after the `--devices`
fix below):

| Mutant (one character) | What it means | Caught by |
|---|---|---|
| `float g = 1.0;` → `2.0` | the eye's segments twice as bright | `--phosphor` (3 assertions) |
| `clamp( p - floor( p ) - a, 0.0, w )` → `…, a )` | the grid's pulse as long as its start time | `--multiplex` (3) |
| `floor( f.y ) * uScan.z` → `f.x` | the rolling shutter rolls across the columns | `--multiplex` (3) |
| `uScan.w * uSlot.z` → `uSlot.x` | the camera normalised by the slot, not the pulse | `--multiplex` (3) |
| `uScan.x > 0.5` → `<` | the camera model applied to the eye | `--multiplex` (5) |
| `max( y - 0.5, …` → `y - 0.6` | the pixel row's box 0.1 px too tall in the wire cover | `--filament` (2) |
| `wireCover( p.y, …` → `p.x` | the wires covered by x, not y | `--filament` (3) |
| `pow( l, 1.0 / 2.4 )` → `2.2` | a 2.2 power instead of the sRGB curve | `--phosphor` (3) |
| `uGrid.y - 1 - row` → `- 2 -` | every bar one segment high | `--skirt` (4) |
| `floor( q.x / uField.z )` → `q.y` | columns found from y | `--skirt` (4) |
| `vec3( 1.0 )` → `vec3( 0.0 )` in the clip | every lit segment black | `--skirt` (4) |
| glass `vec3( 0.0 )` → `vec3( 1.0 )` | the glass white | `--skirt` (4) |
| `length( p - uLampA.xy ) - uLampA.z` → `+` | the status lamp drawn inside out (invisible) | `--devices` (9) |
| `-d.x * sn` → `+d.x * sn` | the rotation's sign: a turned display mirrored | `--geometry` (3) |
| `0.5 * uSeg` → `0.6 * uSeg` | every segment 20 % larger | `--geometry` (7) |
| `+ 0.5 ) * uField.zw` → `+ 0.6` | every segment a tenth of a cell off centre | `--geometry` (7) |
| a space before a `;` | a no-op edit, which must NOT be caught | `--skirt` passes, as it must |

17 of 17 behaved as expected.

The lit-segment test (`fromBottom < uLit[ col ] - 0.5 || ...`) is not mutated:
the table is split on `|`, and a `||` inside a mutant silently becomes an empty
field. Two of these were missed on the first run and are why `--geometry`
exists: a segment 20 % larger, and the rotation's sign (no VFD check turned the
display). The rotation's sign then passed the first `--geometry` too, because
with every segment lit the turned display is symmetric: a mirror of it is
itself. The check now lights only the lower segments.

The lamp mutant first counted 12 failed assertions. That run was before
`--devices` cleared `NEEDLE_NO_DEVICES`, which mutate.sh exports, so the gate was
refusing the scripted device as well. Re-run on the committed tree it fails 9:
still caught, and the only change to `--devices` between the runs was clearing
the gate, so the 3 extra came from the gate.

### Traps hit building it

- **earshot primes each third's Fast envelope with the first update's whole
  block**, so the onset depends on how much audio the first frame carried. A
  1 kHz tone from sample 0 read 0.90 dB apart at 167 ms between 24 fps (a
  2000-sample first block) and 144 fps (333 samples), 0.20 dB at 333 ms,
  0.05 dB at 500 ms and 0.0008 dB from 1 s on. `--hold` starts its tone after a
  quarter second of silence, so every rate primes on zeros. A real display meets
  this only for the first half second after the VFD is chosen.
- **earshot's `Third::holdDb` starts its wait at the level's last maximum** (it
  restarts only when the level is ≥ the hold value). At the end of a note that is
  before the bar falls out of the dot's 3 dB segment, so the dot drops early by
  however long the bar took to leave: measured on earshot's own hold (1.0 s) in
  the `--hold` run, it began to fall 0.910–0.967 s after the bar left the dot;
  with that rule in the display (`holdAtPeakOnly`) the dot held 1.375–1.433 s of
  a 1.5 s Peak Hold. **The first reading of it was wrong** and is recorded so
  nobody repeats it: that a held note's ripple would make "≥ the hold" ever
  rarer, so earshot's hold would time out during the note. Measured, it did not
  (0.00 dB fallen 0.5 s after an 8 s note): at these frame rates a frame samples
  the 2 kHz ripple at a few fixed phases, so the maximum recurs exactly.
  earshot is not patched.
- **The level is still settling through the first second of a note** (the Fast
  envelope: 0.05 dB between 0.5 s and 2.8 s), so "peak minus hold" during a note
  reads the rise as a sag. H2 measures frame-to-frame drops instead.
- **make compares mtimes to the second.** The first mutant after the clean
  build ran the old binary and was "missed", twice. `mutate.sh` deletes the
  object and the binary before every build.
- **earshot's mutations must be set before the stream begins**: the bank is
  designed in `feedBegin`, so a mutation set afterwards changes nothing and its
  negative control "passes". `ArmEngine` sets them right after the Engine is
  built. Likewise a check at 44.1 kHz needs its own engine: re-feeding a 48 kHz
  engine at 44.1 kHz describes the old bank.
- **`--level` for the VFD is the programme's total**, 31 sines each
  10·lg 31 = 14.9 dB under it. Each at `--level` lit every column to the top.
- **`CFFGLPlugin::FindParamInfo` is protected**; element values are read back
  through the public `GetParamElementDefault` (woofer's pattern).
- **FFGL cannot add elements after construction**, so Audio Input has a fixed
  16 slots with "(none)" spares, renamed in place, and the plugin raises
  `FF_EVENT_FLAG_ELEMENTS` when the list changes. Whether Resolume honours that
  event has not been seen.
- **`NEEDLE_NO_DEVICES` refuses scripted devices too.** Inherited from
  verify.sh, it failed `--devices` (d)–(i) on the first full run, and would
  have made `menuByIndex` look caught for the wrong reason. `--devices` now
  clears it for itself.
- **The Multiplex Rate default, 0.30103, is 100.00001 Hz**, not a whole number
  of scans a second. `--clock` uses 500 Hz, the top of the travel, exactly.
- **IS 6964 eq. (12) specifies linear interpolation in lg Ω** between Table
  B.1's breakpoints. earshot's own check calls that "our reading, an
  assumption"; it is the standard. A doc note for earshot, not made here.
- **`--pipe` on v0.1.2's harness exited 0 when its reader hung up** (measured on
  the 840193c build). The fleet's rule is 1, never 141, and it is 1 now.

### Design decisions, made rather than asked

- **Dark on no device.** With no device chosen the VFD shows nothing and the
  lamp is amber. Inventing thirds from 64 unmeasured bins is what Bin Law
  exists to avoid.
- **Appended, not grouped.** The six controls sit after the About block, ids
  37–42, so a v0.1.2 composition meets exactly the ids it knew.
- **Peak Hold and Hold Decay are reused**, shown on the VFD and the bargraph,
  with the same defaults. No source gave a VFD hold time.
- **Shutter is one control** and the readout is fixed at pitch's 16 ms: without
  a camera the scan is invisible and Multiplex Rate would be dead.
- **The geometry is a design**, not a product's drawing: glass 100 × 30 mm,
  field 93 × 24 mm, segments 2.2 × 1.5 mm, wires across rows 1, 4, 7 and 10.

## Open questions

- **What Resolume actually puts in those 64 bins.** Magnitude or power, what
  normalisation, what window, and whether they really are linear to Nyquist.
  `Bin Law` and `Sensitivity` are the hedge. Measuring it once, in any fleet
  repo, would settle it for all of them.
- **Whether the default Reference Level is the useful one.** −18 dBFS is EBU
  R68's alignment level and it is a guess about a host whose calibration is
  unknown. needle#1 is one data point — the old law pinned until the reference
  was 18 dB higher — and the RMS law answers it, but an hour in Arena with a
  known tone and a log of the bins would replace the guess with a number. The
  Windows bench has no sound device, so it has to be a Mac or a box with one.
- **Whether 54° of sweep and a 1.55:1 window read as a VU meter to somebody who
  owns one.** Nobody has seen this on a wall.
- **Whether four instruments in one plugin is right**, or whether the bargraph
  and the eye want to be their own sources with their own controls. As it stands
  a third of the parameters do nothing on any given Type.
- **The VFD, before it ships** (each is Allan's call):
  - dark with an amber lamp when no device is chosen: enough, or should it say
    more?
  - its six controls sit below About because of the append rule; Controls.h
    says ids may move. Keep, or move them up in a release?
  - `ATTRIBUTIONS.md` and `StoatworksAbout.h` are generated and do not yet name
    earshot or miniaudio: stoatworks-backend's sync has to add them;
  - earshot has never been compiled by MSVC, so the public Windows CI job would
    be its first, and `ci.yml` runs none of the VFD checks;
  - pushing this branch publishes the earshot snapshot in a public repo;
  - whether Resolume honours `FF_EVENT_FLAG_ELEMENTS` (Rescan refreshing the
    menu), and what the macOS microphone prompt looks like inside Resolume;
  - `ToOption`'s normalised path (a host sending 0..1 for an option) maps Type
    differently with five elements than four. Resolume sends element values,
    which are unchanged; a host that sends normalised values would see Type
    shift.

## The browser demo

`demo/` is the page at https://needle-demo.stoatworks-labs.com, built on the
shared kit in `stoatworks-backend/resolume-demo/` (vendored into `demo/vendor/`;
never edit it here). It is two halves, and they are not equally faithful.

**The shader is the plugin's.** `VERTEX_SHADER`, `FRAGMENT_SHADER_A` and
`FRAGMENT_SHADER_B` in `demo/plugin.js` are the three raw strings in
`source/Shaders.cpp`, unedited, and the page joins A + B exactly as the C++
compiler joins adjacent literals. `demo/tools/check_shaders.py` compares each
raw string, and the joined fragment text, character for character, fails if
`kFragmentShader` stops being exactly two raw strings, and rejects any backslash
but the one escaped backtick. `tools/verify.sh` runs it as "demo shaders". One
program, one triangle from `gl_VertexID`, the R8 font texture — the same single
pass as `Renderer::Draw`.

**Everything on the CPU is a port that only a reader checks.** Standards (the
closed-form ζ, ωn by bisection, the PPM taus), Movement (RK4 with dry friction,
the follower, the hold, the eye), Engine (4800 Hz, whole steps, frame-one
priming, the 0.25 s clamp, Resolve, the scale maps, the LM3915 law, every channel
integrated whatever Count says), `Audio::LevelFromSpectrum`, every `Controls.h`
conversion (rounded through `Math.fround`), `ToOption`, `BuildFrame`, Render's
uniform packing and the Font table. `ndtest` has never seen any of it.

Decisions made rather than asked:

- **No audio, and no microphone.** The plugin's one input is Resolume's 64-bin
  FFT buffer. The page drives it from a generated **test signal** chosen in the
  kit's `demo.variants` dropdown, labelled "Test signal": a 0 VU step (−18 dBFS,
  2 s on / 2 s off — the default, because it shows the rise, the overshoot and
  the fall), a −20 dB step, a steady alignment tone, 5 ms bursts, a ladder of 5 /
  20 / 50 / 150 / 500 ms bursts, steady pink noise, a pink-noise ramp from −60 to
  −12 dBFS, and silence. Each frame the page integrates the envelope's power
  over that frame's own interval (one spectrum per frame is the plugin's input
  resolution too, so a 5 ms burst arrives averaged, as it would in Resolume) and
  writes it into 64 bins **as magnitudes**: a tone into bin 2, pink noise as 1/f
  with an exponential per-bin scatter. The plugin's level law then runs on them.
- **It is not the host's FFT, and every surface says so** — the banner (`blurb`),
  the disclosure (`differences`), a note under the transport, and the hints on
  Reference Level, Sensitivity and Bin Law. Because the page writes magnitudes,
  Bin Law on Magnitude reads each test level exactly and Power shows the other
  assumption on the same numbers. A test level in dBFS means what the plugin's
  level law reads: RMS, 1.0 = full scale.
- **Absent from the panel:** the `Audio` buffer parameter (a host writes it; the
  test signal stands in) and the About block (a web page has its own links). The
  other 31 parameters are all present with the constructor's names, groups,
  types, element lists and defaults, in `Controls.h` order. The colour triples
  keep the plugin's names, so the kit's swatch reads "Face Red" etc. Needle has
  no FF_TYPE_INTEGER controls, so galvo's dropdown workaround was not needed.
- **No Clip picker and no "Use my own…"** — a source has zero inputs, so both are
  removed from the DOM after mounting (astable's precedent).
- **The plugin's `Clock` is not ported**: the kit's clock is already seconds.
  Restart sends time backwards, which the engine treats as a loop point and
  advances nothing, so the meters carry on from where they were — the plugin's
  behaviour, kept rather than papered over with a reset.
- A readout line under the picture shows, each frame, the level the plugin's law
  read from the page's spectrum, the input on the meter's scale, the PPM
  detector and the engine steps taken — so a visitor can see what went in.

After needle#1 the page carries the RMS law (and writes each test spectrum with
64 times the power, so a test level still reads exactly), `DriveMovement`, the
VU's stops and Standard at the top of Ballistics. The kit's panel cannot hide a
control, so the page shows all 31 and the hints say which act. The port was run
once in node on 2026-10-11 against the same three claims (−18 dB bins read
−18 dB; Free at the defaults overshoots 1.2500 %; +24 VU never passes 1.035) —
a one-off, not a check.

Gaps: no measurement of any kind on the page (the harness is the evidence); GLSL
ES 3.00 in WebGL2, not GL 4.1 core; JS doubles where the C++ has floats in the
geometry; the kit's banner closes with "a native FFGL effect", which is the
kit's fixed wording and slightly wrong for a source.

Verified 2026-09-24 headlessly (Chrome + SwiftShader via cdpshot's CDP client):
no console errors, no `.stage__status.is-error`, the VU reading 0 on the 0 VU
step, and Type VU → PPM changing the canvas while paused; the bargraph (with its
hold bar) and the magic eye rendered as a stereo pair on the ramp.

## Shape of the code

    source/meter/Standards.*  the quoted figures, and the constants solved from
                              them. No state, no GL, no host.
    source/meter/Movement.*   the four instruments as differential equations.
    source/meter/Engine.*     those instruments integrated at 4800 Hz, the scale
                              mappings, and frame-one priming.
    source/Audio.*            the host's spectrum, and what is assumed about it.
    source/Controls.h         0..1 host parameters to physical units.
    source/Shaders.cpp        coverage, from geometry the CPU already decided.
    source/Render.*           one triangle into the host's framebuffer.
    source/Needle.*           the plugin: parameters, layout, the frame path.
    source/Diag.*             a log file, for the meter that never moves.
    source/Vfd.*              the VFD: vfd::Input (the only caller of earshot),
                              vfd::Display (columns, segments, the hold),
                              vfd::Picture (pixels, on the CPU).
    source/VfdShaders.cpp     the VFD's own fragment shader.
    source/VfdRender.*        its program, compiled on the first VFD frame.
    external/earshot/         an unpatched earshot snapshot (SNAPSHOT).
    source/{Clock,Font}.*     carried from graticule.
    tools/ndtest/             the offline harness (Vfd.inc: the VFD's checks).
    tools/mutate.sh           one-character mutants of the VFD shader.
    tools/sweep.py            no control is silently dead.
    tools/verify.sh           all of it.
    demo/                     the browser demo: plugin.js (shaders verbatim,
                              the CPU half ported), tools/check_shaders.py.
