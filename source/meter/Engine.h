#pragma once

#include "meter/Movement.h"

namespace needle
{

/**
	The engine's integration rate, in steps per second.

	Not a taste. Three things set it:

	  * The **fastest** thing modelled is the PPM's rise, `T = 3.161 ms`. At
	    4800 Hz that is fifteen steps, so the shape of a rise is a curve rather
	    than a corner.
	  * The **slowest** is the VU's natural frequency, 2.15 Hz, which gets 2230
	    steps per cycle. RK4 there is exact to about 4e-15 per step, which is
	    what lets `ndtest --ballistics` compare the integrator against the
	    closed form at a tolerance of 1e-9 and mean it.
	  * A host frame is 16.7 ms at best and 41.7 ms at 24 fps. **The standard's
	    own figure is 300 ms**, so a frame is between a twentieth and a seventh
	    of the quantity being specified -- far too coarse to measure the
	    specification against, and coarse enough that the integrator's answer
	    would depend on the host's frame rate. Neither is acceptable for a
	    plugin whose entire claim is a number out of a standard.

	4800 rather than 4000 because it divides 24, 30, 48, 60, 120 and 240 exactly,
	so the common frame rates leave no residual to carry and `ndtest --rate` is
	measuring the model rather than the accumulator.
*/
inline constexpr double kEngineRate = 4800.0;

/// A frame longer than this is a stall, a seek or a scrub, not a frame. Letting
/// it through would advance the engine by a quarter of a second of ballistics
/// in one go -- which for a 300 ms movement is most of the way to the answer,
/// arriving all at once.
inline constexpr double kMaxFrameSeconds = 0.25;

/// Where a meter's scale begins and ends, and what it is linear in.
///
/// **A VU scale is linear in voltage and a PPM scale is linear in decibels**,
/// and that is not a detail: it is why 0 VU sits at 71 % of the arc while a
/// BBC PPM's marks are evenly spaced. Getting this backwards produces a meter
/// that reads correctly at exactly two points.
struct Scale
{
	bool   linearInAmplitude;
	double bottomDb;///< dB, on the meter's own scale, at zero deflection
	double topDb;   ///< dB, on the meter's own scale, at full deflection
};

Scale ScaleFor( MeterType type );

/// Deflection for a level, in scale units where 1.0 is the top of the scale.
/// Not clamped above: a needle can pin, and an eye's wings can overlap.
double DeflectionFor( const Scale& scale, double amplitudeRatio );

/// The inverse, for the harness: the amplitude ratio that deflects to `d`.
double AmplitudeForDeflection( const Scale& scale, double d );

/// What the operator asked for, in physical units.
struct Settings
{
	MeterType type     = MeterType::Vu;
	int       channels = 1;

	double referenceDbfs = -18.0;///< the dBFS that reads 0 on the meter's scale
	double trimDb        = 0.0;  ///< Sensitivity, added to the input

	/// On Standard, every ballistic constant comes from `Standards.h` and the
	/// three controls below are not consulted at all.
	bool standard = true;

	double riseSeconds = 0.300;///< Free: time to 99 % of a rising step
	double fallSeconds = 1.000;///< Free: VU 99 % of a falling step, PPM 20 dB
	double overshoot   = 0.0125;///< Free: fraction, sets the damping

	double holdSeconds          = 1.5; ///< bargraph hold bar: 0 turns it off, and
	                                   ///< nobody specifies either of these
	double holdDecayDbPerSecond = 12.0;
	double persistenceSeconds   = 0.06;///< the eye's phosphor
};

/// The ballistic constants those settings imply. One function, used by the
/// plugin and by the harness, so a check can never be measuring a
/// re-transcription of the model.
struct Resolved
{
	double vuZeta      = 0.0;
	double vuOmegaUp   = 0.0;
	double vuOmegaDown = 0.0;

	double ppmRiseTau = 0.0;
	double ppmFallTau = 0.0;

