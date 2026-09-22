#include "meter/Standards.h"

#include <cmath>

namespace needle::standards
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Enough bisection steps that the answer is at the limit of a double: the
/// bracket below spans 64 units of normalised time, and 2^-80 of that is far
/// under the 2^-52 a double can represent near 4. It costs eighty evaluations
/// of a sine, once per process.
constexpr int kBisectionSteps = 80;
} // namespace

double DampingForOvershoot( double overshoot )
{
	// Mp = exp( -pi.z / sqrt(1 - z^2) )  =>  z = -ln(Mp) / sqrt( pi^2 + ln(Mp)^2 )
	//
	// The degenerate ends are real inputs: Free mode hands this the Overshoot
	// control, and an operator will put it at both stops. No overshoot at all is
	// critical damping; an overshoot of 1 or more is a system that never
	// settles, which is not a movement.
	if( overshoot <= 0.0 )
		return 1.0;
	if( overshoot >= 0.99 )
		overshoot = 0.99;

	const double l = std::log( overshoot );
	return -l / std::sqrt( kPi * kPi + l * l );
}

double StepResponse( double t, double zeta, double omegaN )
{
	if( t <= 0.0 )
		return 0.0;

	if( zeta < 1.0 - 1e-9 )
	{
		const double wd  = omegaN * std::sqrt( 1.0 - zeta * zeta );
		const double phi = std::acos( zeta );
		return 1.0 - std::exp( -zeta * omegaN * t ) / std::sqrt( 1.0 - zeta * zeta ) *
						 std::sin( wd * t + phi );
	}
	if( zeta > 1.0 + 1e-9 )
	{
		// Overdamped: two real poles. Free mode can reach here.
		const double r  = std::sqrt( zeta * zeta - 1.0 );
		const double s1 = -omegaN * ( zeta - r );
		const double s2 = -omegaN * ( zeta + r );
		return 1.0 - ( s1 * std::exp( s2 * t ) - s2 * std::exp( s1 * t ) ) / ( s1 - s2 );
	}
	// Critically damped.
	const double x = omegaN * t;
	return 1.0 - ( 1.0 + x ) * std::exp( -x );
}

double NormalisedTimeToReach( double fraction, double zeta )
{
	// Bisect for the FIRST crossing, which is why the bracket is grown from
	// zero rather than opened wide: an underdamped response crosses `fraction`
	// on the way up, again coming down off the overshoot, and again on the way
	// back up. A wide initial bracket would find whichever of those the
	// midpoint happened to land between, and the answer would be silently the
	// wrong root -- larger than the truth, and still perfectly self-consistent.
	double hi = 0.5;
	while( StepResponse( hi, zeta, 1.0 ) < fraction && hi < 64.0 )
		hi *= 2.0;

	double lo = 0.0;
	for( int i = 0; i < kBisectionSteps; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( StepResponse( mid, zeta, 1.0 ) < fraction )
			lo = mid;
		else
			hi = mid;
	}
	return 0.5 * ( lo + hi );
}

double NaturalFrequencyFor( double fraction, double seconds, double zeta )
{
	// The step response is a function of (omegaN . t) alone, so solving it once
	// at omegaN = 1 and dividing is exact rather than an approximation.
	return NormalisedTimeToReach( fraction, zeta ) / seconds;
}

double VuDamping()
{
	static const double z = DampingForOvershoot( kVuOvershoot );
	return z;
}

double VuNaturalFrequency()
{
	static const double wn = NaturalFrequencyFor( kVu99, kVuRiseTime, VuDamping() );
	return wn;
}

double PpmFallTau()
{
	// 20.log10( e^(-t/T) ) = -20.t / (T.ln 10), so falling kPpmFallDb in
	// kPpmFallSeconds fixes T outright.
	return kPpmFallSeconds / ( kPpmFallDb / 20.0 * std::log( 10.0 ) );
}

double PpmRiseTau()
{
	// 1 - e^(-tb/T) = 10^(-down/20)
	const double target = std::pow( 10.0, -kPpmBurstDownDb / 20.0 );
	return -kPpmBurstSeconds / std::log( 1.0 - target );
}

} // namespace needle::standards
