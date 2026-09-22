# Working on needle

An FFGL **source** plugin (`ND01`) that draws four audio meters — a VU, a peak
programme meter, an LED bargraph and a magic eye — each moving the way its
standard says it must.

`CLAUDE.md` is the command reference; this file is the *why*.

## Status

Built 2026-09-22. Every check in `tools/verify.sh` passes on macOS (Apple
Silicon, macOS 26.4, Metal-backed GL 4.1): the ballistics against ANSI C16.5,
the fall-back against IEC 60268-10 type II, the LM3915 ladder recovered by
bisection, the magic eye's shadow, frame-one priming, frame-rate independence,
the dead band a worn pivot leaves, the defaults, the names, the font, the pixel checks at two rasters, the
dead-control sweep, registration, lipo, plist, ad-hoc codesign and oxbow.

**It has never been loaded into Resolume, and it has not been installed into
Arena.** Nothing has driven the host. The Windows build is CI-only and the CI
has never run. There is no release tag, no website registration, no factory
presets, no OpenFX port, no browser demo and no user guide.

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

- **The level is the square root of the sum over ALL 64 bins.** The spec warns
  that the fleet assumes those bins are linear 0→Nyquist and nobody has measured
  it. That assumption is not made here and does not need to be: by Parseval, a
  sum over every bin is the signal's power however the bins are laid out in
  frequency. Reorder them, warp them, space them logarithmically — the answer is
  the same. Only something that weighted bins against one another would care,
  and there is nothing like that in this plugin.
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
size?*

**The structural answer, first.** Nine of the eleven check groups open **no GL
context at all** — `--ballistics`, `--ppm`, `--steps`, `--eye`, `--prime`,
`--rate`, `--friction`, `--defaults`, `--names` and `--font` drive `source/meter/` and the
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
| Free agrees with Standard at the defaults | 1e-4 relative | The controls are floats (~7 significant digits) and the rise map amplifies relative error by `ln 100 = 4.6`, so the floor is ~5e-7. 1e-4 is two hundred times that and still far tighter than the gap between any two values anybody would confuse. |
| Reference, trim, hold time, hold decay | 1e-4 / 1e-3 absolute | Exact linear maps of exactly-representable constants; the tolerance is a float-rounding allowance. |
| Names, duplicates, blank and duplicate glyphs | counts | No tolerance. |

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
- **Asserting that two floating-point clock origins agree bit for bit.** See
  `--prime`, above.
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
  pretending the result is still a VU.
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
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies**, with
  `guide=""` because no user guide exists — the arrangement graticule shipped
  with. Register needle in `stoatworks-website`'s `projects.json`,
  `stoatworks-backend`'s `sync-about.py` TARGETS and `attributions/names.json`
  and re-run the syncs before the first release. The About header's facts were
  chosen so the button count — and therefore the parameter count — does not
  change when it is regenerated; if the sync adds a guide link, the
  `static_assert` in `Needle.cpp` will say so.

## Open questions

- **What Resolume actually puts in those 64 bins.** Magnitude or power, what
  normalisation, what window, and whether they really are linear to Nyquist.
  `Bin Law` and `Sensitivity` are the hedge. Measuring it once, in any fleet
  repo, would settle it for all of them.
- **Whether the default Reference Level is the useful one.** −18 dBFS is EBU
  R68's alignment level and it is a guess about a host whose calibration is
  unknown. An hour in Arena would replace the guess with a number.
- **Whether 54° of sweep and a 1.55:1 window read as a VU meter to somebody who
  owns one.** Nobody has seen this on a wall.
- **Whether four instruments in one plugin is right**, or whether the bargraph
  and the eye want to be their own sources with their own controls. As it stands
  a third of the parameters do nothing on any given Type.

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
    source/{Clock,Font}.*     carried from graticule.
    tools/ndtest/             the offline harness.
    tools/sweep.py            no control is silently dead.
    tools/verify.sh           all of it.
