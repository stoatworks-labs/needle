#include "meter/Engine.h"

#include <algorithm>
#include <cmath>

namespace needle
{
namespace
{
/// The floor the level is measured from. -140 dB is below anything a 32-bit
/// float spectrum can carry and keeps `log10` away from zero without putting a
/// visible knee anywhere on any scale.
constexpr double kSilence = 1e-7;

double Db( double amplitude )
{
	return 20.0 * std::log10( std::max( amplitude, kSilence ) );
}

double Amp( double db )
{
	return std::pow( 10.0, db / 20.0 );
}

/// The heater in a 6U5. Not a standard, and not a control: a tube warms up in
/// the seconds after power and then stays warm, and an operator has no reason
/// to want that adjustable.
constexpr double kWarmTau = 4.0;
} // namespace

Scale ScaleFor( MeterType type )
{
	switch( type )
	{
	case MeterType::Vu:
		// A VU scale runs -20 to +3 and is linear in VOLTAGE, which is why 0 VU
		// sits at 1/10^(3/20) = 70.8 % of the arc rather than in the middle.
		return { true, -20.0, 3.0 };

	case MeterType::Ppm:
		// A BBC PPM has seven evenly spaced marks 4 dB apart and is linear in
		// dB. Mark 4 is the alignment level, so the meter's own 0 lands exactly
		// half way along the scale.
		return { false, -12.0, 12.0 };

	case MeterType::Bargraph:
		// Ten LM3915 steps of 3 dB. The top segment lights at 0 dB, the bottom
		// at -27, and the scale bottoms out one step below that so an unlit
		// meter is off rather than sitting on its first threshold.
		return { false, -standards::kBargraphStepDb * standards::kBargraphSteps, 0.0 };

	case MeterType::Eye:
	default:
		// Nothing specifies this. 40 dB is the range over which the shadow is
		// worth watching, and 0 dB -- the meter's own reference -- is where it
		// shuts: the "stated overload" of `ndtest --eye`.
		return { false, -40.0, 0.0 };
	}
}

double DeflectionFor( const Scale& scale, double amplitudeRatio )
{
	if( scale.linearInAmplitude )
		return amplitudeRatio / Amp( scale.topDb );

	const double db = Db( amplitudeRatio );
	return ( db - scale.bottomDb ) / ( scale.topDb - scale.bottomDb );
}

double AmplitudeForDeflection( const Scale& scale, double d )
{
	if( scale.linearInAmplitude )
		return d * Amp( scale.topDb );
	return Amp( scale.bottomDb + d * ( scale.topDb - scale.bottomDb ) );
}

Resolved Resolve( const Settings& s )
{
	Resolved r;

	if( s.standard )
	{
		// Locked to the specification. The three ballistic controls are not
		// read at all here, which is the whole meaning of the switch: on
		// Standard they are out of the model, not merely overridden by it.
		r.vuZeta      = standards::VuDamping();
		r.vuOmegaUp   = standards::VuNaturalFrequency();
		r.vuOmegaDown = r.vuOmegaUp;// a real movement has ONE natural frequency

		r.ppmRiseTau = standards::PpmRiseTau();
		r.ppmFallTau = standards::PpmFallTau();

		// The eye is the one instrument with no standard behind it. Giving it
		// the VU's movement is a choice and not a quotation: a magic eye in a
		// receiver sat on an AVC line whose time constants were the set
		// designer's, and the VU's are at least a published pair of numbers
		// that produce a believable needle-speed.
		r.eyeZeta      = r.vuZeta;
		r.eyeOmegaUp   = r.vuOmegaUp;
		r.eyeOmegaDown = r.vuOmegaUp;
	}
	else
	{
		// Free. Deliberately not a movement: a rise that differs from a fall
		// is a switched system, and no mass on a spring does that. It is here
		// because an operator wants a meter that looks good on a beat, and
		// saying so is better than pretending the result is still a VU.
		r.vuZeta    = standards::DampingForOvershoot( s.overshoot );
		r.vuOmegaUp = standards::NaturalFrequencyFor( standards::kVu99,
													  std::max( 1e-3, s.riseSeconds ), r.vuZeta );
		r.vuOmegaDown = standards::NaturalFrequencyFor( standards::kVu99,
														std::max( 1e-3, s.fallSeconds ), r.vuZeta );

		// A one-pole's time to 99 % is T.ln(100); a 20 dB fall takes T.ln(10).
		// Both controls are "how long a fall takes", measured the way each
		// instrument's own standard measures it.
		r.ppmRiseTau = std::max( 1e-5, s.riseSeconds ) / std::log( 100.0 );
		r.ppmFallTau = std::max( 1e-4, s.fallSeconds ) / std::log( 10.0 );

		r.eyeZeta      = r.vuZeta;
		r.eyeOmegaUp   = r.vuOmegaUp;
		r.eyeOmegaDown = r.vuOmegaDown;
	}

	// Never standardised, never locked: these stay live on both settings of the
	// switch, and AGENTS.md says why.
	r.holdSeconds          = std::max( 0.0, s.holdSeconds );
	r.holdDecayDbPerSecond = std::max( 0.0, s.holdDecayDbPerSecond );
	r.persistenceTau       = std::max( 0.0, s.persistenceSeconds );
	r.warmTau              = kWarmTau;
	r.frictionDeadBand     = kMaxDeadBand * std::clamp( s.wear, 0.0, 1.0 );

	return r;
}

void Engine::Reset()
{
	for( Models& c : models_ )
	{
		c.vu.Reset();
		c.ppm.Reset();
		c.hold.Reset();
		c.eye.Reset();
		c.state = ChannelState{};
	}
	started_       = false;
	lastHost_      = 0.0;
	residual_      = 0.0;
	engineSeconds_ = 0.0;
	lastSteps_     = 0;
}

void Engine::SetSettings( const Settings& settings )
{
	settings_ = settings;
	resolved_ = Resolve( settings );
}

const ChannelState& Engine::Channel( int index ) const
{
	static const ChannelState none;
	if( index < 0 || index >= kMaxChannels )
		return none;
	return models_[ index ].state;
}

void Engine::StepAll( double h, const double* amplitudeRatio )
{
	const Scale vuScale  = ScaleFor( MeterType::Vu );
	const Scale eyeScale = ScaleFor( MeterType::Eye );

	// Every channel, not the Count the operator has chosen. See the header.
	for( int i = 0; i < kMaxChannels; ++i )
	{
		Models&      c = models_[ i ];
		const double a = amplitudeRatio[ i ];

		// The VU: a movement, driven by the deflection that level would settle
		// at. Its target is linear in amplitude because its scale is.
		{
			const double u     = DeflectionFor( vuScale, a );
			const double omega = ( u >= c.vu.x ) ? resolved_.vuOmegaUp : resolved_.vuOmegaDown;
			// The friction is expressed as a dead band and converted here, so
			// the band is the same fraction of full scale at any ballistics.
			c.vu.Step( h, u, resolved_.vuZeta, omega,
					   resolved_.frictionDeadBand * omega * omega );
		}

		// The PPM and the bargraph share one detector, in the AMPLITUDE domain,
		// and it is a detector rather than a movement: IEC 60268-10 specifies
		// the *indication*, not the mechanism, so all the ballistics live here
		// and the pointer follows without a movement of its own. Wear therefore
		// does not stick a PPM -- there is no pivot in this model to stick.
		//
		// because that is where the standard's fall-back figure lives. The two
		// differ only in the scale they are read against, which is the honest
		// account of the hardware too: an LED meter and a moving-coil PPM off
		// the same rectifier differ in the display and not in the detector.
		c.ppm.Step( h, a, resolved_.ppmRiseTau, resolved_.ppmFallTau );

		// The eye. Its own movement, because it follows a dB scale and the VU
		// follows a voltage one, so one movement cannot serve both.
		{
			const double u     = DeflectionFor( eyeScale, a );
			const double omega = ( u >= c.eye.movement.x ) ? resolved_.eyeOmegaUp
														   : resolved_.eyeOmegaDown;
			c.eye.Step( h, u, resolved_.eyeZeta, omega, resolved_.persistenceTau,
						resolved_.warmTau, resolved_.frictionDeadBand * omega * omega );
		}

		// The hold bar follows the detector, in dB on the bargraph's scale.
		c.hold.Step( h, Db( c.ppm.x ), resolved_.holdSeconds, resolved_.holdDecayDbPerSecond );
	}
}

void Engine::Frame( double hostSeconds, const double* amplitude, int inputs )
{
	inputs = std::clamp( inputs, 1, kMaxChannels );

	// ---- the input, in the meter's own units --------------------------------
	//
	// One number per channel: how loud, relative to the level the operator
	// called zero. Everything downstream is in these units, so the reference
	// and the trim are applied exactly once, here.
	const double offsetDb = -settings_.referenceDbfs + settings_.trimDb;
	double       ratio[ kMaxChannels ] = { 0.0, 0.0 };
	for( int i = 0; i < kMaxChannels; ++i )
		ratio[ i ] = Amp( Db( std::max( 0.0, amplitude[ std::min( i, inputs - 1 ) ] ) ) + offsetDb );

	// ---- the interval -------------------------------------------------------
	double dt = 0.0;
	if( started_ )
	{
		dt = hostSeconds - lastHost_;
		if( dt < 0.0 )
			dt = 0.0;// a loop point, or the operator scrubbing backwards
		if( dt > kMaxFrameSeconds )
			dt = kMaxFrameSeconds;
	}
	else
	{
		// Frame one. Adopt the host's origin; step nothing. See the header.
		started_ = true;
	}
	lastHost_ = hostSeconds;

	// ---- integrate ----------------------------------------------------------
	//
	// Whole steps only, with the remainder carried, so a frame rate that does
	// not divide the engine rate does not drift: over any span of host time the
	// number of steps taken is floor(span . rate) either way.
	const double h = 1.0 / kEngineRate;
	residual_ += dt;
	long steps = static_cast< long >( std::floor( residual_ * kEngineRate ) );
	if( steps < 0 )
		steps = 0;
	const long maxSteps = static_cast< long >( kMaxFrameSeconds * kEngineRate ) + 2;
	if( steps > maxSteps )
		steps = maxSteps;
	residual_ -= static_cast< double >( steps ) * h;
	lastSteps_ = steps;

	for( long s = 0; s < steps; ++s )
	{
		StepAll( h, ratio );
		engineSeconds_ += h;
	}

	// ---- publish ------------------------------------------------------------
	//
	// Every channel, hidden or not: a meter Count is not showing is still
	// running, and what it publishes is what it would show.
	const Scale scale   = ScaleFor( settings_.type );
	const Scale barScale = ScaleFor( MeterType::Bargraph );

	for( int i = 0; i < kMaxChannels; ++i )
	{
		Models&       c = models_[ i ];
		ChannelState& s = c.state;

		s.inputDb = Db( ratio[ i ] );
		s.ppmDb   = Db( c.ppm.x );

		switch( settings_.type )
		{
		case MeterType::Vu: s.deflection = c.vu.x; break;
		case MeterType::Ppm:
		case MeterType::Bargraph: s.deflection = DeflectionFor( scale, c.ppm.x ); break;
		case MeterType::Eye:
		default: s.deflection = c.eye.shown; break;
		}
		s.deflection = std::max( 0.0, s.deflection );

		// A hold time of zero means no hold bar, not a bar that is released
		// instantly: an operator who does not want one has to be able to turn
		// it off, and the pixel check needs a way to stop it covering a probe
		// -- graticule's burn-in plate, wearing a different hat.
		s.holdDb         = c.hold.db;
		s.holdDeflection = resolved_.holdSeconds > 0.0
							   ? std::clamp( ( c.hold.db - barScale.bottomDb ) /
												 ( barScale.topDb - barScale.bottomDb ),
											 0.0, 1.0 )
							   : 0.0;

		// The LM3915 law: step k lights when the level has reached the k-th
		// threshold above the bottom of the run, and the thresholds are the
		// only part of a bargraph anybody specifies.
		s.litSegments = 0;
		for( int k = 0; k < standards::kBargraphSteps; ++k )
		{
			const double threshold =
				barScale.topDb - standards::kBargraphStepDb *
									 static_cast< double >( standards::kBargraphSteps - 1 - k );
			s.segment[ k ] = s.ppmDb >= threshold;
			if( s.segment[ k ] )
				s.litSegments = k + 1;
		}

		s.eyeShadowDeg  = c.eye.ShadowDegrees();
		s.eyeOverlapDeg = c.eye.OverlapDegrees();
		s.eyeWarm       = c.eye.warm;
	}
}

} // namespace needle
