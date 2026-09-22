#pragma once

/**
	The numbers the standards actually specify, and the constants they imply.

	**Nothing in this file is tuned.** Every constant below is either quoted from
	a published specification or *solved* from one, and the solve is done here,
	in code, so that changing the quoted figure changes the behaviour. A
	ballistic constant that was arrived at by dragging a slider until it looked
	right is a ballistic constant that will be dragged again; this is the file
	that makes that impossible.

	## VU -- ANSI C16.5 (and IEC 60268-17)

	Two sentences of specification:

	  * applying a signal that produces a steady 0 VU reading, the pointer shall
	    reach **99 % of that steady deflection in 300 ms**;
	  * and shall **overshoot by between 1.0 % and 1.5 %**.

	A VU meter is a d'Arsonval movement: a mass on a spring with viscous damping,
	which is a second-order low-pass. Its unit step response is

	    y(t) = 1 - e^(-z.wn.t) / sqrt(1 - z^2) . sin( wd.t + acos(z) ),
	    wd = wn.sqrt(1 - z^2)

	and it has exactly two free parameters. So the two sentences above do not
	leave room for taste: they determine `z` and `wn` and there is nothing left
	to choose.

	`z` comes from the overshoot in closed form -- the first peak of the response
	above is `1 + exp(-pi.z/sqrt(1-z^2))`, so

	    z = -ln(Mp) / sqrt( pi^2 + ln(Mp)^2 )

	and `wn` comes from the 300 ms figure, which has **no** closed form and is
	found by bisection on the equation above. The 1.0 %-1.5 % band is quoted as a
	band, so the design point is its midpoint, 1.25 %, which leaves half a
	percentage point of margin at each end of the tolerance the standard allows.

	### Do not use the textbook settling time

	The familiar `ts ~= 4 / (z.wn)` is the time for the *exponential envelope* to
	enter a 2 % band. It is not the time the response reaches any particular
	value, it ignores that the response re-enters and leaves the band, and here
	it is wrong by a fifth: with the constants below it predicts 364 ms where the
	true first crossing of 99 % is 300 ms by construction. Everything in this
	file and in `ndtest --ballistics` uses the exact expression.

	## PPM -- IEC 60268-10 type II (the BBC PPM)

	  * **fall-back: 20 dB in 2.8 s**;
	  * **integration: a 5 ms tone burst reads 2 dB below the same tone steady.**

	Both are one-pole time constants, and the first of the two is prettier than
	it looks. A one-pole decay in *amplitude* is `e^(-t/T)`, whose value in dB is
	`-20.t / (T.ln 10)` -- a straight line in dB against time. So "linear in dB"
	and "exponential in amplitude" are the same statement, and the fall-back
	figure gives the time constant directly:

	    T_fall = 2.8 / ln(10) = 1.2160 s

	The rise is the same algebra the other way round. 2 dB below steady is
	`10^(-2/20) = 79.43 %` of steady -- which is where the usual informal "a
	5 ms burst reaches about 80 %" comes from; they are the same requirement --
	so `1 - e^(-0.005/T) = 0.7943` and

	    T_rise = 0.005 / -ln(1 - 10^(-0.1)) = 3.161 ms

	## LED bargraph -- the LM3915 law

	**3 dB per step, ten steps.** That is the LM3915's divider chain and it is
	the only thing about a bargraph that is pinned by anything; hold time and
	hold decay are not specified by anybody and are controls here, which
	`AGENTS.md` says out loud.

	## Magic eye -- 6U5 / EM84

	Nothing about a magic eye is standardised. The one number that is a *fact*
	rather than a choice is the 6U5's shadow angle with no signal on the grid,
	which its data sheet gives as 100 degrees; the rest of the model -- how the
	angle maps to level, the heater warm-up, the phosphor persistence -- is
	stated as a choice in `AGENTS.md` and is not pretended to be a standard.
*/
namespace needle::standards
{

// ---------------------------------------------------------------------------
// The quoted figures. Everything else in this file is derived from these.
// ---------------------------------------------------------------------------

/// ANSI C16.5: the fraction of the steady deflection reached at kVuRiseTime.
inline constexpr double kVu99          = 0.99;
/// ANSI C16.5: ...and the time it is reached in, in seconds.
inline constexpr double kVuRiseTime    = 0.300;
/// ANSI C16.5: the permitted overshoot band, as a fraction of steady.
inline constexpr double kVuOvershootLo = 0.010;
inline constexpr double kVuOvershootHi = 0.015;
/// The design point: the middle of the band.
inline constexpr double kVuOvershoot   = 0.5 * ( kVuOvershootLo + kVuOvershootHi );

/// IEC 60268-10 type II: decibels of fall-back, and the time to fall them.
inline constexpr double kPpmFallDb      = 20.0;
inline constexpr double kPpmFallSeconds = 2.8;
/// IEC 60268-10 type II: the burst length, and how far below steady it reads.
inline constexpr double kPpmBurstSeconds = 0.005;
inline constexpr double kPpmBurstDownDb  = 2.0;

/// LM3915: decibels per step, and the number of steps.
inline constexpr double kBargraphStepDb = 3.0;
inline constexpr int    kBargraphSteps  = 10;

/// 6U5 data sheet: the shadow angle with no signal, in degrees.
inline constexpr double kEyeOpenDegrees = 100.0;

// ---------------------------------------------------------------------------
// The solved constants.
// ---------------------------------------------------------------------------

/// The damping ratio of a second-order system whose step response overshoots by
/// `overshoot` (a fraction, so 0.0125 for 1.25 %). Closed form.
double DampingForOvershoot( double overshoot );

/// The unit step response of a second-order system at time `t`. Exact -- this
/// is the expression everything else in the repo is measured against.
double StepResponse( double t, double zeta, double omegaN );

/// The first time `StepResponse` reaches `fraction`, in units of 1/omegaN.
/// Found by bisection on the exact expression; there is no closed form.
double NormalisedTimeToReach( double fraction, double zeta );

/// The natural frequency, in rad/s, of a movement with damping `zeta` that
/// first reaches `fraction` of its steady deflection at `seconds`.
double NaturalFrequencyFor( double fraction, double seconds, double zeta );

/// Damping ratio of the VU movement ANSI C16.5 describes.
double VuDamping();
/// Natural frequency, rad/s, of the VU movement ANSI C16.5 describes.
double VuNaturalFrequency();

/// The PPM's fall time constant, in seconds, from the 20 dB / 2.8 s figure.
double PpmFallTau();
/// The PPM's rise time constant, in seconds, from the 5 ms / -2 dB figure.
double PpmRiseTau();

} // namespace needle::standards
