/**
	ndtest -- the offline harness.

	It drives **the real code that ships**: `NeedlePlugin` for anything that
	goes through the parameter list, and `meter/` directly for the ballistics,
	using the same `Resolve()` the plugin calls, so nothing below is a
	re-transcription of the model.

	    --ballistics  ANSI C16.5: 99 % of a step in 300 ms, 1.0-1.5 % overshoot
	    --ppm         IEC 268-10 II: 20 dB of fall-back in 2.8 s
	    --steps       the LM3915 ladder is 3 dB a rung, recovered by bisection
	    --eye         the shadow is monotonic and shuts at the overload
	    --prime       frame one advances nothing, and a clip trigger is not deaf
	    --rate        the same answer at 24, 30, 50, 60 and 144 fps
	    --friction    a worn pivot stops the pointer short, and differently
	                  depending on which way it came
	    --defaults    Free agrees with Standard at the shipped defaults
	    --names       no parameter name over FFGL's 16 characters
	    --font        print every glyph
	    --list        the fleet's parameter listing, for tools/sweep.py
	    --pixels      the shader draws where the CPU said, at two rasters
	    --bench       ms/frame at 720p, 1080p and 4K

	    --out f.png [--size WxH] [--frames N] [--level dBFS] [--burst ms]
	                [--set "Name=value" ...]

	## The one decision that shapes this file

	**Every physical claim is checked against the CPU engine, never against
	rendered pixels.** `--ballistics`, `--ppm`, `--steps`, `--eye`, `--prime`,
	`--rate`, `--defaults` and `--names` open no GL context at all; they run on
	a machine with no GPU and produce the same numbers there.

	That is not tidiness. Last time round this fleet shipped four checks out of
	six that were calibrated against one Mac's rasteriser and failed on a
	GPU-less runner -- and in all four cases the *test* was wrong, not the
	plugin. A meter's ballistics are a property of a differential equation. A
	rasteriser cannot have an opinion about them, so it should not be in the
	room when they are measured.

	## What `--pixels` may and may not assert

	It is the one check that reads pixels, and every tolerance in it is derived
	rather than observed:

	  * **Flat interiors only.** Coverage in this shader is
	    `clamp( 0.5 - d, 0, 1 )`, so any point at least half a pixel inside a
	    feature has coverage of exactly 1.0 and the colour written is exactly
	    the parameter. Expected values are computed from the parameters, never
	    read off a screen, and the tolerance is +-1 code value for the float to
	    8-bit conversion.
	  * **Colours chosen off the ties.** The face and pointer colours are set to
	    0.8 / 0.4 / 0.2 and 0.2 / 0.6 / 1.0, which land on 204, 102, 51 and 51,
	    153, 255 with more than a thousandth of a code value to spare. A default
	    like 0.90 lands on 229.5, where round-to-nearest is a coin toss that two
	    GPUs may call differently.
	  * **Probes at least a pixel inside.** The worst case distance from a
	    pixel's centre to a line through it is sqrt(2)/2 = 0.71 px, so a probe on
	    the pointer's centreline needs a half-width of at least 1.21 px for
	    coverage to be exactly 1. `--pixels` runs at Size 1.0, where the
	    half-width is 2.16 px at 640x360 and 6.48 px at 1920x1080.
	  * **Two rasters.** Everything is asserted at 640x360 *and* 1920x1080, from
	    geometry the plugin itself produced, so nothing in here can be true only
	    at the size it was written at.
	  * **graticule's burn-in trap.** Whatever is drawn over the whole face
	    covers a probe. Here that is the Lamp, the Glass and the Wear, all of
	    which are on by default, and the scale numbers. `Instance::quiet()`
	    turns off all four before anything is probed.

	## The synthetic input

	There is no host, so the spectrum is written straight into the buffer
	parameter with `SetParamElementValue`: one bin carrying `10^(dB/20)` and the
	rest zero. `LevelFromSpectrum` sums over every bin, so *which* bin is used
	cannot matter -- which is the point being made in `Audio.h`, exercised here
	rather than only asserted.
*/
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Font.h"
#include "Needle.h"
#include "meter/Engine.h"
#include "Audio.h"
#include "meter/Standards.h"

using namespace needle;

