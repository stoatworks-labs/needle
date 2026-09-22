#include "Shaders.h"

namespace needle
{
const char* const kVertexShader = R"(#version 410 core
// One triangle that covers the viewport. No vertex buffer: the corners come
// from gl_VertexID, so the only thing bound at draw time is the VAO.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
)";

// The fragment shader, in two adjacent raw strings -- see Shaders.h.
const char* const kFragmentShader =
R"(#version 410 core
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
// Text, from the 5x7 table. `origin` is the top-left of the string in local
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
)"
R"(
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
)";

} // namespace needle
