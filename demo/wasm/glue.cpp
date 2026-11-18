/*
    The page's way in to the plugin's VFD: what an FFGL host does to a plugin
    instance, as C functions JavaScript can call. (woofer's glue, which is
    mirrorball's and fountain's, with needle's names and ndtest's feed.)

    Nothing here is the plugin, and nothing here does the plugin's work. The
    plugin is `source/Needle.cpp` and everything it calls -- vfd::Input and
    earshot under it, vfd::Display, the VFD's renderer and GLSL, the meter
    engine and the meters' renderer -- compiled UNMODIFIED into the same
    WebAssembly module as this file (demo/tools/build-wasm.sh has the list).
    This file only plays the host:

      - it constructs a `NeedlePlugin`, as SourcePlugin.cpp's registration
        would, and calls InitGL, Resize and DeInitGL on it;
      - it reads the parameter declarations back through the FFGL SDK's own
        host-facing getters, so the page can hold its panel to the plugin's
        constructor;
      - it forwards the host's calls: SetFloatParameter, SetTime and
        ProcessOpenGL into the page's framebuffer, with no input (a source).

    Two things here are the plugin's HARNESS rather than a host, as in
    tools/ndtest:

      - the clock: `nd_new` calls ForceSecondsClock(), as ndtest's Instance
        does, because the page hands SetTime seconds and has no unit to vote
        on;
      - the audio. ndtest never opens a device; it hands the VFD its audio
        through earshot's feed() on the plugin's own vfd::Input
        (`VfdInput().Activate()`, then `FeedBegin` and `Feed`: Vfd.inc's
        Driver). So does this page. `kPi`, `AmpFromDb`, `BandHz`, `Fade`,
        `Programme` and `PerBandDb` are ndtest's own, copied from
        tools/ndtest/main.mm and tools/ndtest/Vfd.inc character for
        character (demo/tools/check_shaders.py holds them to it), so the
        "harness's programme" signal is exactly what `ndtest --out` feeds a
        VFD at `--level -18`. The other test signals -- the stepped ISO
        tones, the sweep, the pink noise and the track -- are this page's,
        written here, and every one is a function of the sample index alone,
        as ndtest's are, so how the page's frames chop them changes nothing.

    No audio device exists in this module: earshot is built with
    EARSHOT_CAPTURE=OFF (NoCapture.cpp; miniaudio is not compiled), and
    `nd_prepare` sets NEEDLE_NO_DEVICES=1, verify.sh's switch, so vfd::Input
    never asks for a device list and refuses a device name before earshot
    sees one. Nothing here sets Audio Input or presses Rescan Inputs, so
    nothing calls selectDevice with a name; `nd_calls` reports the plugin's
    own counters of those calls (C1's), so the page can show it.

    GL calls go to the page's WebGL2 context through emscripten's GL library;
    demo/wasm/gl_shim.cpp replaces the four entry points that cannot be passed
    straight through.
*/
#include "Controls.h"
#include "Diag.h"
#include "Needle.h"
#include "Vfd.h"

#include <earshot/earshot.h>

#include <emscripten/emscripten.h>

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace needle;

