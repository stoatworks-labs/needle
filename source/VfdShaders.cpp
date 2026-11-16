#include "VfdShaders.h"

namespace needle::vfd
{
// The VFD's fragment shader. Its own program, so source/Shaders.cpp -- and the
// demo page's verbatim copy of it -- never change for it. The vertex shader is
// needle's one full-screen triangle (kVertexShader), shared unchanged.
//
// Everything in pixels came from the CPU (vfd::Picture). The shader finds the
// one cell a fragment can be in, decides coverage, and does three pieces of
// physics, each in closed form: the camera's overlap with the grid's pulse,
// the wires' box coverage, and the sRGB encoding of a linear phosphor.
const char* const kVfdFragmentShader = R"(#version 410 core
out vec4 fragColour;

uniform ivec2 uSize;
uniform vec4  uXform;  // centre.x, centre.y, rotation, corner
uniform vec2  uHalf;   // glass half width, half height
uniform vec4  uField;  // x0, y0 of the column-0 row-0 cell; pitch x, pitch y
uniform vec2  uSeg;    // segment w, h
uniform ivec2 uGrid;   // columns, rows
uniform float uLit[ 31 ];
uniform float uDot[ 31 ];
uniform vec3  uPhosphor;// linear
uniform vec4  uWireY;   // four wire centres, local y
uniform vec3  uWire;    // width, x0, x1
uniform vec4  uScan;    // camera, phase0, scans per row, scans per exposure
uniform vec3  uSlot;    // slot, blanking, pulse (scans)
uniform vec4  uLampA;   // x, y, radius, on
uniform vec3  uLampCol;
uniform vec3  uBackCol;
uniform float uBackA;
uniform float uMix;

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

float sdBox( vec2 p, vec2 b )
{
	vec2 d = abs( p ) - b;
	return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 );
}

float sdRoundBox( vec2 p, vec2 b, float r )
{
	vec2 q = abs( p ) - b + vec2( r );
	return length( max( q, 0.0 ) ) + min( max( q.x, q.y ), 0.0 ) - r;
}

// The sRGB transfer function, CSS Color 4 section 10.2.
float encode( float l )
{
	return l <= 0.0031308 ? l * 12.92 : 1.055 * pow( l, 1.0 / 2.4 ) - 0.055;
}

// A grid's on-time from scan 0 to scan position p, in scans: whole scans
// times the pulse width, plus the part of this scan's pulse already past.
float onTime( float p, float a, float w )
{
	return floor( p ) * w + clamp( p - floor( p ) - a, 0.0, w );
}

// The overlap of a wire (centre yc, width w) with the pixel row centred on
// y: exact box coverage, so the rows a wire crosses darken by its width in
// total however thin it is. cov() is a one-pixel ramp and is wrong for this.
float wireCover( float y, float yc, float w )
{
	float lo = max( y - 0.5, yc - 0.5 * w );
	float hi = min( y + 0.5, yc + 0.5 * w );
	return clamp( hi - lo, 0.0, 1.0 );
}

void main()
{
	vec2 f = vec2( gl_FragCoord.x, float( uSize.y ) - gl_FragCoord.y );
	vec4 c = vec4( uBackCol, uBackA );

	vec2  d  = f - uXform.xy;
	float cs = cos( uXform.z );
	float sn = sin( uXform.z );
	vec2  p  = vec2( d.x * cs + d.y * sn, -d.x * sn + d.y * cs );

	// The glass: black, because nothing behind an unlit phosphor gives light
	// and the model has no room light to reflect.
	float glass = cov( sdRoundBox( p, uHalf, uXform.w ) );
	c = paint( c, vec3( 0.0 ), glass );

	vec2 q   = p - uField.xy;
	int  col = int( floor( q.x / uField.z ) );
	int  row = int( floor( q.y / uField.w ) );
	if( col >= 0 && col < uGrid.x && row >= 0 && row < uGrid.y )
	{
		vec2  centre     = uField.xy + ( vec2( float( col ), float( row ) ) + 0.5 ) * uField.zw;
		float coverage   = cov( sdBox( p - centre, 0.5 * uSeg ) );
		float fromBottom = float( uGrid.y - 1 - row );
		bool  lit        = fromBottom < uLit[ col ] - 0.5 || abs( fromBottom - uDot[ col ] ) < 0.5;
		if( lit && coverage > 0.0 )
		{
			// The eye sees the time average, which the drive normalises to 1.
			// A camera row integrates [t0 + r Tr/H, + E]; g is what it caught
			// of the grid's pulse train over what the average would give.
			float g = 1.0;
			if( uScan.x > 0.5 )
			{
				float a  = float( col ) * uSlot.x + uSlot.y;
				float w  = uSlot.x - uSlot.y;
				float p0 = uScan.y + floor( f.y ) * uScan.z;
				float p1 = p0 + uScan.w;
				g = ( onTime( p1, a, w ) - onTime( p0, a, w ) ) / ( uScan.w * uSlot.z );
			}
			// Linear light, clipped per channel as a sensor saturates.
			vec3 lin = min( uPhosphor * g, vec3( 1.0 ) );

			// The filaments, in front of the phosphor: opaque, unlit.
			float shade = 1.0;
			if( p.x >= uWire.y && p.x <= uWire.z )
			{
				for( int i = 0; i < 4; ++i )
					shade *= 1.0 - wireCover( p.y, uWireY[ i ], uWire.x );
			}
			lin *= shade;

			vec3 enc = vec3( encode( lin.r ), encode( lin.g ), encode( lin.b ) );
			c = paint( c, enc, coverage );
		}
	}

	if( uLampA.w > 0.5 )
		c = paint( c, uLampCol, cov( length( p - uLampA.xy ) - uLampA.z ) );

	c.a   = clamp( c.a * uMix, 0.0, 1.0 );
	c.rgb = clamp( c.rgb, 0.0, 1.0 );
	fragColour = vec4( c.rgb * c.a, c.a );
}
)";

} // namespace needle::vfd