	double eyeZeta      = 0.0;
	double eyeOmegaUp   = 0.0;
	double eyeOmegaDown = 0.0;

	double holdSeconds          = 0.0;
	double holdDecayDbPerSecond = 0.0;
	double persistenceTau       = 0.0;
	double warmTau              = 0.0;
};

Resolved Resolve( const Settings& settings );

/// Everything one channel's drawing needs. Nothing here is in pixels.
struct ChannelState
{
	double inputDb   = -120.0;///< the measured level on the meter's own scale
	double deflection = 0.0;  ///< the selected meter's pointer, 0..1 (may pin)

	double ppmDb     = -120.0;///< the quasi-peak detector's output, in dB
	double holdDb    = -120.0;
	double holdDeflection = 0.0;

	int  litSegments = 0;   ///< 0..10
	bool segment[ standards::kBargraphSteps ] = {};

	double eyeShadowDeg  = standards::kEyeOpenDegrees;
	double eyeOverlapDeg = 0.0;
	double eyeWarm       = 0.0;
};

/**
	The CPU side of the plugin: four instruments, integrated at `kEngineRate`,
	with no GL anywhere near it.

	That last clause is the point. Every physical claim this plugin makes is a
	claim about this class, so every check of those claims runs here -- with no
	context, no rasteriser and no raster. `ndtest --ballistics`, `--ppm`,
	`--steps`, `--eye`, `--prime` and `--rate` do not open a window.

	## Frame one

	`Frame` takes the host's clock reading, not a delta, and **the first call
	after a `Reset` advances nothing**: it adopts the host's origin and
	returns. Two things go wrong without it, and the fleet has already paid for
	one of them.

	A clip triggered at 40 seconds into a composition hands the plugin a first
	`SetTime` of 40. An engine that treats that as elapsed time steps 192,000
	times on frame one -- every meter settles instantly, the hold bar is stale
	before it was ever fresh, and the eye is at full temperature. And a plugin
	that instead computes the delta and finds it zero must advance *nothing*;
	the fleet's habit of writing a one-pole as `coefficient = dt > 0 ? 1 -
	exp(-dt/T) : 1` does the opposite, snapping the detector to full on the one
	frame it has no information about. Either way the meter is wrong for the
	first second and a half of every clip -- which is exactly the window an
	operator is looking at it.

	So: frame one sets the origin, takes no steps, and every meter reads its
	rest position. Frame two is the first one with a real interval in it.
*/
class Engine
{
public:
	void Reset();

	void SetSettings( const Settings& settings );
	const Settings& CurrentSettings() const { return settings_; }
	const Resolved& CurrentBallistics() const { return resolved_; }

	/// One host frame. `amplitude` is per channel, linear, 1.0 = digital full
	/// scale. `hostSeconds` is the host's clock, already normalised to seconds.
	void Frame( double hostSeconds, const double* amplitude, int channels );

	const ChannelState& Channel( int index ) const;

	/// Seconds of host time the engine has actually integrated. Zero after the
	/// first frame, whatever the host's clock said.
	double EngineSeconds() const { return engineSeconds_; }

	/// Engine steps taken on the most recent `Frame`.
	long LastSteps() const { return lastSteps_; }

	bool Started() const { return started_; }

	static constexpr int kMaxChannels = 2;

private:
	void StepAll( double h, const double* amplitudeRatio, int channels );

	/// The four instruments for one channel. Named for what they are rather
	/// than for the channel, because `Channel` is already the accessor that
	/// hands out one of their published states.
	struct Models
	{
		Movement vu;
		Follower ppm;
		Hold     hold;
		Eye      eye;
		ChannelState state;
	};

	Settings settings_;
	Resolved resolved_ = Resolve( Settings{} );

	Models models_[ kMaxChannels ];

	bool   started_       = false;
	double lastHost_      = 0.0;
	double residual_      = 0.0;
	double engineSeconds_ = 0.0;
	long   lastSteps_     = 0;
};

} // namespace needle
