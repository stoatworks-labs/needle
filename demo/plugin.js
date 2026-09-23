/**
 * Needle — browser demo.
 *
 * Four audio meters — a VU, a BBC PPM, an LM3915 LED bargraph and a magic eye —
 * each moving the way its standard says it must. The one idea, from `AGENTS.md`:
 * **the movement is not a free choice.** ANSI C16.5 gives a VU two sentences
 * (99 % in 300 ms, 1.0–1.5 % overshoot) and a second-order movement has two free
 * parameters, so `Standards.cpp` solves for them rather than tuning them.
 *
 * Two halves, and they are not equally faithful:
 *
 *   The renderer is the plugin's. `VERTEX_SHADER`, `FRAGMENT_SHADER_A` and
 *   `FRAGMENT_SHADER_B` below are the three raw strings in
 *   `source/Shaders.cpp` — the fragment shader is two adjacent raw strings there
 *   because MSVC caps one literal at about 16 KB, and it is two constants here,
 *   joined the way the C++ compiler joins them. They are copied across
 *   unedited, and `demo/tools/check_shaders.py` compares every one of them (and
 *   the joined fragment text) to the C++ character for character;
 *   `tools/verify.sh` runs it. One program, one full-screen triangle from
 *   gl_VertexID, one font texture — the same single pass as `Renderer::Draw`.
 *
 *   Everything the CPU does is a PORT, and nothing checks the port but a
 *   reader. `meter/Standards` (the closed-form ζ and the bisected ωn, the two
 *   PPM time constants), `meter/Movement` (RK4 movement with dry friction, the
 *   quasi-peak follower, the hold bar, the magic eye), `meter/Engine` (4800 Hz,
 *   whole steps with the remainder carried, frame-one priming, the 0.25 s stall
 *   clamp, Resolve for Standard and Free, the scale maps, the LM3915 law),
 *   `Audio::LevelFromSpectrum` (the Bin Law), `Controls.h` (every 0..1 → unit
 *   conversion), `NeedlePlugin::BuildFrame` (every pixel coordinate the shader
 *   is handed), `Renderer::Draw` (the uniform packing) and `Font.cpp` (the 5×7
 *   glyph table) are all below. `ndtest` checks the C++; it has never seen this
 *   file.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **There is no audio in a browser, and this page does not ask for a
 * microphone.** The plugin reads one thing from its host: Resolume's
 * FF_USAGE_FFT buffer, 64 bins, once per frame. So the meters here are driven
 * by a GENERATED TEST SIGNAL — a 1 kHz tone at the reference level, a 0 VU step,
 * a −20 dB step, 5 ms bursts, a ladder of burst lengths, pink noise and silence —
 * chosen from the kit's one transport dropdown, labelled "Test signal". For
 * every frame the page works out the signal's power over that frame's interval
 * (one spectrum per frame is the plugin's input resolution too) and writes it
 * into 64 bins: a tone into one bin, pink noise spread as 1/f with the
 * frame-to-frame scatter a short FFT of noise has. The plugin's own level law
 * then runs on those bins — the square root of the sum over ALL of them, under
 * whichever Bin Law is selected — and everything downstream is the port.
 *
 * **It is not the host's FFT.** Resolume's window, normalisation, headroom and
 * whether a bin is a magnitude or a power are unknown — `AGENTS.md` lists them as
 * open questions, which is why Bin Law, Reference Level and Sensitivity exist.
 * The page writes MAGNITUDES, so Bin Law on Magnitude reads the test signal at
 * exactly the level it was generated at, and Power shows what the other
 * assumption does to the same numbers. A test level quoted in dBFS here means
 * "what the plugin's level law reads": RMS, with 1.0 as full scale.
 *
 * **No clip picker, and no "use my own file".** Needle is an FFGL source —
 * `SetMinInputs( 0 )`, `SetMaxInputs( 0 )` — so both kit controls are removed
 * from the DOM after mounting, as astable's page does, and `demo.blurb` replaces
 * the banner's "on generated clips" clause.
 *
 * **The Audio buffer parameter and the About block are absent from the panel.**
 * The buffer is written by a host, never by an operator; its stand-in here is
 * the test signal. The About block is a text line and four link buttons so a
 * host has somewhere to put them; a web page has links of its own. Every other
 * parameter — 31 of them — is here, with the constructor's names, groups,
 * types, element lists and defaults, in `Controls.h` order. Needle declares no
 * FF_TYPE_INTEGER controls, so nothing needed galvo's dropdown workaround.
 *
 * **The clock is the kit's, already in seconds.** The plugin's `Clock` measures
 * what unit a host's SetTime is in; that is not ported, because the page's clock
 * has no unit to discover. Restart sends time backwards, which the engine reads
 * as a loop point and advances nothing — the plugin's behaviour, kept.
 *
 * ---------------------------------------------------------------------------
 * What this page is NOT evidence about
 * ---------------------------------------------------------------------------
 *
 * GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core; JavaScript doubles
 * where the plugin has floats in places (the parameter conversions are rounded
 * through Math.fround to match, the engine is double on both sides); a browser's
 * frame clock rather than a host's. Nothing here measures anything: the ANSI and
 * IEC claims are checked by `ndtest --ballistics`, `--ppm`, `--steps`, `--eye`,
 * `--prime`, `--rate` and `--friction` in the repository, with no GL context at
 * all, and that harness is the reason to believe the model.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The one backtick inside a comment is escaped, because a template literal has
// nowhere else to put it; check_shaders.py decodes that one escape before
// comparing and rejects any other backslash, so the escape cannot hide a
// difference.
//---------------------------------------------------------------------------

const VERTEX_SHADER = `#version 410 core
// One triangle that covers the viewport. No vertex buffer: the corners come
// from gl_VertexID, so the only thing bound at draw time is the VAO.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
`;

const FRAGMENT_SHADER_A = `#version 410 core
out vec4 fragColour;

uniform ivec2 uSize;
uniform int   uType; // 0 VU, 1 PPM, 2 bargraph, 3 magic eye
uniform int   uCount;
uniform int   uStyle;// 0 full, 1 marks only, 2 plain, 3 none

uniform vec3  uFace;
uniform vec3  uNeedleCol;
uniform vec3  uBackCol;
uniform float uBackA;
uniform float uLamp;
uniform float uGlass;
uniform float uWear;
uniform float uMix;

// Per instrument. Packed four to a vec4 because a uniform slot is cheaper than
// a struct array and every one of these is written in exactly one place.
uniform vec4 uUnitA[ 2 ];// centre.x, centre.y, rotation, corner
uniform vec4 uUnitB[ 2 ];// halfW, halfH, pivot.x, pivot.y
uniform vec4 uUnitC[ 2 ];// arcRadius, arcHalfAngle, needleLen, needleWidth
uniform vec4 uUnitD[ 2 ];// needleAngle, redFrom, eyeRadius, eyeShadow
uniform vec4 uUnitE[ 2 ];// eyeOverlap, eyeWarm, segLit, holdVisible
uniform vec4 uSeg[ 20 ]; // ten rects per instrument: x, y, w, h
uniform vec4 uHoldRect[ 2 ];

uniform int   uMarkCount;
uniform vec4  uMark[ 16 ];    // angle, lengthFrac, red, 0
uniform ivec2 uMarkText[ 16 ];// offset into uText, length
uniform int   uText[ 64 ];
uniform ivec4 uLegend;        // offset, length, legend scale, mark scale
uniform vec2  uLegendPos;
uniform sampler2D uFont;

const float kPi = 3.141592653589793;

// The over-zero band. A colour out of a standard rather than a taste, so it is
// not a control.
const vec3 kScaleRed = vec3( 0.72, 0.09, 0.07 );
// A magic eye's target. Zinc orthosilicate: the green everybody pictures.
const vec3 kEyeGreen = vec3( 0.36, 1.00, 0.46 );

// ---------------------------------------------------------------------------
// Coverage. One pixel, linear. See Shaders.h.
// ---------------------------------------------------------------------------
float cov( float d ) { return clamp( 0.5 - d, 0.0, 1.0 ); }

vec4 over( vec4 src, vec4 dst )
{
	float a = src.a + dst.a * ( 1.0 - src.a );
	if( a <= 0.0 )
		return vec4( 0.0 );
	return vec4( ( src.rgb * src.a + dst.rgb * dst.a * ( 1.0 - src.a ) ) / a, a );
}

vec4 paint( vec4 dst, vec3 colour, float coverage )
{
	return over( vec4( colour, clamp( coverage, 0.0, 1.0 ) ), dst );
}

// ---------------------------------------------------------------------------
// Signed distances, in pixels.
// ---------------------------------------------------------------------------
float sdBox( vec2 p, vec2 b )
{
	vec2 q = abs( p ) - b;
	return min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) );
}

float sdRoundBox( vec2 p, vec2 b, float r )
{
	vec2 q = abs( p ) - b + r;
	return min( max( q.x, q.y ), 0.0 ) + length( max( q, vec2( 0.0 ) ) ) - r;
}

float sdSegment( vec2 p, vec2 a, vec2 b )
{
	vec2  pa = p - a;
	vec2  ba = b - a;
	float h  = clamp( dot( pa, ba ) / max( 1e-6, dot( ba, ba ) ), 0.0, 1.0 );
	return length( pa - ba * h );
}

// Angle from straight UP, positive clockwise, in the local frame where y runs
// downward. Straight up is (0, -1), so atan( x, -y ) is zero there.
float angleUp( vec2 p ) { return atan( p.x, -p.y ); }
// Angle from straight DOWN, positive clockwise: the magic eye's shadow is cast
// downward from the tube's plate.
float angleDown( vec2 p ) { return atan( p.x, p.y ); }

// An annular band between two radii, clipped to |angle| <= halfAngle, as a
// signed distance. The angular end is treated as a plane through the origin,
// which is exact for a band whose thickness is small against its radius --
// which every band here is.
float sdArcBand( vec2 p, float rIn, float rOut, float angle, float halfAngle )
{
	float r = length( p );
	float d = max( rIn - r, r - rOut );
	float a = abs( angle ) - halfAngle;
	if( a > 0.0 )
		d = max( d, a * r );
	return d;
}

// ---------------------------------------------------------------------------
// Integer hashing, for wear. PCG-style output mix: exact in 32 bits, so two
// GPUs put the same speck in the same place.
// ---------------------------------------------------------------------------
uint hashU( uint x )
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float hash01( ivec2 p )
{
	uint h = hashU( uint( p.x + 4096 ) * 73856093u ^ uint( p.y + 4096 ) * 19349663u );
	return float( h & 0xffffffu ) / float( 0xffffffu );
}

float valueNoise( vec2 p )
{
	vec2  i  = floor( p );
	vec2  fp = p - i;
	ivec2 ii = ivec2( i );
	float a  = hash01( ii );
	float b  = hash01( ii + ivec2( 1, 0 ) );
	float c  = hash01( ii + ivec2( 0, 1 ) );
	float d  = hash01( ii + ivec2( 1, 1 ) );
	vec2  u  = fp * fp * ( 3.0 - 2.0 * fp );
	return mix( mix( a, b, u.x ), mix( c, d, u.x ), u.y );
}

// ---------------------------------------------------------------------------
// Text, from the 5x7 table. \`origin\` is the top-left of the string in local
// pixels; the glyph grid is integer there, so at Rotation 0 every label lands
// on whole pixels.
// ---------------------------------------------------------------------------
bool textHit( vec2 p, vec2 origin, int offset, int len, int s )
{
	if( len <= 0 || s <= 0 )
		return false;
	vec2 q = p - origin;
	if( q.x < 0.0 || q.y < 0.0 )
		return false;
	int qx = int( floor( q.x ) );
	int qy = int( floor( q.y ) );
	int ly = qy / s;
	if( ly >= 7 )
		return false;
	int adv = 6 * s;
	int ci  = qx / adv;
	if( ci >= len )
		return false;
	int lx = ( qx - ci * adv ) / s;
	if( lx >= 5 )
		return false;
	int index = offset + ci;
	if( index < 0 || index >= 64 )
		return false;
	int code = uText[ index ];
	return texelFetch( uFont, ivec2( code * 5 + lx, ly ), 0 ).r > 0.5;
}

float textWidth( int len, int s ) { return float( len * 6 * s - s ); }
`;

const FRAGMENT_SHADER_B = `
// ---------------------------------------------------------------------------
// The dial: VU and PPM. Same movement, different scale.
// ---------------------------------------------------------------------------
vec4 drawDial( vec4 dst, vec2 p, int u )
{
	vec2  half_ = uUnitB[ u ].xy;
	vec2  pivot = uUnitB[ u ].zw;
	float R     = uUnitC[ u ].x;
	float halfA = uUnitC[ u ].y;
	float nLen  = uUnitC[ u ].z;
	float nW    = uUnitC[ u ].w;
	float nAng  = uUnitD[ u ].x;
	float red   = uUnitD[ u ].y;

	// The face, and the clip. On a real meter the pointer's pivot is below the
	// window, hidden behind the bezel, because a long radius is what gives a
	// shallow scale arc across the top -- so everything drawn here is masked by
	// the face, and the boss and the bottom of the pointer simply are not
	// there. Without the mask they would be drawn on the background.
	float onFace = cov( sdRoundBox( p, half_, uUnitA[ u ].w ) );
	dst = paint( dst, uFace, onFace );
	if( onFace <= 0.0 )
		return dst;

	vec2  rel  = p - pivot;
	float ang  = angleUp( rel );
	float tick = R * 0.060;
	vec3  ink  = uNeedleCol * 0.85 + vec3( 0.05 );

	if( uStyle < 3 )
	{
		// The scale arc, and the red band just outside it.
		float arcW = max( 1.0, R * 0.006 );
		dst = paint( dst, ink, onFace * cov( sdArcBand( rel, R - arcW, R, ang, halfA ) ) );
		if( red < halfA )
		{
			float band = max( 1.5, R * 0.016 );
			dst        = paint( dst, kScaleRed,
								onFace * cov( sdArcBand( rel, R + arcW * 0.8, R + arcW * 0.8 + band,
											    ang - 0.5 * ( red + halfA ),
											    0.5 * ( halfA - red ) ) ) );
		}

		// The ticks, and their numbers. Plain draws the arc and the red band
		// and stops there; Marks Only adds the ticks; Full adds the numbers.
		for( int m = 0; m < 16 && uStyle <= 1; ++m )
		{
			if( m >= uMarkCount )
				break;
			float ma  = uMark[ m ].x;
			float len = uMark[ m ].y * tick;
			vec2  dir = vec2( sin( ma ), -cos( ma ) );
			vec2  a   = pivot + dir * ( R - len );
			vec2  b   = pivot + dir * R;
			vec3  col = uMark[ m ].z > 0.5 ? kScaleRed : ink;
			float w   = max( 1.0, R * ( uMark[ m ].y > 0.75 ? 0.0045 : 0.003 ) );
			dst       = paint( dst, col, onFace * cov( sdSegment( p, a, b ) - w * 0.5 ) );

			if( uStyle == 0 && uMarkText[ m ].y > 0 )
			{
				int   s  = uLegend.w;
				vec2  lc = pivot + dir * ( R - tick - float( s ) * 5.0 );
				vec2  o  = floor( vec2( lc.x - 0.5 * textWidth( uMarkText[ m ].y, s ),
										lc.y - 3.5 * float( s ) ) );
				if( textHit( p, o, uMarkText[ m ].x, uMarkText[ m ].y, s ) )
					dst = paint( dst, col, onFace );
			}
		}

		// The instrument's name.
		if( uStyle == 0 && uLegend.y > 0 )
		{
			if( textHit( p, uLegendPos, uLegend.x, uLegend.y, uLegend.z ) )
				dst = paint( dst, ink, onFace );
		}
	}

	// The pointer, and the shadow it casts on the face below it.
	vec2  dir  = vec2( sin( nAng ), -cos( nAng ) );
	vec2  tip  = pivot + dir * nLen;
	float sh   = R * 0.010;
	dst = paint( dst, vec3( 0.0 ),
				 onFace * 0.30 * cov( sdSegment( p - vec2( sh, sh ), pivot, tip ) - nW * 0.5 ) );
	dst = paint( dst, uNeedleCol, onFace * cov( sdSegment( p, pivot, tip ) - nW * 0.5 ) );
	// The boss the pointer turns on. Usually off the bottom of the face and
	// therefore invisible, which is the point of the mask.
	dst = paint( dst, uNeedleCol, onFace * cov( length( p - pivot ) - nW * 2.2 ) );
	return dst;
}

// ---------------------------------------------------------------------------
// The bargraph.
// ---------------------------------------------------------------------------
vec4 drawBargraph( vec4 dst, vec2 p, int u )
{
	dst = paint( dst, uFace, cov( sdRoundBox( p, uUnitB[ u ].xy, uUnitA[ u ].w ) ) );

	int lit = int( uUnitE[ u ].z + 0.5 );
	for( int k = 0; k < 10; ++k )
	{
		vec4 r = uSeg[ u * 10 + k ];
		if( r.z <= 0.0 )
			continue;
		vec2 c = r.xy + 0.5 * r.zw;
		vec2 b = 0.5 * r.zw;

		// The LM3915 law puts the top step at the reference and every step
		// 3 dB below the one above it, so the colours are decided by the
		// thresholds and not by where the operator wants the warning to be:
		// the top step is the reference, the two under it are the 3 and 6 dB
		// of headroom, and the rest is programme.
		vec3 onCol = ( k >= 9 ) ? vec3( 0.95, 0.16, 0.12 )
				   : ( k >= 7 ) ? vec3( 0.98, 0.76, 0.12 )
								: vec3( 0.25, 0.92, 0.35 );
		vec3 offCol = mix( uFace, uNeedleCol, 0.82 );

		float c0 = cov( sdRoundBox( p - c, b, min( b.x, b.y ) * 0.25 ) );
		dst      = paint( dst, k < lit ? onCol : offCol, c0 );
	}

	vec4 h = uHoldRect[ u ];
	if( uUnitE[ u ].w > 0.5 && h.z > 0.0 )
	{
		vec2 c = h.xy + 0.5 * h.zw;
		dst    = paint( dst, vec3( 1.0, 0.98, 0.9 ), cov( sdBox( p - c, 0.5 * h.zw ) ) );
	}
	return dst;
}

// ---------------------------------------------------------------------------
// The magic eye.
// ---------------------------------------------------------------------------
vec4 drawEye( vec4 dst, vec2 p, int u )
{
	float R       = uUnitD[ u ].z;
	float shadow  = uUnitD[ u ].w;
	float overlap = uUnitE[ u ].x;
	float warm    = uUnitE[ u ].y;

	// The bezel the tube sits in.
	dst = paint( dst, uFace, cov( sdRoundBox( p, uUnitB[ u ].xy, uUnitA[ u ].w ) ) );
	dst = paint( dst, uFace * 0.25, cov( length( p ) - R * 1.06 ) );

	// A 6U5's target is a wide ring with a small hub, not a thin band round a
	// big pupil: the fluorescence is what you are meant to read.
	float rIn  = R * 0.32;
	float a    = angleDown( p );
	float band = sdArcBand( p, rIn, R, 0.0, kPi );
	float inTarget = cov( band );

	// Unexcited target: the phosphor is there whether or not it is lit.
	dst = paint( dst, kEyeGreen * 0.10, inTarget );

	// The shadow the plate casts, closing as the level rises.
	float lit = inTarget;
	if( shadow > 0.0 )
		lit *= 1.0 - cov( ( abs( a ) - shadow ) * length( p ) );

	dst = paint( dst, kEyeGreen, lit * warm );

	// Past closure the two wings sweep through one another; where they overlap
	// the target is driven twice and burns brighter. This is the thing an
	// operator actually watches for.
	if( overlap > 0.0 )
	{
		float w = inTarget * ( 1.0 - cov( ( abs( a ) - overlap ) * length( p ) ) );
		dst     = paint( dst, mix( kEyeGreen, vec3( 1.0 ), 0.55 ), w * warm );
	}

	// The cathode down the middle, and the rim.
	dst = paint( dst, vec3( 0.06, 0.07, 0.06 ), cov( length( p ) - rIn ) );
	float rimW = max( 1.0, R * 0.02 );
	dst = paint( dst, uFace * 0.55, cov( abs( length( p ) - R ) - rimW * 0.5 ) );
	return dst;
}

// ---------------------------------------------------------------------------
void main()
{
	// Top-down continuous pixels: row 0 has its centre at y = 0.5, which is the
	// convention every rectangle in the Frame is expressed in.
	vec2 f = vec2( gl_FragCoord.x, float( uSize.y ) - gl_FragCoord.y );

	vec4 c = vec4( uBackCol, uBackA );

	for( int u = 0; u < 2; ++u )
	{
		if( u >= uCount )
			break;

		float rot = uUnitA[ u ].z;
		vec2  d   = f - uUnitA[ u ].xy;
		float cs  = cos( rot );
		float sn  = sin( rot );
		vec2  p   = vec2( d.x * cs + d.y * sn, -d.x * sn + d.y * cs );

		if( uType == 2 )
			c = drawBargraph( c, p, u );
		else if( uType == 3 )
			c = drawEye( c, p, u );
		else
			c = drawDial( c, p, u );

		// The lamp behind the face: warm, brightest above the middle, and it
		// lifts the black of the needle as a real bulb behind a dial does.
		if( uLamp > 0.0 )
		{
			float inside = cov( sdRoundBox( p, uUnitB[ u ].xy, uUnitA[ u ].w ) );
			float g = exp( -dot( ( p - vec2( 0.0, -uUnitB[ u ].y * 0.55 ) ),
								 ( p - vec2( 0.0, -uUnitB[ u ].y * 0.55 ) ) ) /
						   ( uUnitB[ u ].y * uUnitB[ u ].y * 1.1 ) );
			c.rgb += vec3( 1.0, 0.86, 0.62 ) * ( uLamp * 0.45 * g * inside );
		}

		// Wear: a dirty face and a tired lamp. Value noise off the integer
		// hash, so it is the same dirt on every machine.
		if( uWear > 0.0 )
		{
			float inside = cov( sdRoundBox( p, uUnitB[ u ].xy, uUnitA[ u ].w ) );
			float blotch = valueNoise( p / max( 4.0, uUnitB[ u ].y * 0.18 ) );
			float speck  = valueNoise( p * 0.7 + vec2( 91.0, 17.0 ) );
			float dirt   = mix( 0.82, 1.0, blotch ) * ( speck > 0.985 ? 0.45 : 1.0 );
			c.rgb        = mix( c.rgb, c.rgb * dirt, uWear * inside );
		}

		// Glass: one specular streak across the top left, the way a curved
		// cover reflects a room. Additive, because a reflection adds light.
		if( uGlass > 0.0 )
		{
			float inside = cov( sdRoundBox( p, uUnitB[ u ].xy, uUnitA[ u ].w ) );
			vec2  q      = vec2( p.x * 0.55 + p.y * 0.83, -p.x * 0.83 + p.y * 0.55 );
			float streak = exp( -pow( ( q.y + uUnitB[ u ].y * 0.62 ) /
									  max( 2.0, uUnitB[ u ].y * 0.11 ), 2.0 ) );
			c.rgb += vec3( 1.0 ) * ( uGlass * 0.22 * streak * inside );
		}
	}

	c.a = clamp( c.a * uMix, 0.0, 1.0 );
	c.rgb = clamp( c.rgb, 0.0, 1.0 );

	// Premultiplied: Resolume composites premultiplied alpha, and straight
	// alpha makes every edge darker than it should be against anything but
	// black.
	fragColour = vec4( c.rgb * c.a, c.a );
}
`;

// The fragment shader is the two raw strings joined, exactly as the C++
// compiler joins adjacent literals: no separator.
const FRAGMENT_SHADER = FRAGMENT_SHADER_A + FRAGMENT_SHADER_B;

//===========================================================================
// PORT — source/meter/Standards.{h,cpp}. The quoted figures, and the constants
// solved from them. Nothing tuned.
//===========================================================================

const PI = 3.14159265358979323846;

const kVu99 = 0.99;
const kVuRiseTime = 0.300;
const kVuOvershootLo = 0.010;
const kVuOvershootHi = 0.015;
const kVuOvershoot = 0.5 * (kVuOvershootLo + kVuOvershootHi);

const kPpmFallDb = 20.0;
const kPpmFallSeconds = 2.8;
const kPpmBurstSeconds = 0.005;
const kPpmBurstDownDb = 2.0;

const kBargraphStepDb = 3.0;
const kBargraphSteps = 10;

const kEyeOpenDegrees = 100.0;

const kBisectionSteps = 80;

function dampingForOvershoot(overshoot) {
  if (overshoot <= 0.0) return 1.0;
  if (overshoot >= 0.99) overshoot = 0.99;
  const l = Math.log(overshoot);
  return -l / Math.sqrt(PI * PI + l * l);
}

function stepResponse(t, zeta, omegaN) {
  if (t <= 0.0) return 0.0;
  if (zeta < 1.0 - 1e-9) {
    const wd = omegaN * Math.sqrt(1.0 - zeta * zeta);
    const phi = Math.acos(zeta);
    return 1.0 - Math.exp(-zeta * omegaN * t) / Math.sqrt(1.0 - zeta * zeta) * Math.sin(wd * t + phi);
  }
  if (zeta > 1.0 + 1e-9) {
    const r = Math.sqrt(zeta * zeta - 1.0);
    const s1 = -omegaN * (zeta - r);
    const s2 = -omegaN * (zeta + r);
    return 1.0 - (s1 * Math.exp(s2 * t) - s2 * Math.exp(s1 * t)) / (s1 - s2);
  }
  const x = omegaN * t;
  return 1.0 - (1.0 + x) * Math.exp(-x);
}

/** The FIRST crossing: the bracket is grown from zero, never opened wide. */
function normalisedTimeToReach(fraction, zeta) {
  let hi = 0.5;
  while (stepResponse(hi, zeta, 1.0) < fraction && hi < 64.0) hi *= 2.0;
  let lo = 0.0;
  for (let i = 0; i < kBisectionSteps; i += 1) {
    const mid = 0.5 * (lo + hi);
    if (stepResponse(mid, zeta, 1.0) < fraction) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

const naturalFrequencyFor = (fraction, seconds, zeta) => normalisedTimeToReach(fraction, zeta) / seconds;

const VU_DAMPING = dampingForOvershoot(kVuOvershoot);
const VU_NATURAL_FREQUENCY = naturalFrequencyFor(kVu99, kVuRiseTime, VU_DAMPING);
const PPM_FALL_TAU = kPpmFallSeconds / (kPpmFallDb / 20.0 * Math.log(10.0));
const PPM_RISE_TAU = -kPpmBurstSeconds / Math.log(1.0 - Math.pow(10.0, -kPpmBurstDownDb / 20.0));

//===========================================================================
// PORT — source/meter/Movement.{h,cpp}.
//===========================================================================

const MeterType = { Vu: 0, Ppm: 1, Bargraph: 2, Eye: 3 };
const kMeterTypeCount = 4;

const copysign = (magnitude, sign) => ((sign < 0 || Object.is(sign, -0)) ? -magnitude : magnitude);

function field(x, v, u, zeta, omegaN) {
  return [v, omegaN * omegaN * (u - x) - 2.0 * zeta * omegaN * v];
}

/** One-pole coefficient, exact for a constant target; h <= 0 returns 0. */
function pole(h, tau) {
  if (h <= 0.0 || tau <= 0.0) return h > 0.0 ? 1.0 : 0.0;
  return 1.0 - Math.exp(-h / tau);
}

class Movement {
  constructor() { this.x = 0.0; this.v = 0.0; }

  reset() { this.x = 0.0; this.v = 0.0; }

  /** RK4 on x' = v, v' = wn^2 (u - x) - 2 z wn v; dry friction after the step. */
  step(h, u, zeta, omegaN, friction = 0.0) {
    if (h <= 0.0) return;
    const { x, v } = this;
    const k1 = field(x, v, u, zeta, omegaN);
    const k2 = field(x + 0.5 * h * k1[0], v + 0.5 * h * k1[1], u, zeta, omegaN);
    const k3 = field(x + 0.5 * h * k2[0], v + 0.5 * h * k2[1], u, zeta, omegaN);
    const k4 = field(x + h * k3[0], v + h * k3[1], u, zeta, omegaN);
    this.x += h / 6.0 * (k1[0] + 2.0 * k2[0] + 2.0 * k3[0] + k4[0]);
    this.v += h / 6.0 * (k1[1] + 2.0 * k2[1] + 2.0 * k3[1] + k4[1]);

    if (friction <= 0.0) return;

    const decrement = friction * h;
    if (Math.abs(this.v) <= decrement) {
      if (Math.abs(omegaN * omegaN * (u - this.x)) <= friction) this.v = 0.0;
      else this.v -= copysign(decrement, this.v);
    } else {
      this.v -= copysign(decrement, this.v);
    }
  }
}

class Follower {
  constructor() { this.x = 0.0; }
  reset() { this.x = 0.0; }
  step(h, u, riseTau, fallTau) {
    if (h <= 0.0) return;
    this.x += (u - this.x) * pole(h, u >= this.x ? riseTau : fallTau);
  }
}

class Hold {
  constructor() { this.db = -120.0; this.since = 0.0; }
  reset() { this.db = -120.0; this.since = 0.0; }
  step(h, db, holdSeconds, decayDbPerSecond) {
    if (h <= 0.0) return;
    if (db >= this.db) {
      this.db = db;
      this.since = 0.0;
      return;
    }
    this.since += h;
    if (this.since > holdSeconds) this.db = Math.max(db, this.db - decayDbPerSecond * h);
  }
}

class Eye {
  constructor() { this.movement = new Movement(); this.shown = 0.0; this.warm = 0.0; }
  reset() { this.movement.reset(); this.shown = 0.0; this.warm = 0.0; }

  step(h, u, zeta, omegaN, persistenceTau, warmTau, friction = 0.0) {
    if (h <= 0.0) return;
    this.movement.step(h, u, zeta, omegaN, friction);
    const c = persistenceTau > 0.0 ? 1.0 - Math.exp(-h / persistenceTau) : 1.0;
    this.shown += (this.movement.x - this.shown) * c;
    this.warm += (1.0 - this.warm) * (1.0 - Math.exp(-h / Math.max(1e-6, warmTau)));
  }

  shadowDegrees() {
    const d = Math.min(Math.max(this.shown, 0.0), 1.0);
    return kEyeOpenDegrees * (1.0 - d);
  }

  overlapDegrees() {
    const over = this.shown - 1.0;
    if (over <= 0.0) return 0.0;
    return Math.min(kEyeOpenDegrees, kEyeOpenDegrees * over);
  }
}

//===========================================================================
// PORT — source/meter/Engine.{h,cpp}.
//===========================================================================

const kEngineRate = 4800.0;
const kMaxFrameSeconds = 0.25;
const kMaxDeadBand = 0.03;
const kSilence = 1e-7;
const kWarmTau = 4.0;
const kMaxChannels = 2;

const Db = (amplitude) => 20.0 * Math.log10(Math.max(amplitude, kSilence));
const Amp = (db) => Math.pow(10.0, db / 20.0);

function scaleFor(type) {
  switch (type) {
    case MeterType.Vu: return { linearInAmplitude: true, bottomDb: -20.0, topDb: 3.0 };
    case MeterType.Ppm: return { linearInAmplitude: false, bottomDb: -12.0, topDb: 12.0 };
    case MeterType.Bargraph: return { linearInAmplitude: false, bottomDb: -kBargraphStepDb * kBargraphSteps, topDb: 0.0 };
    default: return { linearInAmplitude: false, bottomDb: -40.0, topDb: 0.0 };
  }
}

function deflectionFor(scale, amplitudeRatio) {
  if (scale.linearInAmplitude) return amplitudeRatio / Amp(scale.topDb);
  const db = Db(amplitudeRatio);
  return (db - scale.bottomDb) / (scale.topDb - scale.bottomDb);
}

function resolve(s) {
  const r = {};
  if (s.standard) {
    r.vuZeta = VU_DAMPING;
    r.vuOmegaUp = VU_NATURAL_FREQUENCY;
    r.vuOmegaDown = r.vuOmegaUp;
    r.ppmRiseTau = PPM_RISE_TAU;
    r.ppmFallTau = PPM_FALL_TAU;
    r.eyeZeta = r.vuZeta;
    r.eyeOmegaUp = r.vuOmegaUp;
    r.eyeOmegaDown = r.vuOmegaUp;
  } else {
    r.vuZeta = dampingForOvershoot(s.overshoot);
    r.vuOmegaUp = naturalFrequencyFor(kVu99, Math.max(1e-3, s.riseSeconds), r.vuZeta);
    r.vuOmegaDown = naturalFrequencyFor(kVu99, Math.max(1e-3, s.fallSeconds), r.vuZeta);
    r.ppmRiseTau = Math.max(1e-5, s.riseSeconds) / Math.log(100.0);
    r.ppmFallTau = Math.max(1e-4, s.fallSeconds) / Math.log(10.0);
    r.eyeZeta = r.vuZeta;
    r.eyeOmegaUp = r.vuOmegaUp;
    r.eyeOmegaDown = r.vuOmegaDown;
  }
  r.holdSeconds = Math.max(0.0, s.holdSeconds);
  r.holdDecayDbPerSecond = Math.max(0.0, s.holdDecayDbPerSecond);
  r.persistenceTau = Math.max(0.0, s.persistenceSeconds);
  r.warmTau = kWarmTau;
  r.frictionDeadBand = kMaxDeadBand * Math.min(Math.max(s.wear, 0.0), 1.0);
  return r;
}

const newChannelState = () => ({
  inputDb: -120.0,
  deflection: 0.0,
  ppmDb: -120.0,
  holdDb: -120.0,
  holdDeflection: 0.0,
  litSegments: 0,
  segment: new Array(kBargraphSteps).fill(false),
  eyeShadowDeg: kEyeOpenDegrees,
  eyeOverlapDeg: 0.0,
  eyeWarm: 0.0,
});

class Engine {
  constructor() {
    this.models = [];
    for (let i = 0; i < kMaxChannels; i += 1) {
      this.models.push({ vu: new Movement(), ppm: new Follower(), hold: new Hold(), eye: new Eye(), state: newChannelState() });
    }
    this.settings = null;
    this.resolved = null;
    this.started = false;
    this.lastHost = 0.0;
    this.residual = 0.0;
    this.engineSeconds = 0.0;
    this.lastSteps = 0;
  }

  setSettings(settings) {
    this.settings = settings;
    this.resolved = resolve(settings);
  }

  channel(index) {
    return this.models[index]?.state ?? newChannelState();
  }

  stepAll(h, amplitudeRatio) {
    const vuScale = scaleFor(MeterType.Vu);
    const eyeScale = scaleFor(MeterType.Eye);
    const r = this.resolved;

    // Every channel, not the Count the operator has chosen (v0.1.1).
    for (let i = 0; i < kMaxChannels; i += 1) {
      const c = this.models[i];
      const a = amplitudeRatio[i];
      {
        const u = deflectionFor(vuScale, a);
        const omega = u >= c.vu.x ? r.vuOmegaUp : r.vuOmegaDown;
        c.vu.step(h, u, r.vuZeta, omega, r.frictionDeadBand * omega * omega);
      }
      c.ppm.step(h, a, r.ppmRiseTau, r.ppmFallTau);
      {
        const u = deflectionFor(eyeScale, a);
        const omega = u >= c.eye.movement.x ? r.eyeOmegaUp : r.eyeOmegaDown;
        c.eye.step(h, u, r.eyeZeta, omega, r.persistenceTau, r.warmTau, r.frictionDeadBand * omega * omega);
      }
      c.hold.step(h, Db(c.ppm.x), r.holdSeconds, r.holdDecayDbPerSecond);
    }
  }

  frame(hostSeconds, amplitude, inputs) {
    inputs = Math.min(Math.max(inputs, 1), kMaxChannels);
    const s = this.settings;

    const offsetDb = -s.referenceDbfs + s.trimDb;
    const ratio = [0.0, 0.0];
    for (let i = 0; i < kMaxChannels; i += 1) {
      ratio[i] = Amp(Db(Math.max(0.0, amplitude[Math.min(i, inputs - 1)])) + offsetDb);
    }

    let dt = 0.0;
    if (this.started) {
      dt = hostSeconds - this.lastHost;
      if (dt < 0.0) dt = 0.0;
      if (dt > kMaxFrameSeconds) dt = kMaxFrameSeconds;
    } else {
      // Frame one: adopt the host's origin, step nothing.
      this.started = true;
    }
    this.lastHost = hostSeconds;

    const h = 1.0 / kEngineRate;
    this.residual += dt;
    let steps = Math.floor(this.residual * kEngineRate);
    if (steps < 0) steps = 0;
    const maxSteps = Math.trunc(kMaxFrameSeconds * kEngineRate) + 2;
    if (steps > maxSteps) steps = maxSteps;
    this.residual -= steps * h;
    this.lastSteps = steps;

    for (let n = 0; n < steps; n += 1) {
      this.stepAll(h, ratio);
      this.engineSeconds += h;
    }

    const scale = scaleFor(s.type);
    const barScale = scaleFor(MeterType.Bargraph);
    const r = this.resolved;

    for (let i = 0; i < kMaxChannels; i += 1) {
      const c = this.models[i];
      const st = c.state;

      st.inputDb = Db(ratio[i]);
      st.ppmDb = Db(c.ppm.x);

      switch (s.type) {
        case MeterType.Vu: st.deflection = c.vu.x; break;
        case MeterType.Ppm:
        case MeterType.Bargraph: st.deflection = deflectionFor(scale, c.ppm.x); break;
        default: st.deflection = c.eye.shown; break;
      }
      st.deflection = Math.max(0.0, st.deflection);

      st.holdDb = c.hold.db;
      st.holdDeflection = r.holdSeconds > 0.0
        ? Math.min(Math.max((c.hold.db - barScale.bottomDb) / (barScale.topDb - barScale.bottomDb), 0.0), 1.0)
        : 0.0;

      st.litSegments = 0;
      for (let k = 0; k < kBargraphSteps; k += 1) {
        const threshold = barScale.topDb - kBargraphStepDb * (kBargraphSteps - 1 - k);
        st.segment[k] = st.ppmDb >= threshold;
        if (st.segment[k]) st.litSegments = k + 1;
      }

      st.eyeShadowDeg = c.eye.shadowDegrees();
      st.eyeOverlapDeg = c.eye.overlapDegrees();
      st.eyeWarm = c.eye.warm;
    }
  }
}

//===========================================================================
// PORT — source/Controls.h. 0..1 host values to physical units. The plugin
// computes these in float, so each result is rounded through Math.fround.
//===========================================================================

const f32 = Math.fround;
const ReferenceDbfs = (v) => f32(-40.0 + 40.0 * f32(v));
const TrimDb = (v) => f32(-12.0 + 24.0 * f32(v));
const RiseSeconds = (v) => f32(0.02 * Math.pow(100.0, f32(v)));
const FallSeconds = (v) => f32(0.05 * Math.pow(400.0, f32(v)));
const OvershootFraction = (v) => f32(0.001 * Math.pow(500.0, f32(v)));
const PeakHoldSeconds = (v) => f32(10.0 * f32(v) * f32(v));
const HoldDecayDbPerSecond = (v) => f32(48.0 * f32(v));
const PersistenceSeconds = (v) => f32(0.5 * f32(v));
const SizeFraction = (v) => f32(0.15 + 0.85 * f32(v));
const Offset = (v) => f32((f32(v) - 0.5) * 2.0);
const RotationRadians = (v) => f32((f32(v) - 0.5) * 2.0 * PI);

const kReferenceDefault = 0.55;
const kSensitivityDefault = 0.5;
const kRiseDefault = 0.5880456;
const kFallDefault = 0.6718477;
const kOvershootDefault = 0.4064180;
const kPeakHoldDefault = 0.3872983;
const kHoldDecayDefault = 0.25;
const kPersistenceDefault = 0.12;
const kSizeDefault = 0.55;

const kBinLawCount = 2;
const kScaleStyleCount = 4;

//===========================================================================
// PORT — source/Needle.cpp: ToOption, CurrentSettings, InputLevel,
// AdvanceEngine and BuildFrame.
//===========================================================================

function toOption(v, count) {
  if (count <= 1) return 0;
  const i = (v <= 1.0 && count > 2 && v !== Math.floor(v))
    ? Math.trunc(v * (count - 1) + 0.5)
    : Math.trunc(v + 0.5);
  return Math.min(Math.max(i, 0), count - 1);
}

const isOn = (v) => v > 0.5;

function currentSettings(p) {
  return {
    type: toOption(p.get('type'), kMeterTypeCount),
    channels: toOption(p.get('count'), 2) + 1,
    referenceDbfs: ReferenceDbfs(p.get('reference')),
    trimDb: TrimDb(p.get('sensitivity')),
    standard: isOn(p.get('standard')),
    riseSeconds: RiseSeconds(p.get('rise')),
    fallSeconds: FallSeconds(p.get('fall')),
    overshoot: OvershootFraction(p.get('overshoot')),
    holdSeconds: PeakHoldSeconds(p.get('peakHold')),
    holdDecayDbPerSecond: HoldDecayDbPerSecond(p.get('holdDecay')),
    persistenceSeconds: PersistenceSeconds(p.get('persistence')),
    wear: f32(p.get('wear')),
  };
}

/** PORT — Audio::LevelFromSpectrum. sqrt(sum b^2) for Magnitude, sqrt(sum b) for Power. */
function levelFromSpectrum(bins, power) {
  let total = 0.0;
  for (let i = 0; i < bins.length; i += 1) {
    const b = bins[i];
    if (!(b > 0.0)) continue;
    total += power ? b : b * b;
  }
  return f32(Math.sqrt(total));
}

// A VU scale as it is printed, and a BBC PPM's seven marks. From Needle.cpp.
const VU_MARKS = [
  [-20.0, '20'], [-10.0, '10'], [-7.0, '7'], [-5.0, '5'], [-3.0, '3'],
  [-2.0, null], [-1.0, null], [0.0, '0'], [1.0, null], [2.0, null], [3.0, '3'],
];
const PPM_MARKS = [
  [-12.0, '1'], [-8.0, '2'], [-4.0, '3'], [0.0, '4'], [4.0, '5'], [8.0, '6'], [12.0, '7'],
];

const kMaxMarks = 16;
const kMaxText = 64;
const kPiF = f32(PI);

function appendText(out, s) {
  for (const ch of s) {
    const c = ch.charCodeAt(0);
    out.push(c >= 32 && c < 127 ? c : 63);
  }
}

const newUnit = () => ({
  centre: [0, 0], rotation: 0, halfW: 0, halfH: 0, corner: 0,
  pivot: [0, 0], arcRadius: 0, arcHalfAngle: 0, needleLen: 0, needleWidth: 0, needleAngle: 0, redFrom: 10,
  seg: Array.from({ length: kBargraphSteps }, () => [0, 0, 0, 0]), segLit: 0, hold: [0, 0, 0, 0],
  eyeRadius: 0, eyeShadow: 0, eyeOverlap: 0, eyeWarm: 0,
});

function buildFrame(p, engine, width, height) {
  const settings = currentSettings(p);
  const f = {
    width, height,
    type: settings.type,
    count: settings.channels,
    style: toOption(p.get('scaleStyle'), kScaleStyleCount),
    face: [p.get('faceR'), p.get('faceG'), p.get('faceB')],
    needle: [p.get('needleR'), p.get('needleG'), p.get('needleB')],
    back: [p.get('backR'), p.get('backG'), p.get('backB')],
    backAlpha: p.get('background'),
    lamp: p.get('lamp'),
    glass: p.get('glass'),
    wear: p.get('wear'),
    mix: p.get('mix'),
    unit: [newUnit(), newUnit()],
    marks: [],
    text: [],
    legendOffset: 0,
    legendLength: 0,
    legendPos: [0, 0],
    legendScale: 1,
    markTextScale: 1,
  };

  const minDim = Math.min(width, height);
  const S = f32(SizeFraction(p.get('size')) * minDim * 0.5);
  const rot = RotationRadians(p.get('rotation'));

  let aspect = 1.55;
  if (settings.type === MeterType.Bargraph) aspect = 0.34;
  else if (settings.type === MeterType.Eye) aspect = 1.0;

  const halfH = S;
  const halfW = S * aspect;

  const assemblyX = width * 0.5 + Offset(p.get('posX')) * minDim * 0.5;
  const assemblyY = height * 0.5 + Offset(p.get('posY')) * minDim * 0.5;
  const cs = Math.cos(rot);
  const sn = Math.sin(rot);
  const gap = halfW * 0.14;

  const scale = scaleFor(settings.type);

  for (let u = 0; u < f.count; u += 1) {
    const U = f.unit[u];
    // Side by side ALONG THE LOCAL X AXIS, so a rotated pair tilts as one panel.
    const dx = f.count === 1 ? 0.0 : (u === 0 ? -(halfW + gap) : halfW + gap);
    U.centre[0] = assemblyX + dx * cs;
    U.centre[1] = assemblyY + dx * sn;
    U.rotation = rot;
    U.halfW = halfW;
    U.halfH = halfH;

    const st = engine.channel(u);

    if (settings.type === MeterType.Bargraph) {
      U.corner = S * 0.08;
      const colHalfW = halfW * 0.64;
      const top = -halfH * 0.90;
      const span = halfH * 1.80;
      const pitch = span / kBargraphSteps;
      const gapFrac = 0.22;
      for (let k = 0; k < kBargraphSteps; k += 1) {
        const row = kBargraphSteps - 1 - k;
        U.seg[k][0] = -colHalfW;
        U.seg[k][1] = top + row * pitch + pitch * gapFrac * 0.5;
        U.seg[k][2] = colHalfW * 2.0;
        U.seg[k][3] = pitch * (1.0 - gapFrac);
      }
      U.segLit = st.litSegments;

      if (st.holdDeflection > 0.0) {
        const hh = Math.max(2.0, pitch * 0.22);
        const y = top + (1.0 - st.holdDeflection) * span;
        U.hold[0] = -colHalfW;
        U.hold[1] = Math.min(Math.max(y - hh * 0.5, top), top + span - hh);
        U.hold[2] = colHalfW * 2.0;
        U.hold[3] = hh;
      }
    } else if (settings.type === MeterType.Eye) {
      U.corner = S * 0.18;
      U.eyeRadius = S * 0.72;
      U.eyeShadow = st.eyeShadowDeg * 0.5 * kPiF / 180.0;
      U.eyeOverlap = st.eyeOverlapDeg * 0.5 * kPiF / 180.0;
      U.eyeWarm = st.eyeWarm;
    } else {
      // A shallow arc on a long radius: the pivot is below the glass.
      U.corner = S * 0.09;
      U.pivot[0] = 0.0;
      U.pivot[1] = S * 2.35;
      U.arcRadius = S * 2.80;
      U.arcHalfAngle = 27.0 * kPiF / 180.0;
      U.needleLen = U.arcRadius * 0.970;
      U.needleWidth = Math.max(3.0, S * 0.026);

      const d = Math.min(Math.max(st.deflection, -0.02), 1.035);
      U.needleAngle = (2.0 * d - 1.0) * U.arcHalfAngle;

      if (settings.type === MeterType.Vu) {
        const zero = deflectionFor(scale, 1.0);
        U.redFrom = (2.0 * zero - 1.0) * U.arcHalfAngle;
      } else {
        U.redFrom = 10.0; // a BBC PPM has no red band
      }
    }
  }

  if (settings.type === MeterType.Vu || settings.type === MeterType.Ppm) {
    const specs = settings.type === MeterType.Vu ? VU_MARKS : PPM_MARKS;
    const alpha = f.unit[0].arcHalfAngle;
    for (let m = 0; m < specs.length && f.marks.length < kMaxMarks; m += 1) {
      const [db, label] = specs[m];
      const where = deflectionFor(scale, Math.pow(10.0, db / 20.0));
      const mark = {
        angle: (2.0 * where - 1.0) * alpha,
        lengthFrac: label !== null ? 1.0 : 0.55,
        red: settings.type === MeterType.Vu && db >= 0.0 ? 1 : 0,
        textOffset: 0,
        textLength: 0,
      };
      if (label !== null && f.text.length + label.length <= kMaxText) {
        mark.textOffset = f.text.length;
        appendText(f.text, label);
        mark.textLength = f.text.length - mark.textOffset;
      }
      f.marks.push(mark);
    }

    const legend = settings.type === MeterType.Vu ? 'VU' : 'PPM';
    if (f.text.length + legend.length <= kMaxText) {
      f.legendOffset = f.text.length;
      appendText(f.text, legend);
      f.legendLength = f.text.length - f.legendOffset;
    }
    f.legendScale = Math.max(1, Math.round(S / 42.0));
    f.markTextScale = Math.max(1, Math.round(S / 64.0));

    const lw = f.legendLength * 6 * f.legendScale - f.legendScale;
    f.legendPos[0] = Math.floor(-lw * 0.5);
    f.legendPos[1] = Math.floor(S * 0.42);
  }

  return f;
}

//===========================================================================
// PORT — source/Font.cpp. The 5x7 table, as pictures, carried from graticule.
// Codes 32..127; the texture is indexed by ASCII code directly.
//===========================================================================

const GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 32 space
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'], // 33 !
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'], // 34 "
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'], // 35 #
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'], // 36 $
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'], // 37 %
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'], // 38 &
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'], // 39 '
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'], // 40 (
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'], // 41 )
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'], // 42 *
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'], // 43 +
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // 44 ,
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'], // 45 -
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'], // 46 .
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'], // 47 /
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // 48 0
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 49 1
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'], // 50 2
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'], // 51 3
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'], // 52 4
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'], // 53 5
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'], // 54 6
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'], // 55 7
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'], // 56 8
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'], // 57 9
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // 58 :
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'], // 59 ;
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'], // 60 <
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'], // 61 =
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'], // 62 >
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'], // 63 ?
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'], // 64 @
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 65 A
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'], // 66 B
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'], // 67 C
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'], // 68 D
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'], // 69 E
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'], // 70 F
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'], // 71 G
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 72 H
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 73 I
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'], // 74 J
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'], // 75 K
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'], // 76 L
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'], // 77 M
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'], // 78 N
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 79 O
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'], // 80 P
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'], // 81 Q
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 82 R
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'], // 83 S
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 84 T
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 85 U
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 86 V
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'], // 87 W
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'], // 88 X
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'], // 89 Y
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'], // 90 Z
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'], // 91 [
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'], // 92 backslash
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'], // 93 ]
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'], // 94 ^
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'], // 95 _
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'], // 96 `
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 97 a
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'], // 98 b
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'], // 99 c
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 100 d
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 101 e
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'], // 102 f
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 103 g
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 104 h
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 105 i
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'], // 106 j
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'], // 107 k
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 108 l
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'], // 109 m
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 110 n
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 111 o
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 112 p
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'], // 113 q
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 114 r
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'], // 115 s
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'], // 116 t
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'], // 117 u
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 118 v
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'], // 119 w
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'], // 120 x
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'], // 121 y
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'], // 122 z
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'], // 123 {
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 124 |
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'], // 125 }
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'], // 126 ~
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 127 del
];

const FONT_FIRST = 32;
const FONT_WIDTH = 5;
const FONT_HEIGHT = 7;
const FONT_TEXTURE_WIDTH = FONT_WIDTH * 128;

/** PORT — font::Texture(): kWidth*128 by kHeight, R8, glyph c at column c*kWidth. */
function fontTexture() {
  const out = new Uint8Array(FONT_TEXTURE_WIDTH * FONT_HEIGHT);
  for (let code = FONT_FIRST; code < 128; code += 1) {
    const glyph = GLYPHS[code - FONT_FIRST];
    for (let y = 0; y < FONT_HEIGHT; y += 1) {
      for (let x = 0; x < FONT_WIDTH; x += 1) {
        if (glyph[y][x] === '#') out[y * FONT_TEXTURE_WIDTH + code * FONT_WIDTH + x] = 255;
      }
    }
  }
  return out;
}

//===========================================================================
// NOT THE PLUGIN — the test signal, and the 64-bin spectrum made from it.
//
// This is the one part of the page that stands in for the host rather than
// porting the plugin. Resolume's FFT is unknown (window, normalisation, bin
// law, frequency layout), so nothing here pretends to be it. What it does:
//
//   1. The signal is an ENVELOPE in dBFS — the level the plugin's own level
//      law will read, RMS, 1.0 = full scale — plus a spectral shape.
//   2. For each frame the page integrates the envelope's power over the frame's
//      own interval, because one spectrum per frame is also the plugin's input
//      resolution: a 5 ms burst inside a 16.7 ms frame arrives averaged, here as
//      in Resolume (AGENTS.md, "one spectrum per frame").
//   3. That power is written into 64 bins as MAGNITUDES: a tone into one bin
//      (bin 2, if the bins were 375 Hz wide linear to 24 kHz — the plugin sums
//      every bin, so where the tone lands cannot matter to it), pink noise as
//      1/f across all 64 with an exponential scatter on each bin's power, which
//      is what a short FFT of noise does from one frame to the next.
//===========================================================================

const SAMPLE_STEP = 1e-4; // envelope integration step, seconds
const TEST_TONE_BIN = 2;
const BINS = 64;

const dbfs = (db) => Math.pow(10, db / 20);

/** Test signals: `level(t)` is RMS amplitude, `shape` is 'tone' or 'pink'. */
const SIGNALS = [
  {
    id: 'step0',
    name: '0 VU step: 1 kHz at −18 dBFS, 2 s on / 2 s off',
    hint: 'A tone at the reference level, switched on and off. The VU should reach 99 % of 0 VU in 300 ms and overshoot by 1.25 %; the PPM should rise in a few milliseconds and fall 20 dB in 2.8 s.',
    shape: 'tone',
    level: (t) => ((t % 4) < 2 ? dbfs(-18) : 0),
  },
  {
    id: 'step20',
    name: '−20 dB step: 1 kHz at −38 dBFS, 2 s on / 2 s off',
    hint: 'Twenty decibels under the reference. On a VU, whose scale is linear in voltage, that is the −20 mark at the bottom of the arc; on a PPM, linear in dB, it is below mark 1.',
    shape: 'tone',
    level: (t) => ((t % 4) < 2 ? dbfs(-38) : 0),
  },
  {
    id: 'tone',
    name: '1 kHz tone at −18 dBFS, steady',
    hint: 'The alignment tone. At the default Reference Level every meter reads its zero: 0 VU, PPM 4, the top LED, the eye just shut.',
    shape: 'tone',
    level: () => dbfs(-18),
  },
  {
    id: 'burst5',
    name: '5 ms bursts at −18 dBFS, one a second',
    hint: 'The PPM integration figure is about a 5 ms burst — but the plugin gets one spectrum per video frame, so a 5 ms burst inside a 16.7 ms frame arrives already averaged. That is the plugin’s input resolution, reproduced here, not a fault in the page.',
    shape: 'tone',
    level: (t) => ((t % 1) < 0.005 ? dbfs(-18) : 0),
  },
  {
    id: 'ladder',
    name: 'Tone bursts at −18 dBFS: 5, 20, 50, 150 and 500 ms',
    hint: 'One burst of each length, 1.5 s apart. Watch the PPM and the bargraph reach further on each, and the VU barely notice the short ones.',
    shape: 'tone',
    level: (t) => {
      const lengths = [0.005, 0.02, 0.05, 0.15, 0.5];
      const phase = t % 7.5;
      const k = Math.floor(phase / 1.5);
      return phase - k * 1.5 < lengths[k] ? dbfs(-18) : 0;
    },
  },
  {
    id: 'pink',
    name: 'Pink noise at −18 dBFS RMS, steady',
    hint: 'Noise, spread 1/f over the 64 bins with the frame-to-frame scatter a short FFT has. The needle wanders because the level of a 17 ms block of noise really does.',
    shape: 'pink',
    level: () => dbfs(-18),
  },
  {
    id: 'ramp',
    name: 'Pink noise ramping −60 to −12 dBFS over 8 s, then 2 s silence',
    hint: 'A slow climb through the whole range: every LED of the ladder in turn, 3 dB apart, and the magic eye closing and then overlapping past the reference.',
    shape: 'pink',
    level: (t) => {
      const phase = t % 10;
      return phase < 8 ? dbfs(-60 + 48 * (phase / 8)) : 0;
    },
  },
  {
    id: 'silence',
    name: 'Silence',
    hint: 'Every bin zero — what the plugin sees on a layer with no audio. The meters fall back to rest at their own rates.',
    shape: 'tone',
    level: () => 0,
  },
];

// Pink weights: power per bin proportional to 1/f at the bin centre.
const PINK_WEIGHTS = (() => {
  const w = new Float64Array(BINS);
  let sum = 0;
  for (let i = 0; i < BINS; i += 1) { w[i] = 1 / (i + 0.5); sum += w[i]; }
  for (let i = 0; i < BINS; i += 1) w[i] /= sum;
  return w;
})();

/** Mean power of the envelope over [t0, t1], by the midpoint rule. */
function blockPower(signal, t0, t1) {
  const span = t1 - t0;
  if (!(span > 0)) {
    const a = signal.level(t1);
    return a * a;
  }
  const n = Math.max(1, Math.ceil(span / SAMPLE_STEP));
  const h = span / n;
  let total = 0;
  for (let k = 0; k < n; k += 1) {
    const a = signal.level(t0 + (k + 0.5) * h);
    total += a * a;
  }
  return total / n;
}

/** A small, fast PRNG for the noise scatter. */
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function writeSpectrum(bins, signal, power, random) {
  bins.fill(0);
  if (!(power > 0)) return;
  if (signal.shape === 'tone') {
    bins[TEST_TONE_BIN] = f32(Math.sqrt(power));
    return;
  }
  for (let i = 0; i < BINS; i += 1) {
    // |X|^2 of complex Gaussian noise is exponential with the bin's mean power.
    const e = -Math.log(1 - random());
    bins[i] = f32(Math.sqrt(power * PINK_WEIGHTS[i] * e));
  }
}

//===========================================================================
// PORT — source/Render.cpp's Renderer::InitGL and Renderer::Draw.
//===========================================================================

function createRenderer(gl) {
  const program = new Program(gl, VERTEX_SHADER, FRAGMENT_SHADER, 'needle', { attribs: {} });

  // No vertex buffer: the corners come from gl_VertexID, so only a VAO is bound.
  const vao = gl.createVertexArray();

  const font = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, font);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, FONT_TEXTURE_WIDTH, FONT_HEIGHT, 0, gl.RED, gl.UNSIGNED_BYTE, fontTexture());
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const engine = new Engine();
  const bins = new Float32Array(BINS);
  let random = mulberry32(0x4e443031); // "ND01"
  let lastTime = null;
  let lastSignal = null;

  const loc = (name) => program.location(`${name}[0]`) ?? program.location(name);

  function draw(f) {
    gl.viewport(0, 0, f.width, f.height);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.BLEND);
    program.use();

    const p = program.program;
    const i1 = (name, v) => { const l = program.location(name); if (l !== null) gl.uniform1i(l, v); };
    const f1 = (name, v) => { const l = program.location(name); if (l !== null) gl.uniform1f(l, v); };

    { const l = program.location('uSize'); if (l !== null) gl.uniform2i(l, f.width, f.height); }
    i1('uType', f.type);
    i1('uCount', f.count);
    i1('uStyle', f.style);
    program.set('uFace', f.face);
    program.set('uNeedleCol', f.needle);
    program.set('uBackCol', f.back);
    f1('uBackA', f.backAlpha);
    f1('uLamp', f.lamp);
    f1('uGlass', f.glass);
    f1('uWear', f.wear);
    f1('uMix', f.mix);

    const a = new Float32Array(8);
    const b = new Float32Array(8);
    const c = new Float32Array(8);
    const d = new Float32Array(8);
    const e = new Float32Array(8);
    const seg = new Float32Array(80);
    const hold = new Float32Array(8);
    for (let u = 0; u < 2; u += 1) {
      const U = f.unit[u];
      a.set([U.centre[0], U.centre[1], U.rotation, U.corner], u * 4);
      b.set([U.halfW, U.halfH, U.pivot[0], U.pivot[1]], u * 4);
      c.set([U.arcRadius, U.arcHalfAngle, U.needleLen, U.needleWidth], u * 4);
      d.set([U.needleAngle, U.redFrom, U.eyeRadius, U.eyeShadow], u * 4);
      e.set([U.eyeOverlap, U.eyeWarm, U.segLit, U.hold[2] > 0.0 ? 1.0 : 0.0], u * 4);
      for (let k = 0; k < kBargraphSteps; k += 1) seg.set(U.seg[k], (u * kBargraphSteps + k) * 4);
      hold.set(U.hold, u * 4);
    }
    const v4 = (name, data) => { const l = loc(name); if (l !== null) gl.uniform4fv(l, data); };
    v4('uUnitA', a);
    v4('uUnitB', b);
    v4('uUnitC', c);
    v4('uUnitD', d);
    v4('uUnitE', e);
    v4('uSeg', seg);
    v4('uHoldRect', hold);

    const n = Math.min(f.marks.length, kMaxMarks);
    const mark = new Float32Array(kMaxMarks * 4);
    const span = new Int32Array(kMaxMarks * 2);
    for (let m = 0; m < n; m += 1) {
      const mk = f.marks[m];
      mark.set([mk.angle, mk.lengthFrac, mk.red, 0], m * 4);
      span[m * 2] = mk.textOffset;
      span[m * 2 + 1] = mk.textLength;
    }
    i1('uMarkCount', n);
    v4('uMark', mark);
    { const l = loc('uMarkText'); if (l !== null) gl.uniform2iv(l, span); }
    const text = new Int32Array(kMaxText);
    for (let i = 0; i < kMaxText; i += 1) text[i] = i < f.text.length ? f.text[i] : 32;
    { const l = loc('uText'); if (l !== null) gl.uniform1iv(l, text); }

    { const l = program.location('uLegend'); if (l !== null) gl.uniform4i(l, f.legendOffset, f.legendLength, Math.max(1, f.legendScale), Math.max(1, f.markTextScale)); }
    program.set('uLegendPos', [f.legendPos[0], f.legendPos[1]]);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, font);
    i1('uFont', 0);

    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
    gl.bindTexture(gl.TEXTURE_2D, null);
    gl.useProgram(null);
    void p;
  }

  return {
    engine,
    bins,
    render({ params, width, height, time, variant }) {
      const signal = SIGNALS.find((s) => s.id === variant) ?? SIGNALS[0];

      // The host's spectrum for this frame. A repeated clock (paused, and a
      // control moved) re-sends the last one, as a host re-sends its buffer; a
      // clock that went backwards (Restart) starts a fresh block.
      if (lastTime === null || time < lastTime || signal !== lastSignal) {
        writeSpectrum(bins, signal, blockPower(signal, time, time), random);
      } else if (time > lastTime) {
        writeSpectrum(bins, signal, blockPower(signal, lastTime, time), random);
      }
      lastTime = time;
      lastSignal = signal;

      // PORT — NeedlePlugin::AdvanceEngine. Both meters are fed whatever Count says.
      engine.setSettings(currentSettings(params));
      const power = toOption(params.get('binLaw'), kBinLawCount) === 1;
      const level = levelFromSpectrum(bins, power);
      engine.frame(time, [level, level], kMaxChannels);

      draw(buildFrame(params, engine, width, height));
      readout(level, engine);
    },
    reseed() { random = mulberry32(0x4e443031); },
  };
}