static int failures = 0;
static void Check( bool ok, const std::string& what )
{
	std::printf( "  %s %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

static std::string F( double v, int places = 6 )
{
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.*f", places, v );
	return buffer;
}

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// Which bin the synthetic spectrum goes in. Not zero and not the last, so a
/// level that depended on the bin index would be visibly wrong rather than
/// accidentally right.
constexpr int kInjectBin = 11;

double AmpFromDb( double db )
{
	return std::pow( 10.0, db / 20.0 );
}

//---------------------------------------------------------------------------
// Driving the engine with no host and no GL.
//---------------------------------------------------------------------------

/// Run `seconds` of host time at `fps` with a constant level, from a host clock
/// that starts at `startHost`. Returns the host time of the last frame.
double Drive( Engine& engine, double amplitude, double seconds, double fps, double startHost = 0.0 )
{
	const long frames = static_cast< long >( std::llround( seconds * fps ) );
	const double a[ 2 ] = { amplitude, amplitude };
	double       t      = startHost;
	for( long i = 0; i <= frames; ++i )
	{
		t = startHost + static_cast< double >( i ) / fps;
		engine.Frame( t, a, engine.CurrentSettings().channels );
	}
	return t;
}

/// Settle the engine at a level from cold. Half a second is 158 time constants
/// for the PPM detector and more than a full settling time for either movement,
/// so what comes back is the steady state to the last bit of a double.
void Settle( Engine& engine, const Settings& settings, double amplitude, double seconds = 1.0 )
{
	engine.Reset();
	engine.SetSettings( settings );
	Drive( engine, amplitude, seconds, 60.0 );
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency. Carried from graticule.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf                       compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );
	header.push_back( 6 );
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile,
										(CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										kCGLPFAAccelerated,
										kCGLPFAColorSize,
										(CGLPixelFormatAttribute)24,
										(CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj       pix  = nullptr;
	GLint                   npix = 0;
	if( CGLChoosePixelFormat( attrs, &pix, &npix ) != kCGLNoError || pix == nullptr )
	{
		// No accelerated context (a CI runner): take whatever there is.
		CGLPixelFormatAttribute soft[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										   kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
		if( CGLChoosePixelFormat( soft, &pix, &npix ) != kCGLNoError || pix == nullptr )
			return nullptr;
	}
	CGLContextObj ctx = nullptr;
	if( CGLCreateContext( pix, nullptr, &ctx ) != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( ctx );
	return ctx;
}

struct Target
{
	GLuint fbo = 0, colour = 0;
	int    w = 0, h = 0;

	void Create( int width, int height )
	{
		w = width;
		h = height;
		glGenTextures( 1, &colour );
		glBindTexture( GL_TEXTURE_2D, colour );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	}
	void Destroy()
	{
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( colour )
			glDeleteTextures( 1, &colour );
		fbo = colour = 0;
	}
};

/// Top-down RGBA, so (x, y) indexes the way every rectangle in a Frame is
/// written.
struct Image
{
	int                          w = 0, h = 0;
	std::vector< unsigned char > px;

	const unsigned char* at( int x, int y ) const
	{
		static const unsigned char zero[ 4 ] = { 0, 0, 0, 0 };
		if( x < 0 || y < 0 || x >= w || y >= h )
			return zero;
		return px.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
	}
	bool is( int x, int y, int r, int g, int b, int tol = 1 ) const
	{
		const unsigned char* p = at( x, y );
		return std::abs( p[ 0 ] - r ) <= tol && std::abs( p[ 1 ] - g ) <= tol &&
			   std::abs( p[ 2 ] - b ) <= tol;
	}
	bool same( int x, int y, int x2, int y2 ) const
	{
		return std::memcmp( at( x, y ), at( x2, y2 ), 3 ) == 0;
	}
	std::string str( int x, int y ) const
	{
		const unsigned char* p = at( x, y );
		return "(" + std::to_string( p[ 0 ] ) + "," + std::to_string( p[ 1 ] ) + "," +
			   std::to_string( p[ 2 ] ) + "," + std::to_string( p[ 3 ] ) + ")";
	}
};

Image readBack( const Target& t )
{
	std::vector< unsigned char > raw( static_cast< size_t >( t.w ) * t.h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	Image img;
	img.w = t.w;
	img.h = t.h;
	img.px.resize( raw.size() );
	for( int y = 0; y < t.h; ++y )
		std::memcpy( img.px.data() + static_cast< size_t >( y ) * t.w * 4,
					 raw.data() + static_cast< size_t >( t.h - 1 - y ) * t.w * 4,
					 static_cast< size_t >( t.w ) * 4 );
	return img;
}

struct Host
{
	CGLContextObj ctx = nullptr;

	bool Open()
	{
		ctx = createContext();
		if( ctx == nullptr )
		{
			std::printf( "no GL context\n" );
			return false;
		}
		return true;
	}
};

/// A plugin ready to render, with the synthetic spectrum under the harness's
/// control.
struct Instance
{
	NeedlePlugin plugin;
	double       level = 0.0;///< the injected amplitude

	Instance( int w, int h )
	{
		plugin.ForceSecondsClock();
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( w ), static_cast< FFUInt32 >( h ) };
		if( plugin.InitGL( &vp ) != FF_SUCCESS )
			std::printf( "  InitGL FAILED\n" );
	}
	~Instance() { plugin.DeInitGL(); }

	void set( unsigned int id, float v ) { plugin.SetFloatParameter( id, v ); }

	/// Write a synthetic spectrum carrying `db` relative to digital full scale.
	void inject( double db )
	{
		level = db <= -400.0 ? 0.0 : AmpFromDb( db );
		for( int i = 0; i < audio::kBins; ++i )
			plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned >( i ),
										 i == kInjectBin ? static_cast< float >( level ) : 0.0f );
	}

	/// The lamp, the glass, the wear and the scale numbers are all drawn over
	/// the face, and every one of them would move a flat-interior probe. This
	/// is graticule's burn-in plate, wearing four hats.
	void quiet()
	{
		set( PT_LAMP, 0.0f );
		set( PT_GLASS, 0.0f );
		set( PT_WEAR, 0.0f );
		set( PT_SCALE_STYLE, 3.0f );// None
		set( PT_PEAK_HOLD, 0.0f );  // and the hold bar, which sits over a segment
		set( PT_BACKGROUND, 1.0f );
		set( PT_BACK_R, 0.0f );
		set( PT_BACK_G, 0.0f );
		set( PT_BACK_B, 0.0f );
		set( PT_MIX, 1.0f );
		// Colours that land on whole code values with room to spare: see the
		// file header.
		set( PT_FACE_R, 0.8f );
		set( PT_FACE_G, 0.4f );
		set( PT_FACE_B, 0.2f );
		set( PT_NEEDLE_R, 0.2f );
		set( PT_NEEDLE_G, 0.6f );
		set( PT_NEEDLE_B, 1.0f );
		set( PT_SIZE, 1.0f );
	}
};

/// Render `frames` frames at 60 fps and hand back the last one.
Image renderFrames( Instance& i, const Target& t, int frames, double burstSeconds = -1.0,
					double db = -6.0 )
{
	ProcessOpenGLStruct gl = {};
	gl.HostFBO             = t.fbo;
	for( int f = 0; f < std::max( 1, frames ); ++f )
	{
		const double seconds = static_cast< double >( f ) / 60.0;
		if( burstSeconds >= 0.0 )
			i.inject( seconds < burstSeconds ? db : -400.0 );
		glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
		glViewport( 0, 0, t.w, t.h );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		i.plugin.SetTime( seconds );
		i.plugin.ProcessOpenGL( &gl );
	}
	return readBack( t );
}

/// The pixel a local-frame point falls in. The shader's own convention: pixel
/// (i, j) has its centre at (i + 0.5, j + 0.5) in the top-down frame.
void probePixel( const Unit& u, float lx, float ly, int& px, int& py )
{
	float sx = 0.0f, sy = 0.0f;
	NeedlePlugin::LocalToScreen( u, lx, ly, sx, sy );
	px = static_cast< int >( std::floor( sx ) );
	py = static_cast< int >( std::floor( sy ) );
}
} // namespace

//---------------------------------------------------------------------------
// --ballistics
//---------------------------------------------------------------------------
int runBallistics()
{
	std::printf( "the VU movement, against ANSI C16.5\n\n" );

	Settings settings;
	settings.standard    = true;
	const Resolved r     = Resolve( settings );
	const double   h     = 1.0 / kEngineRate;

	std::printf( "  solved from the standard: zeta = %s, wn = %s rad/s (%s Hz)\n",
				 F( r.vuZeta, 9 ).c_str(), F( r.vuOmegaUp, 9 ).c_str(),
				 F( r.vuOmegaUp / ( 2.0 * kPi ), 6 ).c_str() );
	std::printf( "  the textbook envelope would say ts(2%%) = %s s, which is not the\n"
				 "  same question and here is wrong by %s%%\n\n",
				 F( 4.0 / ( r.vuZeta * r.vuOmegaUp ), 4 ).c_str(),
				 F( 100.0 * ( 4.0 / ( r.vuZeta * r.vuOmegaUp ) / standards::kVuRiseTime - 1.0 ), 1 )
					 .c_str() );

	// A unit step into the movement the plugin runs, at the rate the plugin
	// runs it at. No scale, no pixels, no host.
	Movement m;
	double   t99      = -1.0;
	double   peak     = 0.0;
	double   maxError = 0.0;
	double   previous = 0.0;
	for( long i = 1; i <= static_cast< long >( 4.0 * kEngineRate ); ++i )
	{
		m.Step( h, 1.0, r.vuZeta, r.vuOmegaUp );
		const double t = static_cast< double >( i ) * h;

		if( t99 < 0.0 && m.x >= standards::kVu99 )
		{
			// Linear interpolation between the two bracketing steps. Over a
			// quarter of a millisecond of a curve this smooth the interpolation
			// error is far below a nanosecond.
			const double f = ( standards::kVu99 - previous ) / ( m.x - previous );
			t99            = ( static_cast< double >( i - 1 ) + f ) * h;
		}
		previous = m.x;
		peak     = std::max( peak, m.x );

		// ...and the same trajectory from the exact expression. RK4's local
		// error here is about (wn.h)^5/120 = 1.5e-15 per step, so 1e-9 is five
		// hundred times the bound and still tight enough that any wrong
		// coefficient shows up at once.
		maxError = std::max( maxError, std::fabs( m.x - standards::StepResponse( t, r.vuZeta, r.vuOmegaUp ) ) );
	}

	const double overshoot = peak - 1.0;

	std::printf( "  99%% of a step at %s s   (standard: %s +- 0.005)\n", F( t99, 6 ).c_str(),
				 F( standards::kVuRiseTime, 3 ).c_str() );
	std::printf( "  overshoot         %s%%  (standard: %s to %s)\n", F( 100.0 * overshoot, 4 ).c_str(),
				 F( 100.0 * standards::kVuOvershootLo, 1 ).c_str(),
				 F( 100.0 * standards::kVuOvershootHi, 1 ).c_str() );
	std::printf( "  integrator vs the closed form: max %s of full scale\n\n", F( maxError, 12 ).c_str() );

	Check( std::fabs( t99 - standards::kVuRiseTime ) <= 0.005,
		   "a step to 0 VU reaches 99 % in 300 ms +- 5 ms (" + F( t99 * 1000.0, 3 ) + " ms)" );
	Check( overshoot >= standards::kVuOvershootLo && overshoot <= standards::kVuOvershootHi,
		   "and overshoots between 1.0 % and 1.5 % (" + F( 100.0 * overshoot, 4 ) + " %)" );
	Check( maxError < 1e-9,
		   "the oversampled integrator tracks the closed form to 1e-9 (" + F( maxError, 12 ) + ")" );

	// The same movement driven through the engine, at the rate a host runs.
	Engine engine;
	Settle( engine, settings, AmpFromDb( settings.referenceDbfs ), 3.0 );
	const double steady = engine.Channel( 0 ).deflection;
	const double wanted = DeflectionFor( ScaleFor( MeterType::Vu ), 1.0 );
	Check( std::fabs( steady - wanted ) < 1e-6,
		   "0 VU settles at " + F( wanted, 6 ) + " of full deflection -- a VU scale is linear in "
		   "VOLTAGE, so 0 sits at 71 % of the arc (" + F( steady, 6 ) + ")" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ppm
//---------------------------------------------------------------------------
int runPpm()
{
	std::printf( "the PPM detector, against IEC 60268-10 type II\n\n" );

	Settings settings;
	settings.standard = true;
	const Resolved r  = Resolve( settings );
	const double   h  = 1.0 / kEngineRate;

	std::printf( "  solved from the standard: rise T = %s ms, fall T = %s s\n\n",
				 F( r.ppmRiseTau * 1000.0, 6 ).c_str(), F( r.ppmFallTau, 9 ).c_str() );

	// -- the fall ------------------------------------------------------------
	//
	// From full scale, in the AMPLITUDE domain where the standard's figure
	// lives, so nothing about where a scale ends can affect the answer.
	Follower follower;
	follower.x = 1.0;
	double fallTime = -1.0, previous = follower.x;
	const double target = AmpFromDb( -standards::kPpmFallDb );
	for( long i = 1; i <= static_cast< long >( 10.0 * kEngineRate ); ++i )
	{
		follower.Step( h, 0.0, r.ppmRiseTau, r.ppmFallTau );
		if( fallTime < 0.0 && follower.x <= target )
		{
			const double f = ( previous - target ) / ( previous - follower.x );
			fallTime       = ( static_cast< double >( i - 1 ) + f ) * h;
			break;
		}
		previous = follower.x;
	}

	std::printf( "  20 dB of fall-back in %s s   (standard: %s +- 0.05)\n", F( fallTime, 6 ).c_str(),
				 F( standards::kPpmFallSeconds, 2 ).c_str() );
	Check( fallTime > 0.0 && std::fabs( fallTime - standards::kPpmFallSeconds ) <= 0.05,
		   "a PPM falls 20 dB in 2.8 s +- 0.05 s (" + F( fallTime, 6 ) + " s)" );

	// -- the integration -----------------------------------------------------
	//
	// A 5 ms burst must read 2 dB below the same tone held. That is the same
	// requirement as the informal "reaches about 80 %": 10^(-2/20) = 0.7943.
	Follower burst;
	const long burstSteps = static_cast< long >( std::llround( standards::kPpmBurstSeconds * kEngineRate ) );
	for( long i = 0; i < burstSteps; ++i )
		burst.Step( h, 1.0, r.ppmRiseTau, r.ppmFallTau );
	const double downDb = -20.0 * std::log10( burst.x );

	std::printf( "  a %s ms burst reads %s dB below steady (standard: %s)\n",
				 F( standards::kPpmBurstSeconds * 1000.0, 0 ).c_str(), F( downDb, 6 ).c_str(),
				 F( standards::kPpmBurstDownDb, 1 ).c_str() );
	// The one-pole step is the EXACT solution over a constant target, so the
	// only error here is the burst length not being a whole number of steps:
	// 5 ms at 4800 Hz is 24 steps exactly, so the tolerance is float rounding.
	Check( std::fabs( downDb - standards::kPpmBurstDownDb ) < 1e-6,
		   "a 5 ms tone burst reads 2 dB below the same tone steady (" + F( downDb, 6 ) + " dB)" );

	// -- and through the engine ---------------------------------------------
	Engine   engine;
	Settings s = settings;
	s.type     = MeterType::Ppm;
	Settle( engine, s, AmpFromDb( s.referenceDbfs ), 2.0 );
	Check( std::fabs( engine.Channel( 0 ).ppmDb ) < 1e-4,
		   "at the reference the detector reads 0.0 dB on the meter's scale (" +
			   F( engine.Channel( 0 ).ppmDb, 6 ) + ")" );
	Check( std::fabs( engine.Channel( 0 ).deflection - 0.5 ) < 1e-4,
		   "which a BBC PPM puts at mark 4, exactly half way along a dB-linear scale (" +
			   F( engine.Channel( 0 ).deflection, 6 ) + ")" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --steps
//---------------------------------------------------------------------------
int runSteps()
{
	std::printf( "the bargraph ladder, against the LM3915 law\n\n" );

	Settings settings;
	settings.type = MeterType::Bargraph;

	Engine engine;

	// Recover each threshold by bisection on the INPUT, which is the only
	// honest way to read a ladder: it asks the question an operator asks --
	// "how loud does it have to be before this one lights?" -- rather than
	// reading back the constant the code used to decide.
	auto lights = [ & ]( int segment, double db ) {
		Settle( engine, settings, AmpFromDb( settings.referenceDbfs + db ), 0.5 );
		return engine.Channel( 0 ).segment[ segment ];
	};

	double threshold[ standards::kBargraphSteps ] = {};
	for( int k = 0; k < standards::kBargraphSteps; ++k )
	{
		double lo = -60.0, hi = 12.0;// known dark, known lit
		for( int i = 0; i < 40; ++i )
		{
			const double mid = 0.5 * ( lo + hi );
			if( lights( k, mid ) )
				hi = mid;
			else
				lo = mid;
		}
		threshold[ k ] = 0.5 * ( lo + hi );
	}

	std::printf( "  step  threshold   step above\n" );
	for( int k = standards::kBargraphSteps - 1; k >= 0; --k )
		std::printf( "  %4d  %9s   %s\n", k + 1, F( threshold[ k ], 6 ).c_str(),
					 k > 0 ? F( threshold[ k ] - threshold[ k - 1 ], 6 ).c_str() : "-" );
	std::printf( "\n" );

	// Forty bisections over a 72 dB bracket resolve to 72 / 2^40 = 7e-11 dB, so
	// a tolerance of 1e-3 dB is eight orders looser than the measurement and
	// still two orders tighter than any error worth having -- half a step is
	// 1.5 dB.
	bool spaced = true;
	for( int k = 1; k < standards::kBargraphSteps; ++k )
		spaced = spaced && std::fabs( ( threshold[ k ] - threshold[ k - 1 ] ) -
									  standards::kBargraphStepDb ) < 1e-3;
	Check( spaced, "every step is 3.000 dB above the one below it" );
	Check( std::fabs( threshold[ standards::kBargraphSteps - 1 ] ) < 1e-3,
		   "and the top step lights exactly at the reference (" +
			   F( threshold[ standards::kBargraphSteps - 1 ], 6 ) + " dB)" );
	Check( std::fabs( threshold[ 0 ] + standards::kBargraphStepDb *
										   ( standards::kBargraphSteps - 1 ) ) < 1e-3,
		   "so ten steps span 27 dB, bottom step at " + F( threshold[ 0 ], 3 ) + " dB" );

	// The two scale maps are inverses, on every scale.
	//
	// They are used in opposite directions and in different files: the engine
	// turns a level into a deflection, and the layout turns a printed mark's
	// decibels into a place on the arc. A disagreement between them would leave
	// the pointer right and every number on the face wrong -- which is the one
	// way a meter can be broken and still look plausible.
	//
	// The round trip is a pow and a log10 in double, so its error is a few ulp
	// of a number near one; 1e-12 is three orders above that.
	bool inverse = true;
	double worstRoundTrip = 0.0;
	for( int ty = 0; ty < kMeterTypeCount; ++ty )
	{
		const Scale sc = ScaleFor( static_cast< MeterType >( ty ) );
		for( double d = 0.0; d <= 1.0 + 1e-9; d += 0.125 )
		{
			const double back = DeflectionFor( sc, AmplitudeForDeflection( sc, d ) );
			worstRoundTrip    = std::max( worstRoundTrip, std::fabs( back - d ) );
		}
	}
	inverse = worstRoundTrip < 1e-12;
	Check( inverse, "every scale's level-to-deflection and deflection-to-level maps are "
					"inverses (worst " + F( worstRoundTrip, 15 ) + ")" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --eye
//---------------------------------------------------------------------------
int runEye()
{
	std::printf( "the magic eye's shadow\n\n" );

	Settings settings;
	settings.type = MeterType::Eye;

	const Scale scale    = ScaleFor( MeterType::Eye );
	const double overload = 0.0;// the meter's own reference, by definition

	Engine engine;

	auto shadowAt = [ & ]( double db ) {
		Settle( engine, settings, AmpFromDb( settings.referenceDbfs + db ), 2.0 );
		return engine.Channel( 0 ).eyeShadowDeg;
	};
	auto overlapAt = [ & ]( double db ) {
		Settle( engine, settings, AmpFromDb( settings.referenceDbfs + db ), 2.0 );
		return engine.Channel( 0 ).eyeOverlapDeg;
	};

	bool   monotonic = true;
	double previous  = 1e9;
	double worst     = 0.0;
	for( double db = -50.0; db <= 10.0 + 1e-9; db += 0.25 )
	{
		const double s = shadowAt( db );
		if( s > previous + 1e-9 )
		{
			monotonic = false;
			worst     = db;
		}
		previous = s;
	}

	const double open   = shadowAt( -50.0 );
	const double atLoad = shadowAt( overload );
	const double above  = shadowAt( 0.5 );
	const double below  = shadowAt( -0.5 );
	const double past   = overlapAt( 6.0 );

	// What "shut" can mean, and it is not "exactly zero at the overload".
	//
	// The target deflection AT the overload is exactly 1, and a second-order
	// movement approaches its target asymptotically -- so the shadow angle
	// there is a positive number that gets smaller the longer the engine runs,
	// and never reaches zero in finite time. Demanding `== 0.0` demands that an
	// exponential arrive at its asymptote. (It was the first version of this
	// check, and it failed on the machine it was written on, which is the best
	// place for that sort of thing to happen.)
	//
	// So the claim is made against what can be SEEN. The largest eye this
	// plugin can draw is Size 1.0 on a 3840x2160 raster: radius 0.72 * 1080 =
	// 778 px, where one pixel of arc subtends 1/778 rad = 0.0736 degrees. A
	// thousandth of a degree is seventy times finer than that, so a shadow
	// below it cannot be drawn by any raster this plugin will meet.
	//
	// Above the overload the model IS exact -- `ShadowDegrees` clamps -- so
	// that one is asserted as an equality.
	constexpr double kInvisibleDegrees = 1e-3;

	std::printf( "  scale: %s dB to %s dB, linear in dB\n", F( scale.bottomDb, 1 ).c_str(),
				 F( scale.topDb, 1 ).c_str() );
	std::printf( "  shadow at -50 dB  %s deg\n", F( open, 6 ).c_str() );
	std::printf( "  shadow at  -0.5   %s deg\n", F( below, 6 ).c_str() );
	std::printf( "  shadow at   0 dB  %s deg  (one 4K pixel of arc is 0.0736)\n",
				 F( atLoad, 9 ).c_str() );
	std::printf( "  shadow at  +0.5   %s deg\n", F( above, 9 ).c_str() );
	std::printf( "  overlap at +6 dB  %s deg\n\n", F( past, 6 ).c_str() );

	Check( monotonic, "the shadow angle never widens as the level rises" +
						  ( monotonic ? std::string() : " (first at " + F( worst, 2 ) + " dB)" ) );
	Check( open == standards::kEyeOpenDegrees,
		   "wide open below the scale, at the 6U5's 100 degrees exactly (" + F( open, 6 ) + ")" );
	Check( below > kInvisibleDegrees,
		   "still visibly open half a decibel below the overload (" + F( below, 6 ) + " deg)" );
	Check( atLoad < kInvisibleDegrees,
		   "shut at the stated overload, to a seventieth of a 4K pixel of arc (" +
			   F( atLoad, 9 ) + " deg)" );
	Check( above == 0.0, "and identically shut once past it (" + F( above, 9 ) + " deg)" );
	Check( overlapAt( overload ) == 0.0, "with no overlap at the overload itself" );
	Check( past > 0.0, "and the wings overlapping past it (" + F( past, 6 ) + " deg at +6 dB)" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --prime
//---------------------------------------------------------------------------
int runPrime()
{
	std::printf( "frame one\n\n" );

	Settings settings;
	Engine   engine;
	engine.SetSettings( settings );

	const double loud = AmpFromDb( settings.referenceDbfs );
	const double a[ 2 ] = { loud, loud };

	// A clip triggered forty seconds into a composition. The host's first
	// SetTime is 40, and an engine that reads that as elapsed time steps
	// 192,000 times before anybody has seen a frame.
	engine.Reset();
	engine.Frame( 40.0, a, 1 );
	Check( engine.LastSteps() == 0, "a first frame at host time 40 takes no steps (" +
										std::to_string( engine.LastSteps() ) + ")" );
	Check( engine.EngineSeconds() == 0.0, "and the engine clock is still at zero" );
	Check( engine.Channel( 0 ).deflection == 0.0,
		   "so every meter is at rest rather than pinned or settled" );

	// A zero interval must advance nothing. Writing a one-pole as
	// `dt > 0 ? 1 - exp(-dt/T) : 1` -- which is how the fleet has written it
	// before -- does the opposite, and the meter snaps to full on the one frame
	// it has no information about.
	engine.Frame( 40.0, a, 1 );
	Check( engine.LastSteps() == 0, "a repeated host time advances nothing" );
	Check( engine.Channel( 0 ).deflection == 0.0, "and nothing snapped" );

	// From here on the clock is real.
	for( int i = 1; i <= 60; ++i )
		engine.Frame( 40.0 + static_cast< double >( i ) / 60.0, a, 1 );
	Check( std::fabs( engine.EngineSeconds() - 1.0 ) <= 1.0 / kEngineRate,
		   "sixty frames later the engine has integrated one second (" +
			   F( engine.EngineSeconds(), 9 ) + ")" );

	// The claim that matters: a meter triggered at 40 seconds behaves exactly
	// like one triggered at zero.
	auto crossing = [ & ]( double startHost ) {
		Engine e;
		e.SetSettings( settings );
		e.Reset();
		const double wanted = 0.99 * DeflectionFor( ScaleFor( MeterType::Vu ), 1.0 );
		double       previous = 0.0;
		for( int i = 0; i <= 120; ++i )
		{
			const double t = startHost + static_cast< double >( i ) / 60.0;
			e.Frame( t, a, 1 );
			const double d = e.Channel( 0 ).deflection;
			if( i > 0 && d >= wanted )
			{
				const double f = ( wanted - previous ) / ( d - previous );
				// The first frame took no steps, so elapsed time is (i-1) + f
				// frames from the frame at which integration began.
				return ( static_cast< double >( i - 1 ) + f ) / 60.0;
			}
			previous = d;
		}
		return -1.0;
	};

	const double fromZero  = crossing( 0.0 );
	const double fromForty = crossing( 40.0 );
	std::printf( "\n  99 %% crossing through the frame path: %s s from a clock at 0,\n"
				 "  %s s from a clock at 40\n\n",
				 F( fromZero, 6 ).c_str(), F( fromForty, 6 ).c_str() );

	Check( fromZero > 0.0 && fromForty > 0.0, "the meter reaches 99 % from either clock origin" );
	// Not bit-identical, and it must not be asked to be. `40.0 + n/60.0` is not
	// the same sequence of doubles as `n/60.0`: the offset costs about 9e-15 s
	// of precision per reading, which is nothing until the carried residual
	// happens to cross a step boundary one frame earlier in one run than the
	// other. The two answers can therefore differ by at most ONE engine step --
	// the same bound `--rate` derives -- and that is what is asserted. An
	// earlier version of this line demanded 1e-9 s, which is a demand that
	// floating point be exact; it failed at once, on the machine it was
	// written on.
	Check( std::fabs( fromZero - fromForty ) <= 1.0 / kEngineRate,
		   "and reaches it at the same time from both, to within one engine step -- a clip "
		   "trigger is not deaf (" + F( std::fabs( fromZero - fromForty ) * 1e6, 3 ) + " us, "
		   "allowed " + F( 1e6 / kEngineRate, 1 ) + ")" );
	// This measurement is sampled on the host's frame grid, so its resolution
	// is a frame: 16.7 ms. 12 ms is inside that and still tight enough to catch
	// a movement that is out by a whole frame. The +-5 ms claim is made by
	// --ballistics, at the engine rate, where it can be.
	Check( std::fabs( fromZero - standards::kVuRiseTime ) < 0.012,
		   "300 ms, to the resolution of a 60 fps frame grid (" + F( fromZero * 1000.0, 2 ) + " ms)" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --friction
//---------------------------------------------------------------------------
int runFriction()
{
	std::printf( "a worn pivot\n\n" );

	Settings settings;
	settings.wear    = 1.0;
	const Resolved r = Resolve( settings );
	const double   h = 1.0 / kEngineRate;
	const double   band = r.frictionDeadBand;
	const double   f    = band * r.vuOmegaUp * r.vuOmegaUp;

	std::printf( "  dead band %s of full scale, friction %s units/s^2\n\n",
				 F( band, 6 ).c_str(), F( f, 4 ).c_str() );

	// Drive to the same target from below and from above, ten seconds each --
	// far longer than any settling time, so whatever is left is stuck and not
	// merely slow.
	const double target = 0.5;
	auto restAt = [ & ]( double from ) {
		Movement m;
		m.x = from;
		for( long i = 0; i < static_cast< long >( 10.0 * kEngineRate ); ++i )
			m.Step( h, target, r.vuZeta, r.vuOmegaUp, f );
		return m;
	};

	const Movement below = restAt( 0.0 );
	const Movement above = restAt( 1.0 );

	std::printf( "  from below, rests at %s  (error %s)\n", F( below.x, 9 ).c_str(),
				 F( below.x - target, 9 ).c_str() );
	std::printf( "  from above, rests at %s  (error %s)\n\n", F( above.x, 9 ).c_str(),
				 F( above.x - target, 9 ).c_str() );

	Check( below.v == 0.0 && above.v == 0.0, "the pointer comes to a dead stop, both ways" );
	// The stopping condition IS the dead band -- the step only zeroes the
	// velocity when |omega^2 (u - x)| <= friction, which is |u - x| <= band --
	// so this needs no tolerance beyond double arithmetic.
	Check( std::fabs( below.x - target ) <= band * ( 1.0 + 1e-12 ) &&
			   std::fabs( above.x - target ) <= band * ( 1.0 + 1e-12 ),
		   "and inside the dead band the friction implies, either way" );
	Check( std::fabs( below.x - target ) > 0.0 && std::fabs( above.x - target ) > 0.0,
		   "short of the target, which is what dry friction does and damping does not" );
	Check( below.x != above.x,
		   "and at a different place depending on which way it came -- the reason people "
		   "tap a worn meter (" + F( std::fabs( above.x - below.x ), 9 ) + " apart)" );

	// Wear 0 must be bit-identical to no friction at all: the headline ANSI
	// C16.5 claim is made there and must not be reachable from this code path.
	Settings clean;
	const Resolved rc = Resolve( clean );
	Check( rc.frictionDeadBand == 0.0, "Wear 0 resolves to no friction at all" );

	Movement a, b;
	bool identical = true;
	for( long i = 0; i < static_cast< long >( 2.0 * kEngineRate ); ++i )
	{
		a.Step( h, 1.0, rc.vuZeta, rc.vuOmegaUp );
		b.Step( h, 1.0, rc.vuZeta, rc.vuOmegaUp, rc.frictionDeadBand );
		identical = identical && a.x == b.x && a.v == b.v;
	}
	Check( identical, "and the movement is then bit-identical to the frictionless one" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --rate
//---------------------------------------------------------------------------
int runRate()
{
	std::printf( "the same answer whatever the host's frame rate\n\n" );

	Settings settings;
	const Resolved r = Resolve( settings );
	const double   h = 1.0 / kEngineRate;
	const double   span = 0.5;

	// The tolerance is derived, not observed. Two frame rates can differ by at
	// most one engine step in how many steps they have taken over the same span
	// of host time -- that is what the carried residual guarantees -- so the
	// most their answers can differ by is the movement's steepest slope times
	// one step. Measure that slope rather than guessing it.
	Movement probe;
	double   maxSlope = 0.0, previous = 0.0;
	for( long i = 1; i <= static_cast< long >( span * kEngineRate ); ++i )
	{
		probe.Step( h, 1.0, r.vuZeta, r.vuOmegaUp );
		maxSlope = std::max( maxSlope, std::fabs( probe.x - previous ) / h );
		previous = probe.x;
	}
	const double tolerance = 2.0 * maxSlope * h;

	const double rates[] = { 24.0, 30.0, 50.0, 60.0, 144.0 };
	double       result[ 5 ] = {};
	const double loud    = AmpFromDb( settings.referenceDbfs );

	std::printf( "  steepest slope %s per second; one engine step is %s s,\n"
				 "  so two rates cannot differ by more than %s\n\n",
				 F( maxSlope, 6 ).c_str(), F( h, 9 ).c_str(), F( tolerance, 9 ).c_str() );

	for( int i = 0; i < 5; ++i )
	{
		Engine engine;
		engine.SetSettings( settings );
		engine.Reset();
		// The first frame takes no steps, so the span is driven from frame one.
		const long frames = static_cast< long >( std::llround( span * rates[ i ] ) );
		const double a[ 2 ] = { loud, loud };
		for( long fr = 0; fr <= frames; ++fr )
			engine.Frame( static_cast< double >( fr ) / rates[ i ], a, 1 );
		result[ i ] = engine.Channel( 0 ).deflection;
		std::printf( "  %6s fps   deflection %s   steps %s\n", F( rates[ i ], 0 ).c_str(),
					 F( result[ i ], 9 ).c_str(), F( engine.EngineSeconds() * kEngineRate, 1 ).c_str() );
	}

	double spread = 0.0;
	for( int i = 1; i < 5; ++i )
		spread = std::max( spread, std::fabs( result[ i ] - result[ 0 ] ) );

	std::printf( "\n" );
	Check( spread <= tolerance, "half a second of the same signal reads the same at 24, 30, 50, 60 "
								"and 144 fps (spread " + F( spread, 12 ) + ", allowed " +
								F( tolerance, 9 ) + ")" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --defaults
//---------------------------------------------------------------------------
int runDefaults()
{
	std::printf( "the shipped defaults\n\n" );

	NeedlePlugin plugin;
	Settings     s = plugin.CurrentSettings();

	std::printf( "  reference %s dBFS   trim %s dB\n", F( s.referenceDbfs, 4 ).c_str(),
				 F( s.trimDb, 4 ).c_str() );
	std::printf( "  rise %s s   fall %s s   overshoot %s %%\n\n", F( s.riseSeconds, 6 ).c_str(),
				 F( s.fallSeconds, 6 ).c_str(), F( 100.0 * s.overshoot, 4 ).c_str() );

	Check( s.standard, "Standard is on out of the box" );

	// Every Free default is the value at which Free agrees with Standard, so
	// flipping the switch alone changes nothing. The controls are floats with
	// about seven significant digits and the rise map amplifies a relative
	// error by ln(100) = 4.6, so the honest floor here is around 5e-7; 1e-4 is
	// two hundred times that and still far tighter than the gap between any two
	// values anybody would confuse.
	Settings free_ = s;
	free_.standard = false;
	const Resolved a = Resolve( s );
	const Resolved b = Resolve( free_ );

	auto close = []( double x, double y ) { return std::fabs( x - y ) <= 1e-4 * std::fabs( y ); };
	Check( close( b.vuZeta, a.vuZeta ),
		   "Overshoot's default gives the standard's damping (" + F( b.vuZeta, 9 ) + " vs " +
			   F( a.vuZeta, 9 ) + ")" );
	Check( close( b.vuOmegaUp, a.vuOmegaUp ),
		   "Rise's default gives the standard's natural frequency (" + F( b.vuOmegaUp, 6 ) + " vs " +
			   F( a.vuOmegaUp, 6 ) + ")" );
	Check( close( b.ppmFallTau, a.ppmFallTau ),
		   "Fall's default gives the standard's fall-back (" + F( b.ppmFallTau, 6 ) + " vs " +
			   F( a.ppmFallTau, 6 ) + ")" );
	Check( std::fabs( s.referenceDbfs + 18.0 ) < 1e-4, "Reference Level is -18 dBFS" );
	Check( std::fabs( s.trimDb ) < 1e-4, "Sensitivity is 0 dB" );
	Check( std::fabs( s.holdSeconds - 1.5 ) < 1e-3, "Peak Hold is 1.5 s" );
	Check( std::fabs( s.holdDecayDbPerSecond - 12.0 ) < 1e-3, "Hold Decay is 12 dB/s" );

	// And what the audio path reads, with nothing injected.
	Check( plugin.InputLevel() == 0.0f, "with no host, every FFT bin is zero and the level is zero" );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --names, --list
//---------------------------------------------------------------------------
int runNames()
{
	NeedlePlugin plugin;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	int bad = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr )
			continue;
		if( std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu characters\n", id, name, std::strlen( name ) );
			++bad;
		}
		if( std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
		{
			std::printf( "  %-3u  %-28s is a duplicate -- --set and the sweep find a "
						 "parameter by name\n",
						 id, name );
			++bad;
		}
		seen.emplace_back( name );

		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s (%zu)\n", id, name, e, el, std::strlen( el ) );
				++bad;
			}
		}
	}
	std::printf( "\n  %d problem(s)\n", bad );
	return bad == 0 ? 0 : 1;
}

int runList()
{
	NeedlePlugin plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s %s\n", "id", "name", "kind", "value", "range", "means" );
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= PT_ABOUT_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "about", "-", "-",
						 "the Stoatworks About block; not swept" );
			continue;
		}
		if( id == PT_AUDIO )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s %s\n", id, name ? name : "", "buffer", "-", "-",
						 "the host's spectrum; the harness injects it with --level" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind,
					 plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --font
//---------------------------------------------------------------------------
int runFont()
{
	std::printf( "the font\n\n" );
	std::vector< std::pair< std::string, int > > seen;
	int blank = 0, dup = 0;
	for( int code = 32; code < 127; ++code )
	{
		const char* const* g = font::Glyph( code );
		std::string        key;
		bool               lit = false;
		for( int y = 0; y < font::kHeight; ++y )
		{
			key += g[ y ];
			for( int x = 0; x < font::kWidth; ++x )
				lit = lit || g[ y ][ x ] == '#';
		}
		if( code != 32 && !lit )
		{
			std::printf( "  glyph %d ('%c') is blank\n", code, code );
			++blank;
		}
		for( const auto& s : seen )
			if( code != 32 && s.first == key )
			{
				std::printf( "  glyph %d ('%c') duplicates %d ('%c')\n", code, code, s.second, s.second );
				++dup;
			}
		seen.emplace_back( key, code );
	}
	// The characters a meter actually draws, printed for a human.
	const char* used = "0123456789+-VUPM";
	for( const char* c = used; *c; ++c )
	{
		const char* const* g = font::Glyph( *c );
		std::printf( "  '%c'\n", *c );
		for( int y = 0; y < font::kHeight; ++y )
			std::printf( "      %s\n", g[ y ] );
	}
	std::printf( "\n  %d blank, %d duplicated\n", blank, dup );
	return ( blank == 0 && dup == 0 ) ? 0 : 1;
}

//---------------------------------------------------------------------------
// --pixels
//---------------------------------------------------------------------------
namespace
{
void pixelsAt( int W, int H )
{
	std::printf( "\n-- %dx%d ---------------------------------------------------\n", W, H );
	Target t;
	t.Create( W, H );

	const int faceR = 204, faceG = 102, faceB = 51;// 0.8 / 0.4 / 0.2, exactly
	const int needR = 51, needG = 153, needB = 255; // 0.2 / 0.6 / 1.0, exactly

	// -- the VU dial ---------------------------------------------------------
	{
		Instance i( W, H );
		i.quiet();
		i.inject( -400.0 );// silence: the pointer rests at the left stop
		Image        img = renderFrames( i, t, 120 );
		const Frame  f   = i.plugin.BuildFrame( W, H );
		const Unit&  U   = f.unit[ 0 ];

		Check( img.is( 0, 0, 0, 0, 0 ) && img.is( W - 1, H - 1, 0, 0, 0 ),
			   "the corners are the background (" + img.str( 0, 0 ) + ")" );

		// Deep inside the face, outside the arc entirely: local
		// (-0.70 halfW, -0.70 halfH) sits 1.6 arc radii from the pivot and a
		// fifth of the half-height inside the rounded corner.
		int px = 0, py = 0;
		probePixel( U, -U.halfW * 0.70f, -U.halfH * 0.70f, px, py );
		Check( img.is( px, py, faceR, faceG, faceB ),
			   "the face is exactly the face colour at (" + std::to_string( px ) + "," +
				   std::to_string( py ) + ") -- got " + img.str( px, py ) );

		// The pointer, at the angle the Frame carries, 60 % of the way out.
		const float half = U.needleWidth * 0.5f;
		Check( half >= 1.21f,
			   "the pointer is wide enough for a centreline probe to be exact (" + F( half, 3 ) +
				   " px, needs 1.21)" );
		auto probeNeedle = [ & ]( float angle, float frac, int& x, int& y ) {
			const float lx = U.pivot[ 0 ] + std::sin( angle ) * U.needleLen * frac;
			const float ly = U.pivot[ 1 ] - std::cos( angle ) * U.needleLen * frac;
			probePixel( U, lx, ly, x, y );
		};
		probeNeedle( U.needleAngle, 0.60f, px, py );
		Check( img.is( px, py, needR, needG, needB ),
			   "the pointer is drawn along the angle the CPU computed, got " + img.str( px, py ) );
		probeNeedle( -U.needleAngle, 0.60f, px, py );
		Check( img.is( px, py, faceR, faceG, faceB ),
			   "and nowhere near the mirror of it, got " + img.str( px, py ) );

		const double atRest = U.needleAngle;

		// Now drive it loud and check the pointer moved the way the engine says.
		Instance j( W, H );
		j.quiet();
		j.inject( -18.0 );// the reference: 0 VU
		img = renderFrames( j, t, 120 );
		const Frame  g = j.plugin.BuildFrame( W, H );
		const Unit&  V = g.unit[ 0 ];
		Check( V.needleAngle > atRest + 0.05,
			   "0 VU swings the pointer right (" + F( atRest, 4 ) + " -> " + F( V.needleAngle, 4 ) + " rad)" );
		probeNeedle( V.needleAngle, 0.60f, px, py );
		// Recomputed against the NEW unit, which has the same pivot.
		{
			const float lx = V.pivot[ 0 ] + std::sin( V.needleAngle ) * V.needleLen * 0.60f;
			const float ly = V.pivot[ 1 ] - std::cos( V.needleAngle ) * V.needleLen * 0.60f;
			probePixel( V, lx, ly, px, py );
		}
		Check( img.is( px, py, needR, needG, needB ),
			   "and the pointer is there too, got " + img.str( px, py ) );

		// Premultiplied alpha: a transparent background leaves the face
		// untouched, because coverage inside the face is 1.
		j.set( PT_BACKGROUND, 0.0f );
		img = renderFrames( j, t, 2 );
		probePixel( V, -V.halfW * 0.70f, -V.halfH * 0.70f, px, py );
		Check( img.is( px, py, faceR, faceG, faceB ) && img.at( px, py )[ 3 ] == 255,
			   "with Background off the face is still opaque and unchanged, got " + img.str( px, py ) );
		Check( img.at( 0, 0 )[ 3 ] == 0, "and the corner is fully transparent" );
	}

	// -- the bargraph --------------------------------------------------------
	{
		Instance i( W, H );
		i.quiet();
		i.set( PT_TYPE, 2.0f );
		// quiet() has already turned the hold bar off. Leaving it on puts a
		// bright bar across whichever step the level happens to sit in, and the
		// probe at that step's centre reads the bar instead -- which is exactly
		// graticule's burn-in plate covering a probe, arrived at independently.
		i.inject( -18.0 - 7.5 );// 7.5 dB down: two and a half steps
		Image       img = renderFrames( i, t, 120 );
		const Frame f   = i.plugin.BuildFrame( W, H );
		const Unit& U   = f.unit[ 0 ];

		// What the LM3915 law says should be lit, from the law rather than from
		// what this machine printed: every step whose threshold is at or below
		// the level, thresholds being 3 dB apart with the top one at the
		// reference. `--steps` recovers those thresholds by bisection without
		// transcribing anything, so this three-line restatement is checked
		// against an independent measurement elsewhere.
		const double db       = -7.5;
		const int    expected = std::clamp(
			   standards::kBargraphSteps +
				   static_cast< int >( std::floor( db / standards::kBargraphStepDb ) ),
			   0, standards::kBargraphSteps );
		const int lit = f.unit[ 0 ].segLit;
		Check( lit == expected, F( db, 1 ) + " dB below the reference lights " +
									std::to_string( expected ) + " of ten steps (" +
									std::to_string( lit ) + ")" );

		int dark[ 2 ] = { 0, 0 };
		probePixel( U, U.seg[ 9 ][ 0 ] + U.seg[ 9 ][ 2 ] * 0.5f,
					U.seg[ 9 ][ 1 ] + U.seg[ 9 ][ 3 ] * 0.5f, dark[ 0 ], dark[ 1 ] );

		bool litOk = true, darkOk = true;
		for( int k = 0; k < standards::kBargraphSteps; ++k )
		{
			int x = 0, y = 0;
			probePixel( U, U.seg[ k ][ 0 ] + U.seg[ k ][ 2 ] * 0.5f,
						U.seg[ k ][ 1 ] + U.seg[ k ][ 3 ] * 0.5f, x, y );
			// Relational, so no colour rule is transcribed into the test: a lit
			// step differs from a known-dark one and every dark step matches it.
			if( k < lit )
				litOk = litOk && !img.same( x, y, dark[ 0 ], dark[ 1 ] );
			else
				darkOk = darkOk && img.same( x, y, dark[ 0 ], dark[ 1 ] );
		}
		Check( litOk, "every lit step is drawn differently from an unlit one" );
		Check( darkOk, "and every unlit step is drawn the same" );

		// The hold bar, after the signal has gone.
		Instance j( W, H );
		j.quiet();
		j.set( PT_TYPE, 2.0f );
		j.set( PT_PEAK_HOLD, 1.0f );
		img = renderFrames( j, t, 90, 0.25, -18.0 );// loud for 250 ms, then silence
		const Frame g = j.plugin.BuildFrame( W, H );
		const Unit& V = g.unit[ 0 ];
		Check( V.hold[ 2 ] > 0.0f, "a burst leaves a hold bar behind" );
		Check( V.segLit < 10, "after the burst the column has fallen (" +
								  std::to_string( V.segLit ) + " lit)" );
		if( V.hold[ 2 ] > 0.0f )
		{
			int hx = 0, hy = 0;
			probePixel( V, V.hold[ 0 ] + V.hold[ 2 ] * 0.5f, V.hold[ 1 ] + V.hold[ 3 ] * 0.5f, hx, hy );
			int dx = 0, dy = 0;
			probePixel( V, V.seg[ 9 ][ 0 ] + V.seg[ 9 ][ 2 ] * 0.5f,
						V.seg[ 9 ][ 1 ] + V.seg[ 9 ][ 3 ] * 0.5f, dx, dy );
			Check( !img.same( hx, hy, dx, dy ), "and it is drawn, not the unlit colour, got " +
													img.str( hx, hy ) );
		}
	}

	// -- the magic eye -------------------------------------------------------
	{
		Instance i( W, H );
		i.quiet();
		i.set( PT_TYPE, 3.0f );
		i.inject( -18.0 - 20.0 );// 20 dB down: the shadow is half open
		Image       img = renderFrames( i, t, 600 );// long enough to warm up
		const Frame f   = i.plugin.BuildFrame( W, H );
		const Unit& U   = f.unit[ 0 ];

		Check( U.eyeShadow > 0.05f && U.eyeShadow < 1.5f,
			   "the shadow is part open at 20 dB down (" + F( U.eyeShadow * 180.0 / kPi, 2 ) +
				   " deg either side)" );

		// A point in the middle of the target ring, straight up (lit) and
		// straight down (in the shadow). Both are flat interiors.
		const float r = U.eyeRadius * 0.78f;
		int ux = 0, uy = 0, dx = 0, dy = 0;
		probePixel( U, 0.0f, -r, ux, uy );
		probePixel( U, 0.0f, r, dx, dy );
		Check( !img.same( ux, uy, dx, dy ),
			   "the lit sector and the shadow are different colours, " + img.str( ux, uy ) +
				   " vs " + img.str( dx, dy ) );

		// Drive it past the overload; the shadow shuts and the wings overlap.
		Instance j( W, H );
		j.quiet();
		j.set( PT_TYPE, 3.0f );
		j.inject( -18.0 + 6.0 );
		img = renderFrames( j, t, 600 );
		const Frame g = j.plugin.BuildFrame( W, H );
		const Unit& V = g.unit[ 0 ];
		Check( V.eyeShadow == 0.0f, "past the overload the shadow has shut" );
		Check( V.eyeOverlap > 0.0f, "and the wings overlap (" +
										F( V.eyeOverlap * 180.0 / kPi, 2 ) + " deg)" );
		probePixel( V, 0.0f, r, dx, dy );
		Check( !img.same( dx, dy, 0, 0 ), "the bottom of the target is now lit, got " + img.str( dx, dy ) );
	}

	// -- a stereo pair -------------------------------------------------------
	{
		Instance i( W, H );
		i.quiet();
		i.set( PT_COUNT, 1.0f );
		// quiet() sets Size to 1.0, which is right for one instrument and too
		// big for two: a pair of 4:3 dials side by side is 5.7 half-heights
		// wide, so at Size 1.0 the left-hand one hangs off a 16:9 raster and
		// the probe lands outside the image. The assertion below that both
		// probes are ON the raster is there so that a future change to the
		// layout fails loudly instead of quietly reading the out-of-bounds
		// sentinel, which is what happened the first time this was written.
		i.set( PT_SIZE, 0.40f );
		i.inject( -18.0 );
		Image       img = renderFrames( i, t, 120 );
		const Frame f   = i.plugin.BuildFrame( W, H );
		Check( f.count == 2, "Stereo Pair draws two instruments" );
		Check( f.unit[ 0 ].centre[ 0 ] < f.unit[ 1 ].centre[ 0 ], "side by side" );
		int ax = 0, ay = 0, bx = 0, by = 0;
		probePixel( f.unit[ 0 ], -f.unit[ 0 ].halfW * 0.70f, -f.unit[ 0 ].halfH * 0.70f, ax, ay );
		probePixel( f.unit[ 1 ], -f.unit[ 1 ].halfW * 0.70f, -f.unit[ 1 ].halfH * 0.70f, bx, by );
		Check( ax >= 0 && ax < W && ay >= 0 && ay < H && bx >= 0 && bx < W && by >= 0 && by < H,
			   "both probes land on the raster (" + std::to_string( ax ) + "," +
				   std::to_string( ay ) + " and " + std::to_string( bx ) + "," +
				   std::to_string( by ) + ")" );
		Check( img.is( ax, ay, faceR, faceG, faceB ) && img.is( bx, by, faceR, faceG, faceB ),
			   "both faces are drawn (" + img.str( ax, ay ) + " " + img.str( bx, by ) + ")" );
		Check( ax != bx, "at different places" );
	}

	t.Destroy();
}
} // namespace

int runPixels()
{
	Host host;
	if( !host.Open() )
		return 1;
	std::printf( "pixels -- every probe is a flat interior at a coordinate the plugin\n"
				 "itself produced, at two rasters\n" );
	std::printf( "\n  GL %s\n", glGetString( GL_VERSION ) );

	pixelsAt( 640, 360 );
	pixelsAt( 1920, 1080 );

	std::printf( "\n  %s\n", failures == 0 ? "PASS" : "FAIL" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBench()
{
	Host host;
	if( !host.Open() )
		return 1;

	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "1280x720" }, { 1920, 1080, "1920x1080" }, { 3840, 2160, "3840x2160" } };

	// Five passes of 200 frames, reporting the FASTEST and the slowest.
	//
	// This shader is cheap enough that a single average measures the machine's
	// mood rather than the plugin: the GPU is shared with a desktop, and a run
	// that lands next to a compositor pass reads two or three times a run that
	// does not. The minimum is the least-contended sample and is the number to
	// quote; the spread is printed beside it so the quote cannot pretend to a
	// precision it does not have.
	//
	// glFinish on both sides, without which this times how fast the driver
	// accepts commands rather than how fast the GPU runs them.
	std::printf( "ms/frame: five passes of 200 frames after a 30-frame warm-up,\n"
				 "fastest pass quoted, spread beside it. glFinish on both sides.\n\n" );
	std::printf( "  %-12s %-10s %-14s %s\n", "raster", "ms/frame", "spread", "% of a 60 fps frame" );

	for( const Size& s : sizes )
	{
		Target t;
		t.Create( s.w, s.h );
		Instance i( s.w, s.h );
		i.inject( -18.0 );

		ProcessOpenGLStruct gl = {};
		gl.HostFBO             = t.fbo;
		for( int f = 0; f < 30; ++f )
		{
			i.plugin.SetTime( f / 60.0 );
			i.plugin.ProcessOpenGL( &gl );
		}
		glFinish();

		double best = 1e9, worst = 0.0;
		for( int pass = 0; pass < 5; ++pass )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < 200; ++f )
			{
				i.plugin.SetTime( ( 30 + pass * 200 + f ) / 60.0 );
				i.plugin.ProcessOpenGL( &gl );
			}
			glFinish();
			const double ms = std::chrono::duration< double, std::milli >(
								  std::chrono::steady_clock::now() - start )
								  .count() /
							  200.0;
			best  = std::min( best, ms );
			worst = std::max( worst, ms );
		}

		std::printf( "  %-12s %-10s %-14s %.1f%%\n", s.name, F( best, 3 ).c_str(),
					 ( F( best, 3 ) + "-" + F( worst, 3 ) ).c_str(),
					 100.0 * best / ( 1000.0 / 60.0 ) );
		t.Destroy();
	}
	return 0;
}

//---------------------------------------------------------------------------
// --out
//---------------------------------------------------------------------------
int runOut( const std::string& path, int w, int h, int frames, double level, double burstMs,
			const std::vector< std::pair< std::string, std::string > >& sets )
{
	Host host;
	if( !host.Open() )
		return 1;
	Target t;
	t.Create( w, h );

	Instance i( w, h );
	for( const auto& kv : sets )
	{
		bool found = false;
		for( unsigned int id = 0; id < PT_COUNT_; ++id )
		{
			const char* name = i.plugin.GetParamName( id );
			if( name != nullptr && kv.first == name )
			{
				i.plugin.SetFloatParameter( id, static_cast< float >( std::atof( kv.second.c_str() ) ) );
				found = true;
				break;
			}
		}
		if( !found )
		{
			std::printf( "no parameter named '%s'\n", kv.first.c_str() );
			return 1;
		}
	}

	i.inject( level );
	const Image img = renderFrames( i, t, frames, burstMs >= 0.0 ? burstMs / 1000.0 : -1.0, level );
	if( !writePng( path, w, h, img.px ) )
	{
		std::printf( "could not write %s\n", path.c_str() );
		return 1;
	}
	return 0;
}

int main( int argc, char** argv )
{
	std::string                                          out;
	int                                                  w = 640, h = 360, frames = 60;
	double                                               level = -18.0, burstMs = -1.0;
	std::vector< std::pair< std::string, std::string > > sets;

	for( int a = 1; a < argc; ++a )
	{
		const std::string arg = argv[ a ];
		if( arg == "--ballistics" )
			return runBallistics();
		if( arg == "--ppm" )
			return runPpm();
		if( arg == "--steps" )
			return runSteps();
		if( arg == "--eye" )
			return runEye();
		if( arg == "--prime" )
			return runPrime();
		if( arg == "--rate" )
			return runRate();
		if( arg == "--friction" )
			return runFriction();
		if( arg == "--defaults" )
			return runDefaults();
		if( arg == "--names" )
			return runNames();
		if( arg == "--list" )
			return runList();
		if( arg == "--font" )
			return runFont();
		if( arg == "--pixels" )
			return runPixels();
		if( arg == "--bench" )
			return runBench();
		if( arg == "--out" && a + 1 < argc )
			out = argv[ ++a ];
		else if( arg == "--size" && a + 1 < argc )
			std::sscanf( argv[ ++a ], "%dx%d", &w, &h );
		else if( arg == "--frames" && a + 1 < argc )
			frames = std::atoi( argv[ ++a ] );
		else if( arg == "--level" && a + 1 < argc )
			level = std::atof( argv[ ++a ] );
		else if( arg == "--burst" && a + 1 < argc )
			burstMs = std::atof( argv[ ++a ] );
		else if( arg == "--set" && a + 1 < argc )
		{
			const std::string kv = argv[ ++a ];
			const size_t      eq = kv.find( '=' );
			if( eq == std::string::npos )
			{
				std::printf( "--set wants Name=value\n" );
				return 1;
			}
			sets.emplace_back( kv.substr( 0, eq ), kv.substr( eq + 1 ) );
		}
	}

	if( !out.empty() )
		return runOut( out, w, h, frames, level, burstMs, sets );

	std::printf(
		"usage: ndtest --ballistics | --ppm | --steps | --eye | --prime | --rate\n"
		"              --friction | --defaults | --names | --list | --font\n"
		"              --pixels | --bench\n"
		"       ndtest --out f.png [--size WxH] [--frames N] [--level dBFS]\n"
		"              [--burst MS] [--set Name=value ...]\n" );
	return 2;
}
