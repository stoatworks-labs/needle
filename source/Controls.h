#pragma once

#include <cmath>

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

/**
	Every parameter, and what its host-side value means.

	## The two units

	FFGL hands a plugin floats. What a float *means* depends on the type the
	parameter was declared with, and the fleet has been bitten often enough that
	the rule is written down rather than remembered:

	- **`FF_TYPE_STANDARD` is 0..1**, always. `SetParamInfo` clamps the default
	  into that range before `SetParamRange` could widen it, and there is no
	  `SetParamDefault`, so a standard parameter that wants to mean anything
	  else is mapped here, in a named inline function, and nowhere else.
	- **`FF_TYPE_OPTION` holds the element VALUE**, which for every dropdown
	  here is its index. `ToOption` in Needle.cpp accepts either that or a
	  normalised 0..1 from a host that does it the other way.
	- **`FF_TYPE_BOOLEAN` is 0 or 1**, read as `> 0.5f`.
	- **`FF_TYPE_BUFFER`** holds the spectrum. Its elements are written by the
	  host, not by the operator, and it is the only parameter here that carries
	  information rather than intent.

	## Where the defaults come from

	Every default in the Ballistics group is the value that makes Free agree
	with Standard for the quantity it is named after: Rise and Overshoot give
	the VU's standard movement, Fall the PPM's standard fall-back. `ndtest
	--defaults` asserts exactly those three. It is NOT a promise that flipping
	the switch changes nothing: one Rise and one Fall cannot be a VU and a PPM at
	once, so in Free the VU falls over the PPM's 2.8 s and the PPM rises over
	the VU's 300 ms. Found writing the user guide, 2026-09-23.

	## Order is load-bearing

	The host draws parameters in declaration order and `SetParamGroup` collapses
	*runs* of the same group name into one fold, so an id moved out of its run
	splits its group in two in the inspector. The enum below is the inspector,
	top to bottom.

	## Names

	FFGL truncates a parameter name at 16 characters, in the host, silently.
	`ndtest --names` lists any that are over. They must also be unique:
	`ndtest --set` and `tools/sweep.py` find a parameter by its name.
*/
namespace needle
{
enum ParamId : FFUInt32
{
	// -- Meter --------------------------------------------------------------
	PT_TYPE,
	PT_COUNT,
	PT_REFERENCE,
	PT_SENSITIVITY,
	PT_BIN_LAW,
	PT_STANDARD,

	// -- Ballistics ---------------------------------------------------------
	PT_RISE,
	PT_FALL,
	PT_OVERSHOOT,
	PT_PEAK_HOLD,
	PT_HOLD_DECAY,

	// -- Look ---------------------------------------------------------------
	PT_FACE_R,
	PT_FACE_G,
	PT_FACE_B,
	PT_NEEDLE_R,
	PT_NEEDLE_G,
	PT_NEEDLE_B,
	PT_SCALE_STYLE,
	PT_LAMP,
	PT_GLASS,
	PT_WEAR,
	PT_PERSISTENCE,

	// -- Layout -------------------------------------------------------------
	PT_SIZE,
	PT_POS_X,
	PT_POS_Y,
	PT_ROTATION,
	PT_BACKGROUND,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_MIX,

	// -- Audio --------------------------------------------------------------
	PT_AUDIO,///< the host's spectrum; see Audio.h

	// -- About --------------------------------------------------------------
	// One text line and one button per link. Its size is decided by
	// StoatworksAbout.h at compile time, so Needle.cpp static_asserts this run
	// against `about::kParamCount`.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,