//===========================================================================
// The page.
//===========================================================================

const pct = (v) => `${(v * 100).toFixed(0)}%`;
const signedDb = (db) => `${db >= 0 ? '+' : '−'}${Math.abs(db).toFixed(1)} dB`;
const dur = (s) => (s <= 0 ? 'off' : s < 1 ? `${(s * 1000).toFixed(0)} ms` : `${s.toFixed(2)} s`);

const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const std = (id, name, def, group, extra = {}) =>
  ({ id, name, type: 'standard', default: def, group, ...(typeof extra === 'string' ? { hint: extra } : extra) });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const NOT_AUDIO =
  'There is no audio on this page: the level comes from the test signal chosen under the picture, shaped into a spectrum by this page — not by Resolume’s FFT, whose calibration is unknown.';

const PARAMS = [
  // -- Meter --------------------------------------------------------------
  opt('type', 'Type', ['VU', 'PPM', 'Bargraph', 'Magic Eye'], 0, 'Meter',
    'Four instruments on one engine. VU: ANSI C16.5’s movement, on a scale linear in voltage. PPM: IEC 60268-10 type II, the BBC meter, linear in dB. Bargraph: the LM3915’s ten 3 dB steps off the PPM’s detector. Magic Eye: a 6U5’s shadow, which closes at the reference and overlaps past it.'),
  opt('count', 'Count', ['Mono', 'Stereo Pair'], 0, 'Meter',
    'How many meters are DRAWN, never how many run: both are integrated every frame. FFGL has one spectrum to give, so a pair shows the same programme twice.'),
  std('reference', 'Reference Level', kReferenceDefault, 'Meter', {
    display: (v) => `${ReferenceDbfs(v).toFixed(1)} dBFS`,
    hint: `The dBFS the meter calls zero, −40 to 0. The default is −18 dBFS, EBU R68’s alignment level, and the test signals here are built around it. ${NOT_AUDIO}`,
  }),
  std('sensitivity', 'Sensitivity', kSensitivityDefault, 'Meter', {
    display: (v) => signedDb(TrimDb(v)),
    hint: `Input trim, ±12 dB, because the absolute calibration of a host’s spectrum is not known. ${NOT_AUDIO}`,
  }),
  opt('binLaw', 'Bin Law', ['Magnitude', 'Power'], 0, 'Meter',
    'Whether a host bin is a magnitude or a power — genuinely unknown for Resolume, so it is a switch. This page writes its test spectrum as MAGNITUDES, so Magnitude reads the signal at the level it was generated at and Power shows what the other assumption does to the same numbers.'),
  bool('standard', 'Standard', 1, 'Meter',
    'On: every ballistic constant comes from the standards and Rise, Fall and Overshoot are not read at all. Off: Free, deliberately unphysical — a rise that differs from a fall is not a mass on a spring.'),

  // -- Ballistics ---------------------------------------------------------
  std('rise', 'Rise', kRiseDefault, 'Ballistics', {
    display: (v) => dur(RiseSeconds(v)),
    hint: 'Free only. Time to 99 % of a rising step, 20 ms to 2 s. The default, 300 ms, is ANSI C16.5’s.',
  }),
  std('fall', 'Fall', kFallDefault, 'Ballistics', {
    display: (v) => dur(FallSeconds(v)),
    hint: 'Free only. A 99 % time for the movements, a 20 dB fall-back time for the detectors, 50 ms to 20 s. The default, 2.8 s, is IEC 60268-10 type II’s.',
  }),
  std('overshoot', 'Overshoot', kOvershootDefault, 'Ballistics', {
    display: (v) => `${(OvershootFraction(v) * 100).toFixed(2)}%`,
    hint: 'Free only. 0.1 % to 50 %, logarithmic; it sets the damping. The default, 1.25 %, is the middle of ANSI C16.5’s band.',
  }),
  std('peakHold', 'Peak Hold', kPeakHoldDefault, 'Ballistics', {
    display: (v) => dur(PeakHoldSeconds(v)),
    hint: 'The bargraph’s hold bar, 0 to 10 s; zero turns it off. Nothing specifies it, so it stays live on Standard.',
  }),
  std('holdDecay', 'Hold Decay', kHoldDecayDefault, 'Ballistics', {
    display: (v) => `${HoldDecayDbPerSecond(v).toFixed(1)} dB/s`,
    hint: 'How fast the hold bar falls afterwards, 0 to 48 dB/s. Not specified by anybody.',
  }),

  // -- Look ---------------------------------------------------------------
  colour('faceR', 'Face Red', 0.93, 'Look', 'The dial’s face, the bargraph’s panel, the magic eye’s bezel.'),
  colour('faceG', 'Face Green', 0.90, 'Look'),
  colour('faceB', 'Face Blue', 0.79, 'Look'),
  colour('needleR', 'Needle Red', 0.10, 'Look', 'The pointer and the scale ink. The eye’s phosphor is a fixed tube green, so this does nothing on the Magic Eye.'),
  colour('needleG', 'Needle Green', 0.09, 'Look'),
  colour('needleB', 'Needle Blue', 0.08, 'Look'),
  opt('scaleStyle', 'Scale Style', ['Full', 'Marks Only', 'Plain', 'None'], 0, 'Look',
    'Full: marks, numbers and the legend. Marks Only: no text. Plain: the arc and the red band. None: face and pointer.'),
  std('lamp', 'Lamp', 0.35, 'Look', 'A warm bulb behind the face.'),
  std('glass', 'Glass', 0.25, 'Look', 'One specular streak across the cover.'),
  std('wear', 'Wear', 0.0, 'Look',
    'Two jobs: dirt on the face in the shader, and dry friction in the pivot in the engine — a worn pointer stops SHORT, anywhere within 3 % of full scale at Wear 1, and somewhere different depending on which way it came.'),
  std('persistence', 'Persistence', kPersistenceDefault, 'Look', {
    display: (v) => dur(PersistenceSeconds(v)),
    hint: 'The magic eye’s phosphor, 0 to 500 ms: the shadow’s edge smears when the level moves fast.',
  }),

  // -- Layout -------------------------------------------------------------
  std('size', 'Size', kSizeDefault, 'Layout', {
    display: (v) => `${pct(SizeFraction(v))} of height`,
    hint: 'The instrument’s height as a fraction of the raster’s shorter side, 15 % to 100 %.',
  }),
  std('posX', 'Position X', 0.5, 'Layout', { display: (v) => Offset(v).toFixed(2) }),
  std('posY', 'Position Y', 0.5, 'Layout', { display: (v) => Offset(v).toFixed(2) }),
  std('rotation', 'Rotation', 0.5, 'Layout', {
    display: (v) => `${(RotationRadians(v) * 180 / PI).toFixed(0)}°`,
    hint: '−180° to +180°. Text is pixel-exact only at 0°; at any other angle the glyph grid is resampled nearest-neighbour, on purpose.',
  }),
  std('background', 'Background', 0.0, 'Layout', 'Opacity of the Back colour behind the instrument. At 0 the output is the meter alone, with real alpha.'),
  colour('backR', 'Back Red', 0.04, 'Layout'),
  colour('backG', 'Back Green', 0.04, 'Layout'),
  colour('backB', 'Back Blue', 0.05, 'Layout'),
  std('mix', 'Mix', 1.0, 'Layout', 'Overall opacity of the output.'),
];

