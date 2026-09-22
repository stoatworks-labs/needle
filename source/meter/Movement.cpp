#include "meter/Movement.h"

#include <algorithm>
#include <cmath>

namespace needle
{
namespace
{
/// One evaluation of the movement's derivative.
struct Deriv
{
	double dx, dv;
};

inline Deriv Field( double x, double v, double u, double zeta, double omegaN )
{
	return { v, omegaN * omegaN * ( u - x ) - 2.0 * zeta * omegaN * v };
}

/// A one-pole coefficient, exact for a constant target over `h` seconds. A
/// non-positive `h` returns 0 -- see the note on Follower in the header.
inline double Pole( double h, double tau )
{
	if( h <= 0.0 || tau <= 0.0 )
		return h > 0.0 ? 1.0 : 0.0;
	return 1.0 - std::exp( -h / tau );
}
} // namespace

void Movement::Step( double h, double u, double zeta, double omegaN, double friction )
{
	if( h <= 0.0 )
		return;

	const Deriv k1 = Field( x, v, u, zeta, omegaN );
	const Deriv k2 = Field( x + 0.5 * h * k1.dx, v + 0.5 * h * k1.dv, u, zeta, omegaN );
	const Deriv k3 = Field( x + 0.5 * h * k2.dx, v + 0.5 * h * k2.dv, u, zeta, omegaN );
	const Deriv k4 = Field( x + h * k3.dx, v + h * k3.dv, u, zeta, omegaN );

	x += h / 6.0 * ( k1.dx + 2.0 * k2.dx + 2.0 * k3.dx + k4.dx );
	v += h / 6.0 * ( k1.dv + 2.0 * k2.dv + 2.0 * k3.dv + k4.dv );

	if( friction <= 0.0 )
		return;

	// Dry friction, semi-implicitly. The decrement this step could take is
	// friction.h; if the pointer is going slower than that it would reverse,
	// which a friction cannot make it do -- so it either stops dead, or the
	// spring is strong enough to drag it on through.
	const double decrement = friction * h;
	if( std::fabs( v ) <= decrement )
	{
		if( std::fabs( omegaN * omegaN * ( u - x ) ) <= friction )
			v = 0.0;// stuck, short of the target, and staying there
		else
			v -= std::copysign( decrement, v );
	}
	else
	{
		v -= std::copysign( decrement, v );
	}
}

void Follower::Step( double h, double u, double riseTau, double fallTau )
{
	if( h <= 0.0 )
		return;
	x += ( u - x ) * Pole( h, u >= x ? riseTau : fallTau );
}

void Hold::Step( double h, double db_, double holdSeconds, double decayDbPerSecond )
{
	if( h <= 0.0 )
		return;

	if( db_ >= db )
	{
		db    = db_;
		since = 0.0;
		return;
	}

	since += h;
	if( since > holdSeconds )
		db = std::max( db_, db - decayDbPerSecond * h );
}

void Eye::Step( double h, double u, double zeta, double omegaN, double persistenceTau,
				double warmTau, double friction )
{
	if( h <= 0.0 )
		return;

	movement.Step( h, u, zeta, omegaN, friction );

	// Phosphor persistence is a lag on what is SEEN, not on what the tube is
	// doing: the target has already been excited, the glow is what is left of
	// it. Symmetric, unlike every other filter in this plugin, because a
	// phosphor decays the same way whichever direction the beam moved.
	const double c = ( persistenceTau > 0.0 ) ? ( 1.0 - std::exp( -h / persistenceTau ) ) : 1.0;
	shown += ( movement.x - shown ) * c;

	// The heater. One pole, from cold, and it never resets while the plugin
	// instance lives -- a tube does not cool down between clips.
	warm += ( 1.0 - warm ) * ( 1.0 - std::exp( -h / std::max( 1e-6, warmTau ) ) );
}

double Eye::ShadowDegrees() const
{
	const double d = std::clamp( shown, 0.0, 1.0 );
	return standards::kEyeOpenDegrees * ( 1.0 - d );
}

double Eye::OverlapDegrees() const
{
	// Past full deflection the wings cross. Capped at the open angle, because
	// beyond that the sectors have swept through each other and there is
	// nothing more to see.
	const double over = shown - 1.0;
	if( over <= 0.0 )
		return 0.0;
	return std::min( standards::kEyeOpenDegrees, standards::kEyeOpenDegrees * over );
}

} // namespace needle
