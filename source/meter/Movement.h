#pragma once

#include "meter/Standards.h"

namespace needle
{

/// The four instruments, in the order the dropdown shows them.
enum class MeterType : int
{
	Vu = 0,
	Ppm,
	Bargraph,
	Eye,
	Count
};
constexpr int kMeterTypeCount = static_cast< int >( MeterType::Count );

/**
	A pointer on a spring: the second-order movement a VU meter is.

	`Step` is fourth-order Runge-Kutta on

	    x' = v
	    v' = wn^2 ( u - x ) - 2.z.wn.v

	with the drive `u` held constant across the step, which is what an
	oversampled engine gives it. RK4 is used rather than the exact matrix
	exponential for one reason that matters and one that does not: it is
	honest about being an integrator, so `ndtest --ballistics` comparing it
	against the closed form in `Standards.h` is a real test of the engine
	rather than a tautology; and it costs four evaluations of two lines of
	arithmetic, which is nothing.

	At the engine rate in `Engine.h` the local error is of order `(wn.h)^5/120`,
	which for the VU's `wn` of 13.5 rad/s and `h` of 1/4800 s is about 4e-15
	per step. That is the number `--ballistics` checks against, and it is why
	its tolerance is 1e-9 rather than something chosen by eye.
*/
struct Movement
{
	double x = 0.0;///< deflection, in scale units where 1.0 is full scale
	double v = 0.0;///< its rate of change, per second

	void Reset()
	{
		x = 0.0;
		v = 0.0;
	}

	/// Advance by `h` seconds toward drive `u`.
	///
	/// `friction` is a **dry** friction, in deflection units per second
	/// squared, opposing whichever way the pointer is going. It is what a worn
	/// pivot does, and it behaves nothing like more damping: viscous damping
	/// slows a movement down and still lets it arrive, while dry friction stops
	/// it *short*, anywhere inside a dead band of `friction / omegaN^2` either
	/// side of the target, and leaves it there. A worn meter therefore reads
	/// differently depending on which direction it came from, which is exactly
	/// the complaint people have about one.
	///
	/// It is applied after the Runge-Kutta step rather than inside the
	/// derivative, because `sign( v )` is discontinuous and an RK4 stage that
	/// straddles the discontinuity is meaningless. The pointer sticks when the
	/// step's own velocity decrement would reverse it AND the spring cannot
	/// break it out.
	///
	/// A friction of zero leaves the step bit-identical to the frictionless
	/// one, which `ndtest --friction` asserts: the headline ANSI C16.5 claim is
	/// made at Wear 0 and must not be reachable from here.
	void Step( double h, double u, double zeta, double omegaN, double friction = 0.0 );
};

/**
	A quasi-peak follower: one pole up, a slower one down.

	The step is the *exact* solution of a one-pole with a constant target over
	the interval -- `x += (u - x)(1 - e^(-h/T))` -- so unlike `Movement` this
	carries no integration error at all, and `ndtest --ppm` measuring a 20 dB
	fall out of it is measuring the time constant and nothing else.

	It runs on **amplitude**, not on a scale position. A one-pole decay in
	amplitude is a straight line in dB, which is what a PPM's fall-back figure
	describes, and keeping the detector in amplitude means the fall rate cannot
	be changed by a decision about where the scale ends.

	A `h` of zero advances nothing. It must not snap: on the first frame of a
	clip the host's clock has not moved yet, and a follower written as
	`coefficient = h > 0 ? 1 - exp(-h/T) : 1` -- which is how the fleet has
	written it before -- pins the meter on its very first frame and then decays
	from a reading that never happened.
*/
struct Follower
{
	double x = 0.0;

	void Reset() { x = 0.0; }
	void Step( double h, double u, double riseTau, double fallTau );
};

/**
	The hold bar over a bargraph: catch the maximum, hold it, then let it down.

	Nothing specifies either number. A hold time somewhere between one and two
	seconds and a fall of about 10-20 dB/s is what hardware meters do, and both
	are controls here rather than constants, because calling them a standard
	would be a lie by association with the three that are.
*/
struct Hold
{
	double db    = -120.0;///< the held level, in dB on the meter's own scale
	double since = 0.0;   ///< seconds since it was last caught

	void Reset()
	{
		db    = -120.0;
		since = 0.0;
	}

	void Step( double h, double db_, double holdSeconds, double decayDbPerSecond );
};

/**
	The magic eye.

	A 6U5 is a triode driving a fluorescent target. The triode's plate sits
	beside the cathode ray's path and its potential deflects the electrons, so
	a *shadow* is cast over a sector of the target whose angle follows the grid
	voltage: wide open with no signal, closing as the signal grows, and past the
	point where it closes the two edges of the lit sector overlap and the shadow
	sector reverses into a bright overlap. That overlap is what "the eye closes"
	means to anyone who has used one, and it is modelled here rather than
	clamped away.

	Three things are the tube and not the signal:

	  * **Warm-up.** The heater takes seconds to bring the cathode up, so the
	    target brightens from nothing over the first few seconds of a session.
	  * **Persistence.** The phosphor keeps glowing after the beam has moved, so
	    the shadow's edge smears when the level moves fast.
	  * **Wear.** An old tube's target is dim, and unevenly so where it was
	    driven hardest.

	Only the first of those has a number anyone published -- the 100 degree
	shadow angle at zero signal, from the data sheet. The rest are choices, and
	`AGENTS.md` says which.
*/
struct Eye
{
	Movement movement;  ///< the level the target is following
	double   shown = 0.0;///< after phosphor persistence
	double   warm  = 0.0;///< 0 cold, 1 at temperature

	void Reset()
	{
		movement.Reset();
		shown = 0.0;
		warm  = 0.0;
	}

	void Step( double h, double u, double zeta, double omegaN, double persistenceTau,
			   double warmTau, double friction = 0.0 );

	/// Shadow half-sector angle in degrees: `kEyeOpenDegrees` at rest, zero at
	/// and beyond full deflection.
	double ShadowDegrees() const;
	/// How far the two wings overlap past closure, in degrees. Zero until the
	/// shadow has shut.
	double OverlapDegrees() const;
};

} // namespace needle
