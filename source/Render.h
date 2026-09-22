#pragma once

#include <string>
#include <vector>

#include "meter/Standards.h"

#include <FFGLSDK.h>

/**
	The draw: one program, one triangle, one font texture, into the host's FBO.

	## Every pixel coordinate is computed on the CPU

	This is the single decision that shapes both this file and the harness. The
	shader is handed rectangles, radii and angles **in pixels**, already laid
	out, and does nothing but decide coverage. It computes no layout of its own.

	Two things follow, and both are worth the uniform slots it costs:

	  * `ndtest --pixels` can probe the picture at coordinates taken from the
	    same `Frame` the shader was given, so a probe is never a transcription
	    of the layout into the test. If the layout changes, the probe moves with
	    it, and the check still means "the shader drew where the CPU said".
	  * The layout is identical on every rasteriser, because it was decided
	    before any rasteriser saw it. What a GPU can still differ about is the
	    coverage of an *edge*, by a fraction of a pixel -- so every probe in the
	    harness sits at least two pixels inside the feature it is probing, and
	    nothing is asserted about an edge pixel.

	## The local frame

	Each instrument has a centre in screen pixels and a rotation. Everything
	inside it -- pivot, arc, segments, glyphs -- is in **local pixels**, y
	downward, origin at that centre. The shader rotates the fragment into the
	local frame once and works there.

	At Rotation 0 the local frame is the raster's own, so the glyphs land on
	whole pixels and the scale numbers are as crisp as graticule's. At any other
	angle the glyph grid is resampled with nearest-neighbour sampling and the
	labels get the ragged edges a rotated bitmap has. That is a stated limit,
	not a bug to be fixed with a filter: filtering it would soften every other
	edge on the instrument too.

	## No framebuffer of its own

	A meter is drawn straight into `HostFBO` at the viewport size the host
	reports, with blending on so `Background` and `Mix` mean something on a
	layer. Every `ffglex::Scoped*` binding clears to 0 on exit rather than
	restoring, so they are no help handing the context back; what this touches
	is put back by hand at the end of `Draw`.
*/
namespace needle
{

/// One tick on a dial's scale.
struct Mark
{
	float angle      = 0.0f;///< radians from straight up, + clockwise
	float lengthFrac = 0.5f;///< of the tick band's depth
	int   red        = 0;   ///< drawn in the over-zero colour
	int   textOffset = 0;   ///< into Frame::text; length 0 means no label
	int   textLength = 0;
};

/// One drawn instrument. Everything is in local pixels unless it says screen.
struct Unit
{
	float centre[ 2 ] = { 0.0f, 0.0f };///< screen pixels, y downward
	float rotation    = 0.0f;          ///< radians
	float halfW = 0.0f, halfH = 0.0f;
	float corner = 0.0f;

	// -- dial (VU, PPM) -----------------------------------------------------
	float pivot[ 2 ]   = { 0.0f, 0.0f };
	float arcRadius    = 0.0f;
	float arcHalfAngle = 0.0f;
	float needleLen    = 0.0f;
	float needleWidth  = 0.0f;
	float needleAngle  = 0.0f;
	float redFrom      = 10.0f;///< radians; >= arcHalfAngle means no red band

	// -- bargraph -----------------------------------------------------------
	float seg[ standards::kBargraphSteps ][ 4 ] = {};///< x, y, w, h
	int   segLit                                = 0;
	float hold[ 4 ]                             = { 0, 0, 0, 0 };///< w <= 0: none

	// -- magic eye ----------------------------------------------------------
	float eyeRadius  = 0.0f;
	float eyeShadow  = 0.0f;///< radians, HALF the shadow sector
	float eyeOverlap = 0.0f;///< radians, half the overlapping wedge
	float eyeWarm    = 0.0f;
};

/// Everything the next `ProcessOpenGL` would draw, in the shader's own units.
struct Frame
{
	int width = 0, height = 0;
	int type  = 0;///< MeterType
	int count = 1;
	int style = 0;///< scale style

	float face[ 3 ]   = { 0.93f, 0.90f, 0.80f };
	float needle[ 3 ] = { 0.10f, 0.09f, 0.08f };
	float back[ 3 ]   = { 0.0f, 0.0f, 0.0f };
	float backAlpha   = 0.0f;
	float lamp        = 0.0f;
	float glass       = 0.0f;
	float wear        = 0.0f;
	float mix         = 1.0f;

	Unit unit[ 2 ];

	std::vector< Mark > marks;
	std::vector< int >  text;///< character codes the marks and the legend index

	/// The instrument's name, drawn on the face. Length 0 means none.
	int   legendOffset = 0, legendLength = 0;
	float legendPos[ 2 ] = { 0.0f, 0.0f };///< local pixels, top-left of the string
	int   legendScale    = 1;
	int   markTextScale  = 1;

	static constexpr int kMaxMarks = 16;
	static constexpr int kMaxText  = 64;
};

class Renderer
{
public:
	Renderer() = default;

	bool InitGL();
	void DeInitGL();
	bool Ready() const { return mReady; }

	void Draw( const Frame& frame, GLuint hostFBO );

	const std::string& Note() const { return mNote; }

private:
	ffglex::FFGLShader mProgram;
	GLuint             mVao  = 0;
	GLuint             mFont = 0;

	bool        mReady = false;
	std::string mNote;
};

} // namespace needle
