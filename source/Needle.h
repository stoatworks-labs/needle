#pragma once

#include <string>

#include "Clock.h"
#include "Controls.h"
#include "Render.h"
#include "meter/Engine.h"

#include <FFGLSDK.h>

/**
	The plugin: four audio meters, each with the ballistics its standard
	specifies, drawn at the output's own raster.

	This class is the wiring. Everything that decides anything is elsewhere and
	is testable without a host, and in three of the four cases without any GL at
	all:

	  `meter/Standards`  the quoted figures, and the constants solved from them
	  `meter/Movement`   the four instruments, as differential equations
	  `meter/Engine`     those instruments integrated at an oversampled rate
	  `Audio`            the host's spectrum, and what is assumed about it
	  `Shaders`          coverage, from geometry the CPU already decided
	  `Render`           one triangle into the host's framebuffer

	## Per frame

	`ProcessOpenGL` reads the spectrum, hands the engine the host's clock, and
	then calls `BuildFrame`, which turns the engine's published state and the
	parameter cache into a `Frame` in the shader's units -- pixels, y downward.
	`BuildFrame` is `const` and touches no GL, which is how `ndtest --pixels`
	gets at the exact geometry the shader was given.

	## The order of the two clocks

	`SetTime` is a host's word for where the composition is, in a unit FFGL
	never names. `Clock` measures that unit rather than assuming it, and the
	engine is handed the result in seconds. Nothing downstream of `Clock` ever
	sees a host time, which matters here more than in most of the fleet: every
	ballistic constant in this plugin is a duration, so a clock reading a
	thousand times fast does not make the meters look wrong, it makes them look
	instantaneous -- which is indistinguishable from a meter with no ballistics
	at all, and therefore from the plugin having no point.
*/
namespace needle
{
class NeedlePlugin : public CFFGLPlugin
{
public:
	NeedlePlugin();
	~NeedlePlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float    GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char*    GetTextParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	/// The settings the current parameters mean, in physical units.
	Settings CurrentSettings() const;

	/// The Frame the next ProcessOpenGL would draw at this size: the same code
	/// path, without a context.
	Frame BuildFrame( int width, int height ) const;

	/// Read the host's spectrum into one amplitude, the way the frame path
	/// does. Public so the harness can confirm what it injected arrived.
	float InputLevel() const;

	/// Advance the engine one host frame. `ProcessOpenGL` calls this; the
	/// harness calls it directly when it wants no GL involved.
	void AdvanceEngine( double hostSeconds );

	const Engine& EngineState() const { return mEngine; }

	/// For the harness, which sends seconds and wants the movement where the
	/// arithmetic says it is rather than where a wall clock put it.
	void ForceSecondsClock() { mClock.ForceSeconds(); }

	/// Screen pixels for a point in a unit's local frame. The inverse of what
	/// the shader does, and the only supported way for a test to turn the
	/// geometry in a `Frame` into somewhere to probe.
	static void LocalToScreen( const Unit& unit, float lx, float ly, float& sx, float& sy );

private:
	int OptionIndex( unsigned int param, int count ) const;

	float mParams[ PT_COUNT_ ] = {};

	Renderer mRenderer;
	Clock    mClock;
	Engine   mEngine;

	/// Which instance this is within the host process; several clips of this
	/// plugin can live in one composition and their log lines would otherwise
	/// interleave into one account of a plugin that contradicts itself.
	int         mInstanceId = 0;
	std::string mTag;

	bool   mGlReady      = false;
	bool   mHostTimeSeen = false;
	double mHostTime     = 0.0;
};

} // namespace needle