namespace
{
//---------------------------------------------------------------------------
// tools/ndtest's signal functions, verbatim (check_shaders.py compares).
//---------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;

double AmpFromDb( double db )
{
	return std::pow( 10.0, db / 20.0 );
}

constexpr int kFeedRate = 48000;

using Signal = std::function< double( long long ) >;

/// IS 6964 eq. (3) for b = 3, base ten: the exact midband of band x. The
/// harness uses it to choose test FREQUENCIES; no bound below comes from it.
double BandHz( int x )
{
	return 1000.0 * std::pow( 10.0, x / 10.0 );
}

double Fade( double t, double ramp )
{
	return t >= ramp ? 1.0 : 0.5 - 0.5 * std::cos( kPi * t / ramp );
}

/// Programme for pictures: a sine at every third's centre, each at `levelDb`
/// (dB re a full-scale sine) with a 2 dB tilt across the display and `modDb`
/// of slow, independent movement, so bars move and no two columns agree.
Signal Programme( double levelDb, double modDb, int rate, double stop = -1.0 )
{
	return [ = ]( long long k ) {
		const double t = static_cast< double >( k ) / rate;
		if( stop >= 0.0 && t >= stop )
			return 0.0;
		double s = 0.0;
		for( int x = -17; x <= 13; ++x )
		{
			const double hz = BandHz( x );
			if( hz >= 0.45 * rate )
				continue;
			const int    c  = x + 17;
			const double db = levelDb + 2.0 * ( 15 - c ) / 15.0 +
							  modDb * std::sin( 2.0 * kPi * ( 0.21 + 0.037 * c ) * t + 1.3 * c );
			s += AmpFromDb( db ) * std::sin( 2.0 * kPi * hz * t + 0.7 * c );
		}
		return s * Fade( t, 0.02 );
	};
}

/// The programme at a TOTAL level: 31 equal sines sum to 10 lg 31 = 14.9 dB
/// over each one, so each sits that far under `--level`. At the default
/// -18 dBFS a band reads about -33 dBFS, eight segments under Reference.
double PerBandDb( double totalDb )
{
	return totalDb - 10.0 * std::log10( 31.0 );
}

//---------------------------------------------------------------------------
// The page's own test signals. Levels are dB re a full-scale sine -- AES17's
// dBFS, and the unit earshot reports a third in -- so a sine at -18 here
// reads -18 in its own band, and the VFD's default Reference Level, -18,
// lights its column to the top.
//---------------------------------------------------------------------------
constexpr double kLevelDb = -18.0;

/// A deterministic white noise sample in [-1, 1) for an index: an integer
/// hash, so a noise is a function of the sample index like everything else.
double Hash( uint64_t i )
{
	uint64_t z = i + 0x9E3779B97F4A7C15ull;
	z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	return static_cast< double >( z >> 11 ) * ( 1.0 / 9007199254740992.0 ) * 2.0 - 1.0;
}

/// RBJ biquad (Audio EQ Cookbook) high- or low-pass, run in place.
void Biquad( std::vector< double >& x, double hz, double q, bool high )
{
	const double w0 = 2.0 * kPi * hz / kFeedRate;
	const double al = std::sin( w0 ) / ( 2.0 * q ), cw = std::cos( w0 );
	const double b0 = high ? ( 1.0 + cw ) / 2.0 : ( 1.0 - cw ) / 2.0;
	const double b1 = high ? -( 1.0 + cw ) : 1.0 - cw;
	const double b2 = b0;
	const double a0 = 1.0 + al, a1 = -2.0 * cw, a2 = 1.0 - al;
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	for( double& v : x )
	{
		const double y = ( b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2 ) / a0;
		x2 = x1;
		x1 = v;
		y2 = y1;
		y1 = y;
		v  = y;
	}
}

/// A Butterworth filter of order 2 * sections, as cascaded biquads with the
/// Butterworth poles' Qs.
void Butterworth( std::vector< double >& x, double hz, int sections, bool high )
{
	for( int s = 0; s < sections; ++s )
		Biquad( x, hz, 1.0 / ( 2.0 * std::sin( kPi * ( 2.0 * s + 1.0 ) / ( 4.0 * sections ) ) ), high );
}

/// A noise burst made once: white noise times an exponential decay of `tau`
/// seconds, THEN band-limited (so the envelope's own onset puts nothing
/// outside the band), peak-normalised to 1.
std::vector< float > Burst( double seconds, double tau, double highHz, double lowHz, uint64_t seed )
{
	const int n = static_cast< int >( seconds * kFeedRate );
	std::vector< double > x( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		x[ static_cast< size_t >( i ) ] = Hash( seed + static_cast< uint64_t >( i ) ) * std::exp( -i / ( tau * kFeedRate ) );
	if( highHz > 0.0 )
		Butterworth( x, highHz, 4, true );
	if( lowHz > 0.0 )
		Butterworth( x, lowHz, 2, false );
	double peak = 0.0;
	for( double v : x )
		peak = std::max( peak, std::fabs( v ) );
	std::vector< float > out( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		out[ static_cast< size_t >( i ) ] = static_cast< float >( x[ static_cast< size_t >( i ) ] / peak );
	return out;
}

double At( const std::vector< float >& table, double t )
{
	const long i = std::lround( t * kFeedRate );
	return i >= 0 && i < static_cast< long >( table.size() ) ? table[ static_cast< size_t >( i ) ] : 0.0;
}

const std::vector< float >& ClosedHat()
{
	static const std::vector< float > t = Burst( 0.25, 0.03, 7000.0, 0.0, 1u << 20 );
	return t;
}
const std::vector< float >& OpenHat()
{
	static const std::vector< float > t = Burst( 0.8, 0.22, 6000.0, 0.0, 2u << 20 );
	return t;
}
const std::vector< float >& SnareNoise()
{
	static const std::vector< float > t = Burst( 0.5, 0.11, 1200.0, 7000.0, 3u << 20 );
	return t;
}

/// A kick: a sine swept 160 -> 48 Hz (35 ms), decaying over 220 ms.
double KickAt( double t )
{
	if( t < 0.0 || t > 0.5 )
		return 0.0;
	const double f1 = 48.0, df = 112.0, tau = 0.035;
	const double phase = 2.0 * kPi * ( f1 * t + df * tau * ( 1.0 - std::exp( -t / tau ) ) );
	return std::sin( phase ) * std::exp( -t / 0.22 ) * Fade( t, 0.001 );
}

/// The track's chords, one a bar (2 s): A minor, F, C, G. Frequencies of the
/// bass root and the three voices, equal temperament from A4 = 440 Hz.
struct Chord
{
	double root, v[ 3 ];
};
constexpr Chord kChords[ 4 ] = {
	{ 55.000, { 220.000, 261.626, 329.628 } },// Am
	{ 43.654, { 174.614, 220.000, 261.626 } },// F
	{ 65.406, { 261.626, 329.628, 391.995 } },// C
	{ 48.999, { 195.998, 246.942, 293.665 } },// G
};

/// The page's track: 120 BPM, an 8 s loop of four bars. A kick on every beat,
/// a snare on two and four, closed hats on the eighths and an open hat on the
/// last off-beat of each bar, a bass on the eighths, a held chord and a
/// sixteenth-note arpeggio two octaves up. Low, middle and top columns all
/// move, and the kicks and hats leave peak dots behind. Peaks near 0.8.
double TrackAt( double t )
{
	const double loop  = std::fmod( t, 8.0 );
	const int    barN  = static_cast< int >( loop / 2.0 );
	const double bar   = loop - 2.0 * barN;
	const double beat  = std::fmod( bar, 0.5 );
	const int    beatN = static_cast< int >( bar / 0.5 );
	const Chord& ch    = kChords[ barN ];
	double       v     = 0.0;

	v += 0.50 * KickAt( beat );
	if( beatN == 1 || beatN == 3 )
		v += 0.16 * std::sin( 2.0 * kPi * 185.0 * beat ) * std::exp( -beat / 0.07 ) * Fade( beat, 0.001 ) +
			 0.22 * At( SnareNoise(), beat );
	const double eighth = std::fmod( bar, 0.25 );
	const int    eighthN = static_cast< int >( bar / 0.25 );
	if( eighthN == 7 )
		v += 0.10 * At( OpenHat(), eighth );
	else
		v += ( eighthN % 2 ? 0.11 : 0.06 ) * At( ClosedHat(), eighth );

	// The bass: a band-limited saw on the root, an envelope per eighth.
	{
		const double env = Fade( eighth, 0.004 ) * ( 0.55 + 0.45 * std::exp( -eighth / 0.06 ) ) * Fade( 0.24 - eighth, 0.01 );
		double       saw = 0.0;
		for( int n = 1; n <= 8; ++n )
			saw += std::sin( 2.0 * kPi * n * ch.root * t ) / ( n * ( n > 3 ? n / 3.0 : 1.0 ) );
		v += 0.10 * env * saw;
	}
	// The chord, held for the bar.
	{
		const double env = Fade( bar, 0.25 ) * Fade( 2.0 - bar, 0.2 );
		for( double hz : ch.v )
			v += 0.035 * env * ( std::sin( 2.0 * kPi * hz * t ) + 0.3 * std::sin( 4.0 * kPi * hz * t ) + 0.12 * std::sin( 6.0 * kPi * hz * t ) );
	}
	// The arpeggio: chord tones two and three octaves up, a sixteenth each.
	{
		const double s16  = std::fmod( bar, 0.125 );
		const int    n16  = static_cast< int >( bar / 0.125 );
		const double hz   = ch.v[ n16 % 3 ] * ( n16 % 4 == 3 ? 8.0 : 4.0 );
		const double env  = Fade( s16, 0.003 ) * std::exp( -s16 / 0.07 );
		v += 0.06 * env * std::sin( 2.0 * kPi * hz * s16 );
	}
	return v;
}

/// The 31 exact base-ten midbands in turn, 20 Hz to 20 kHz, half a second
/// each with a 20 ms raised-cosine fade at both ends.
double TonesAt( double t )
{
	const double step = 0.5;
	const long   n    = static_cast< long >( std::floor( t / step ) );
	const int    x    = -17 + static_cast< int >( n % 31 );
	const double u    = t - static_cast< double >( n ) * step;
	return AmpFromDb( kLevelDb ) * Fade( u, 0.02 ) * Fade( step - u, 0.02 ) * std::sin( 2.0 * kPi * BandHz( x ) * u );
}

/// A logarithmic sine sweep, 20 Hz to 20 kHz in 20 s, then 2 s of silence.
double SweepAt( double t )
{
	const double T = 20.0, f0 = 20.0, f1 = 20000.0, L = std::log( f1 / f0 );
	const double u = std::fmod( t, T + 2.0 );
	if( u >= T )
		return 0.0;
	const double phase = 2.0 * kPi * f0 * T / L * ( std::exp( L * u / T ) - 1.0 );
	return AmpFromDb( kLevelDb ) * Fade( u, 0.02 ) * Fade( T - u, 0.02 ) * std::sin( phase );
}

/// Pink periodic noise: a loop of 2^18 samples (5.46 s) whose spectrum is
/// every line from the lower edge of the 20 Hz third (17.78 Hz) to the upper
/// edge of the 20 kHz third (22.39 kHz), each at a power proportional to 1/f
/// with a random phase, made once by an inverse FFT. Every third gets the
/// same power. Normalised to kLevelDb re a full-scale sine overall, so each
/// of the 31 thirds reads about 14.9 dB under that.
const std::vector< float >& PinkLoop()
{
	static std::vector< float > loop;
	if( !loop.empty() )
		return loop;
	const int n = 1 << 18;
	std::vector< std::complex< double > > X( static_cast< size_t >( n ) );
	const double lo = 1000.0 * std::pow( 10.0, -17.5 / 10.0 ), hi = 1000.0 * std::pow( 10.0, 13.5 / 10.0 );
	for( int m = 1; m < n / 2; ++m )
	{
		const double hz = static_cast< double >( m ) * kFeedRate / n;
		if( hz < lo || hz > hi )
			continue;
		const double phase = kPi * Hash( 0xA5A5A5A5ull + static_cast< uint64_t >( m ) );
		X[ static_cast< size_t >( m ) ]     = std::polar( 1.0 / std::sqrt( hz ), phase );
		X[ static_cast< size_t >( n - m ) ] = std::conj( X[ static_cast< size_t >( m ) ] );
	}
	// An iterative radix-2 FFT (sign +1: the inverse, unscaled).
	for( int i = 1, j = 0; i < n; ++i )
	{
		int bit = n >> 1;
		for( ; j & bit; bit >>= 1 )
			j ^= bit;
		j ^= bit;
		if( i < j )
			std::swap( X[ static_cast< size_t >( i ) ], X[ static_cast< size_t >( j ) ] );
	}
	for( int len = 2; len <= n; len <<= 1 )
	{
		const std::complex< double > w = std::polar( 1.0, 2.0 * kPi / len );
		for( int i = 0; i < n; i += len )
		{
			std::complex< double > wk( 1.0, 0.0 );
			for( int k = 0; k < len / 2; ++k )
			{
				const std::complex< double > a = X[ static_cast< size_t >( i + k ) ];
				const std::complex< double > b = X[ static_cast< size_t >( i + k + len / 2 ) ] * wk;
				X[ static_cast< size_t >( i + k ) ]           = a + b;
				X[ static_cast< size_t >( i + k + len / 2 ) ] = a - b;
				wk *= w;
			}
		}
	}
	double ms = 0.0;
	for( const auto& c : X )
		ms += c.real() * c.real();
	ms /= n;
	// A full-scale sine's mean square is 1/2.
	const double gain = std::sqrt( 0.5 * std::pow( 10.0, kLevelDb / 10.0 ) / ms );
	loop.resize( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		loop[ static_cast< size_t >( i ) ] = static_cast< float >( X[ static_cast< size_t >( i ) ].real() * gain );
	return loop;
}

/// The page's signals, by the number the page hands over.
enum Which : int
{
	kProgramme = 0,///< ndtest's: Programme( PerBandDb( -18 ), 9.0, 48000 )
	kTrack,
	kTones,
	kSweep,
	kPink,
	kSilence,
	kWhichCount
};

double SampleAt( int which, long long k )
{
	static const Signal programme = Programme( PerBandDb( -18.0 ), 9.0, kFeedRate );
	const double t = static_cast< double >( k ) / kFeedRate;
	switch( which )
	{
	case kProgramme: return programme( k );
	case kTrack: return TrackAt( t );
	case kTones: return TonesAt( t );
	case kSweep: return SweepAt( t );
	case kPink:
	{
		const std::vector< float >& p = PinkLoop();
		return p[ static_cast< size_t >( k % static_cast< long long >( p.size() ) ) ];
	}
	default: return 0.0;
	}
}

//---------------------------------------------------------------------------
// One plugin instance and the host-side structs it is handed.
//---------------------------------------------------------------------------
struct Instance
{
	NeedlePlugin plugin;
	bool         initialised = false;

	ProcessOpenGLStruct process {};

	/// The feed's absolute sample clock at kFeedRate: samples [0, sent) of the
	/// signal's timeline are behind us, fed or skipped.
	uint64_t sent    = 0;
	uint64_t fed     = 0;///< samples actually handed to feed()
	uint64_t skipped = 0;///< samples jumped over (see nd_feed)
	std::vector< float > buffer;

	std::string scratch;///< a string handed back to JS lives here until the next call
};

float asFloat( FFMixed mixed )
{
	float value = 0.0f;
	static_assert( sizeof( value ) == sizeof( mixed.UIntValue ), "FFMixed carries a float in its bits" );
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

const char* hold( Instance* instance, std::string text )
{
	instance->scratch = std::move( text );
	return instance->scratch.c_str();
}

/// The largest block handed to feed() in one frame: earshot's own catch-up
/// limit (Engine.cpp, kMaxCatchUp = 0.5 s). It would skip anything older
/// itself; skipping it here saves generating it.
constexpr uint64_t kMaxBlock = kFeedRate / 2;
} // namespace

extern "C"
{
//---------------------------------------------------------------------------
// Lifetime.
//---------------------------------------------------------------------------

/// Before the first instance: give the plugin's log (Diag.cpp) somewhere to
/// go, a directory in the page's in-memory file system named through the
/// override Diag.cpp reads (its own `mkdir -p` goes through system(), which a
/// browser does not have); and NEEDLE_NO_DEVICES=1, verify.sh's switch, so
/// vfd::Input never lists devices and refuses any device name.
EMSCRIPTEN_KEEPALIVE void nd_prepare()
{
	setenv( "NEEDLE_LOG_DIR", "/needle/logs", 1 );
	setenv( "NEEDLE_NO_DEVICES", "1", 1 );
	mkdir( "/needle", 0755 );
	mkdir( "/needle/logs", 0755 );
}

/// A new instance, its clock told it is handed seconds (ndtest's Instance).
EMSCRIPTEN_KEEPALIVE Instance* nd_new()
{
	Instance* instance = new Instance();
	instance->plugin.ForceSecondsClock();
	return instance;
}

EMSCRIPTEN_KEEPALIVE void nd_delete( Instance* instance )
{
	if( instance == nullptr )
		return;
	if( instance->initialised )
		instance->plugin.DeInitGL();
	delete instance;
}

/// InitGL, with the viewport a host would pass. 1 on success.
EMSCRIPTEN_KEEPALIVE int nd_init_gl( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width        = static_cast< GLuint >( width );
	viewport.height       = static_cast< GLuint >( height );
	instance->initialised = instance->plugin.InitGL( &viewport ) == FF_SUCCESS;
	return instance->initialised ? 1 : 0;
}

/// The host's FF_RESIZE, for a change of raster (ndtest --resize's call).
EMSCRIPTEN_KEEPALIVE void nd_resize( Instance* instance, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.width  = static_cast< GLuint >( width );
	viewport.height = static_cast< GLuint >( height );
	instance->plugin.Resize( &viewport );
}

//---------------------------------------------------------------------------
// The declarations, read as a host reads them.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int nd_param_count( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetNumParams() );
}

EMSCRIPTEN_KEEPALIVE const char* nd_param_name( Instance* instance, int index )
{
	const char* name = instance->plugin.GetParamName( static_cast< unsigned int >( index ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE int nd_param_type( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetParamType( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* nd_param_group( Instance* instance, int index )
{
	return hold( instance, instance->plugin.GetParamGroup( static_cast< unsigned int >( index ) ) );
}

/// The declared default of a numeric parameter, as a host sees it.
EMSCRIPTEN_KEEPALIVE float nd_param_default( Instance* instance, int index )
{
	return asFloat( instance->plugin.GetParamDefault( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE int nd_param_element_count( Instance* instance, int index )
{
	return static_cast< int >( instance->plugin.GetNumParamElements( static_cast< unsigned int >( index ) ) );
}

EMSCRIPTEN_KEEPALIVE const char* nd_param_element_name( Instance* instance, int index, int element )
{
	const char* name = instance->plugin.GetParamElementName( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) );
	return name ? name : "";
}

EMSCRIPTEN_KEEPALIVE float nd_param_element_value( Instance* instance, int index, int element )
{
	return asFloat( instance->plugin.GetParamElementDefault( static_cast< unsigned int >( index ), static_cast< unsigned int >( element ) ) );
}

EMSCRIPTEN_KEEPALIVE int nd_param_visible( Instance* instance, int index )
{
	return instance->plugin.GetParamVisibility( static_cast< unsigned int >( index ) ) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int nd_min_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMinInputs() );
}

EMSCRIPTEN_KEEPALIVE int nd_max_inputs( Instance* instance )
{
	return static_cast< int >( instance->plugin.GetMaxInputs() );
}

//---------------------------------------------------------------------------
// The host's calls.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int nd_set_float( Instance* instance, int index, float value )
{
	return instance->plugin.SetFloatParameter( static_cast< unsigned int >( index ), value ) == FF_SUCCESS ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float nd_get_float( Instance* instance, int index )
{
	return instance->plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
}

/// One frame: SetTime, then ProcessOpenGL into the page's framebuffer (the
/// default one, 0: the kit binds it before every frame). A source: no inputs.
/// 1 on success.
EMSCRIPTEN_KEEPALIVE int nd_process( Instance* instance, double seconds )
{
	instance->plugin.SetTime( seconds );
	instance->process.numInputTextures = 0;
	instance->process.inputTextures    = nullptr;
	instance->process.HostFBO          = 0;
	return instance->plugin.ProcessOpenGL( &instance->process ) == FF_SUCCESS ? 1 : 0;
}

//---------------------------------------------------------------------------
// The audio, as ndtest hands it over (Vfd.inc's Driver): the plugin's own
// vfd::Input built and activated, then earshot's feedBegin( 48000, 2 ), then
// feed() a frame at a time, the same sample on both channels.
//---------------------------------------------------------------------------

EMSCRIPTEN_KEEPALIVE void nd_feed_begin( Instance* instance )
{
	instance->plugin.VfdInput().Activate();
	instance->plugin.VfdInput().FeedBegin( kFeedRate, 2 );
}

/// Signal `which` up to absolute sample `end` at kFeedRate: samples
/// [sent, end) of its timeline, into feed(). A gap longer than earshot's own
/// catch-up (half a second: the page's clock ran while the VFD was not drawn)
/// is skipped to its newest half second, as earshot itself would skip it.
/// Returns the samples fed.
EMSCRIPTEN_KEEPALIVE int nd_feed( Instance* instance, double end, int which )
{
	const double wanted = std::floor( end + 0.5 );
	if( !( wanted > static_cast< double >( instance->sent ) ) )
		return 0;
	const uint64_t to = static_cast< uint64_t >( wanted );
	if( to - instance->sent > kMaxBlock )
	{
		instance->skipped += to - kMaxBlock - instance->sent;
		instance->sent = to - kMaxBlock;
	}
	const int n = static_cast< int >( to - instance->sent );
	instance->buffer.resize( static_cast< size_t >( 2 * n ) );
	for( int k = 0; k < n; ++k )
	{
		const float v = static_cast< float >( SampleAt( which, static_cast< long long >( instance->sent + static_cast< uint64_t >( k ) ) ) );
		instance->buffer[ static_cast< size_t >( 2 * k ) ] = instance->buffer[ static_cast< size_t >( 2 * k + 1 ) ] = v;
	}
	instance->plugin.VfdInput().Feed( instance->buffer.data(), n );
	instance->sent = to;
	instance->fed += static_cast< uint64_t >( n );
	return n;
}

EMSCRIPTEN_KEEPALIVE int nd_signal_count()
{
	return kWhichCount;
}

EMSCRIPTEN_KEEPALIVE double nd_fed( Instance* instance )
{
	return static_cast< double >( instance->fed );
}

EMSCRIPTEN_KEEPALIVE double nd_skipped( Instance* instance )
{
	return static_cast< double >( instance->skipped );
}

//---------------------------------------------------------------------------
// For the panel's read-outs: Controls.h's conversion for a parameter, the one
// NeedlePlugin::VfdSettings and BuildVfdPicture apply to it. NaN for one with
// no conversion here.
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE double nd_convert( int index, float v )
{
	switch( static_cast< unsigned int >( index ) )
	{
	case PT_MULTIPLEX: return MultiplexHz( v );
	case PT_SHUTTER: return ShutterSeconds( v );
	case PT_REFERENCE: return ReferenceDbfs( v );
	case PT_SENSITIVITY: return TrimDb( v );
	case PT_PEAK_HOLD: return PeakHoldSeconds( v );
	case PT_HOLD_DECAY: return HoldDecayDbPerSecond( v );
	default: return std::numeric_limits< double >::quiet_NaN();
	}
}

//---------------------------------------------------------------------------
// What the line under the picture reports: the plugin's own state, through
// the accessors its harness reads (VfdDisplay(), VfdInput()).
//---------------------------------------------------------------------------
EMSCRIPTEN_KEEPALIVE int nd_vfd_columns( Instance* instance )
{
	return instance->plugin.VfdDisplay().Count();
}
EMSCRIPTEN_KEEPALIVE int nd_vfd_present( Instance* instance, int column )
{
	return instance->plugin.VfdDisplay().At( column ).present ? 1 : 0;
}
/// The column's level after Sensitivity, dB re a full-scale sine.
EMSCRIPTEN_KEEPALIVE double nd_vfd_level( Instance* instance, int column )
{
	return instance->plugin.VfdDisplay().At( column ).levelDb;
}
EMSCRIPTEN_KEEPALIVE int nd_vfd_lit( Instance* instance, int column )
{
	return instance->plugin.VfdDisplay().At( column ).lit;
}
EMSCRIPTEN_KEEPALIVE int nd_vfd_dot( Instance* instance, int column )
{
	return instance->plugin.VfdDisplay().At( column ).dot;
}
/// The column's band centre (the IEC 61260 base-ten midband of its band, or
/// of an octave's centre third), Hz.
EMSCRIPTEN_KEEPALIVE double nd_vfd_hz( Instance* instance, int column )
{
	return BandHz( vfd::ColumnX( instance->plugin.VfdDisplay().Bands(), column ) );
}
/// vfd::Lamp: 0 amber, 1 green, 2 red, 3 blinking red.
EMSCRIPTEN_KEEPALIVE int nd_vfd_lamp( Instance* instance )
{
	return static_cast< int >( instance->plugin.VfdInput().LampState() );
}
EMSCRIPTEN_KEEPALIVE const char* nd_capture_state( Instance* instance )
{
	earshot::Engine* engine = instance->plugin.VfdInput().EngineForTest();
	return engine ? earshot::toString( engine->captureState() ) : "no engine";
}
EMSCRIPTEN_KEEPALIVE int nd_sample_rate( Instance* instance )
{
	earshot::Engine* engine = instance->plugin.VfdInput().EngineForTest();
	return engine ? engine->frame().sampleRate : 0;
}
EMSCRIPTEN_KEEPALIVE int nd_third_count( Instance* instance )
{
	earshot::Engine* engine = instance->plugin.VfdInput().EngineForTest();
	return engine ? engine->frame().thirdCount : 0;
}
EMSCRIPTEN_KEEPALIVE double nd_audio_time( Instance* instance )
{
	earshot::Engine* engine = instance->plugin.VfdInput().EngineForTest();
	return engine ? engine->frame().audioTime : 0.0;
}
/// The plugin's own counters of every call that can start CoreAudio (C1):
/// 0 Engines built, 1 device lists, 2 selectDevice calls. Process-wide.
EMSCRIPTEN_KEEPALIVE int nd_calls( int which )
{
	auto& c = vfd::Input::Calls();
	return which == 0 ? c.engines.load() : which == 1 ? c.lists.load() : c.selects.load();
}
/// Devices the plugin's menu lists: slots past None and System default that
/// name a device (the rest are "(none)" spares). 0 here, always.
EMSCRIPTEN_KEEPALIVE int nd_devices_listed( Instance* instance )
{
	int n = 0;
	for( const vfd::Slot& s : instance->plugin.VfdInput().Slots() )
		if( !s.name.empty() && s.name != earshot::kDefaultDevice )
			++n;
	return n;
}

/// The plugin's log file (Diag.cpp), in the page's in-memory file system.
EMSCRIPTEN_KEEPALIVE const char* nd_log_path()
{
	static std::string path;
	path = diag::logPath();
	return path.c_str();
}

} // extern "C"