let readoutNode = null;
let readoutAt = 0;

function readout(level, engine) {
  if (!readoutNode) return;
  const now = performance.now();
  if (now - readoutAt < 150) return;
  readoutAt = now;
  const st = engine.channel(0);
  const read = level > 0 ? `${Db(level).toFixed(1)} dBFS` : 'silence';
  readoutNode.textContent =
    `This frame’s spectrum, read by the plugin’s level law: ${read} · on the meter’s own scale: ${st.inputDb <= -119 ? '—' : signedDb(st.inputDb)} · PPM detector: ${st.ppmDb <= -119 ? '—' : signedDb(st.ppmDb)} · engine steps this frame: ${engine.lastSteps}`;
}

const mounted = mountDemo({
  name: 'Needle',
  pluginId: 'ND01',
  tagline:
    'Four audio meters — a VU, a BBC PPM, an LM3915 LED bargraph and a magic eye — each moving the way its standard says it must. The VU’s damping and natural frequency are solved from ANSI C16.5’s two sentences, the PPM’s time constants from IEC 60268-10 type II, and the engine integrates them at 4800 Hz whatever the frame rate.',
  repo: 'https://github.com/stoatworks-labs/needle',
  page: 'https://stoatworks-labs.com/software/needle/',
  video: 'https://www.youtube.com/watch?v=37tMfGAREYU',

  // Needle is a SOURCE. The stock clause says "on generated clips".
  blurb:
    'It is Needle’s own GLSL, ported from the repository to WebGL2, driven by a JavaScript port of the plugin’s meter engine and level law — same parameters, same maths, no install. There is no audio here: the meters are driven by a generated test signal (tone steps, bursts, pink noise), which this page shapes into a 64-bin spectrum itself. That is not Resolume’s FFT, whose window, normalisation and bin law nobody has measured.',

  // Background defaults to 0, so the output is the meter alone with real alpha;
  // in Resolume what shows through would be the layers below.
  showBackdrop: true,

  sources: [],

  variants: {
    label: 'Test signal',
    default: 'step0',
    options: SIGNALS.map(({ id, name, hint }) => ({ id, name, hint })),
  },

  params: PARAMS,

  differences: [
    'There is no audio. The plugin reads one thing from its host — Resolume’s 64-bin FFT buffer, once per frame — and a browser has no Resolume. This page does not ask for a microphone. Instead a generated test signal (a 0 VU step, a −20 dB step, a steady alignment tone, 5 ms bursts, a ladder of burst lengths, pink noise, a pink-noise ramp, silence) is integrated over each frame’s interval and written into 64 bins by this page, and the plugin’s own level law runs on those bins.',
    'It is not the host’s FFT. Resolume’s window, normalisation, headroom, bin layout and whether a bin is a magnitude or a power are all unknown — which is why the plugin has Bin Law, Reference Level and Sensitivity. The page writes magnitudes (a tone into one bin, pink noise as 1/f with a per-bin exponential scatter), so Bin Law on Magnitude reads each test level exactly and Power shows the other assumption. A test level in dBFS means what the plugin’s level law reads: RMS, 1.0 as full scale.',
    'The Audio buffer parameter and the About block are absent from the panel. The buffer is written by a host, not an operator — the test signal stands in for it — and the About block’s text line and four link buttons exist so a host has somewhere to put links a web page already has. The other 31 parameters are all here.',
    'There is no clip and no "use my own file". Needle is a source with zero inputs; the kit offers both controls to every demo and this page removes them rather than leaving them present and inert.',
    'The whole CPU half is a JavaScript port, and nothing checks it but a reader: the solved constants (ζ from the overshoot in closed form, ωn by bisection on the exact step response, the PPM’s two time constants), the RK4 movement with dry friction, the quasi-peak follower, the hold bar, the magic eye’s warm-up and persistence, the 4800 Hz engine with frame-one priming and the 0.25 s stall clamp, Resolve for Standard and Free, the scale maps, the LM3915 law, the Controls.h conversions, BuildFrame’s geometry and the 5×7 font. The shaders are the plugin’s, unedited; demo/tools/check_shaders.py fails the repository’s verify script if a character drifts.',
    'The clock is the browser’s frame clock in seconds. The plugin’s Clock, which works out what unit a host’s SetTime is in, is not ported, because there is no unit to discover here. Restart sends the clock backwards, which the engine treats as a loop point and advances nothing — the meters carry on from where they were, as the plugin’s would.',
    'Nothing here is measured. The 300 ms to 99 %, the 1.25 % overshoot, the 20 dB in 2.8 s, the 3 dB ladder, the eye shutting at the overload, frame-one priming and frame-rate independence are all checked by ndtest in the repository, with no GL context at all. That harness, not this page, is the reason to believe the ballistics.',
    'The plugin has never been loaded into Resolume on macOS and no real audio has reached it in a host. On Windows it passed the fleet’s Arena gate in Arena 7.27.1 on llvmpipe, with the audio-driven controls skipped for want of a sound device. This page is a browser and is evidence about neither.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The two controls a source has no use for, removed rather than left dead —
// astable's precedent. And a line under the picture that says, every frame,
// what the test signal is putting into the plugin's level law.
//---------------------------------------------------------------------------
for (const field of document.querySelectorAll('.transport__field')) {
  if (field.querySelector('.transport__label')?.textContent === 'Clip') field.remove();
}
document.querySelector('.transport__file')?.remove();

const transportBar = document.querySelector('.transport');
if (transportBar && mounted) {
  const note = document.createElement('p');
  note.className = 'stage__status';
  note.textContent =
    'Test signal, not audio. The meters are driven by the signal chosen above, which this page turns into a 64-bin spectrum itself. That is not Resolume’s FFT: the plugin’s own level law runs on it, but the host’s calibration is unknown and is not reproduced.';
  readoutNode = document.createElement('p');
  readoutNode.className = 'stage__status stage__readout';
  transportBar.after(note, readoutNode);
}