	PT_COUNT_
};

/// How the host's spectrum values are to be read. Nobody in this fleet has ever
/// measured which of these Resolume sends, so it is a control rather than a
/// guess -- see Audio.h.
enum class BinLaw : int
{
	Magnitude = 0,
	Power,
	Count
};
constexpr int kBinLawCount = static_cast< int >( BinLaw::Count );

/// How much of the scale is drawn.
enum class ScaleStyle : int
{
	Full = 0,  ///< marks, numbers and the legend
	MarksOnly, ///< marks, no text
	Plain,     ///< the arc alone
	None,      ///< face and pointer only
	Count
};
constexpr int kScaleStyleCount = static_cast< int >( ScaleStyle::Count );

// ---------------------------------------------------------------------------
// The 0..1 controls, in engineering units.
//
// Each default below is quoted next to it, because the constructor declares the
// default as a 0..1 number and the number that matters is the physical one.
// ---------------------------------------------------------------------------

/// The digital level the meter calls zero. -40 dBFS to 0 dBFS; the default of
/// 0.55 is **-18 dBFS**, the EBU R68 alignment level, which is also close
/// enough to SMPTE RP 155's -20 dBFS that an operator can find either.
inline float ReferenceDbfs( float v )
{
	return -40.0f + 40.0f * v;
}
inline constexpr float kReferenceDefault = 0.55f;

/// Input trim, -12 dB to +12 dB. The default of 0.5 is 0 dB. This exists
/// because the absolute calibration of the host's spectrum is not known -- see
/// Audio.h -- so the operator needs a way to put the needle where the programme
/// actually sits.
inline float TrimDb( float v )
{
	return -12.0f + 24.0f * v;
}
inline constexpr float kSensitivityDefault = 0.5f;

/// Free-mode rise: the time a movement takes to reach 99 % of a rising step.
/// 20 ms to 2 s, logarithmic. The default is **300 ms**, ANSI C16.5's figure.
inline float RiseSeconds( float v )
{
	return 0.02f * std::pow( 100.0f, v );
}
inline constexpr float kRiseDefault = 0.5880456f;

/// Free-mode fall. Read as a 99 % time by the movements and as a 20 dB
/// fall-back time by the detectors -- both are "how long a fall takes",
/// measured the way each instrument's own standard measures it. 50 ms to 20 s,
/// logarithmic; the default is **2.8 s**, IEC 60268-10 type II's figure.
inline float FallSeconds( float v )
{
	return 0.05f * std::pow( 400.0f, v );
}
inline constexpr float kFallDefault = 0.6718477f;

/// Free-mode overshoot as a fraction. 0.1 % to 50 %, logarithmic, so the
/// standard's **1.25 %** lands near the middle of the travel instead of in the
/// first twentieth of it. The bottom of the range is not zero on purpose: zero
/// overshoot is critical damping, an end stop rather than a setting, and 0.1 %
/// is already visually indistinguishable from it.
inline float OvershootFraction( float v )
{
	return 0.001f * std::pow( 500.0f, v );
}
inline constexpr float kOvershootDefault = 0.4064180f;

/// How long the hold bar sits at a peak, 0 to 10 s; zero turns the bar off
/// altogether. Nothing specifies either of these.
inline float PeakHoldSeconds( float v )
{
	return 10.0f * v * v;
}
inline constexpr float kPeakHoldDefault = 0.3872983f;///< 1.5 s

/// How fast it falls afterwards, 0 to 48 dB/s. Nothing specifies this either.
inline float HoldDecayDbPerSecond( float v )
{
	return 48.0f * v;
}
inline constexpr float kHoldDecayDefault = 0.25f;///< 12 dB/s

/// The magic eye's phosphor, 0 to 500 ms.
inline float PersistenceSeconds( float v )
{
	return 0.5f * v;
}
inline constexpr float kPersistenceDefault = 0.12f;///< 60 ms

/// The instrument's height as a fraction of the raster's shorter side.
inline float SizeFraction( float v )
{
	return 0.15f + 0.85f * v;
}
inline constexpr float kSizeDefault = 0.55f;

/// Position, in half-short-sides from the centre.
inline float Offset( float v )
{
	return ( v - 0.5f ) * 2.0f;
}

/// Rotation in radians, -180 to +180 degrees.
inline float RotationRadians( float v )
{
	return ( v - 0.5f ) * 2.0f * 3.14159265358979323846f;
}

} // namespace needle
