#include "Vfd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <earshot/earshot.h>
#include <earshot/testing.h>

#include "Diag.h"

namespace needle::vfd
{
namespace
{
Input::BackendFactory g_factory = nullptr;

/// NEEDLE_NO_DEVICES=1: never ask the OS for its devices and never hand
/// earshot a device name (earshot's own lookup starts CoreAudio even for a
/// name it then fails to match). verify.sh sets it for oxbow and the sweep.
bool DevicesDisabled()
{
	const char* v = std::getenv( "NEEDLE_NO_DEVICES" );
	return v != nullptr && *v != '\0' && std::strcmp( v, "0" ) != 0;
}

constexpr const char* kHostLabel    = "None (host FFT)";
constexpr const char* kDefaultLabel = "System default";
constexpr const char* kEmptySlot    = "(none)";

// ---- colour: CSS Color 4, section 10.2 --------------------------------------
struct Mat3
{
	double m[ 3 ][ 3 ];
};

Mat3 Inverse( const Mat3& a )
{
	const auto& m = a.m;
	const double det = m[ 0 ][ 0 ] * ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] ) -
					   m[ 0 ][ 1 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 0 ] ) +
					   m[ 0 ][ 2 ] * ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] );
	Mat3 r{};
	r.m[ 0 ][ 0 ] = ( m[ 1 ][ 1 ] * m[ 2 ][ 2 ] - m[ 1 ][ 2 ] * m[ 2 ][ 1 ] ) / det;
	r.m[ 0 ][ 1 ] = ( m[ 0 ][ 2 ] * m[ 2 ][ 1 ] - m[ 0 ][ 1 ] * m[ 2 ][ 2 ] ) / det;
	r.m[ 0 ][ 2 ] = ( m[ 0 ][ 1 ] * m[ 1 ][ 2 ] - m[ 0 ][ 2 ] * m[ 1 ][ 1 ] ) / det;
	r.m[ 1 ][ 0 ] = ( m[ 1 ][ 2 ] * m[ 2 ][ 0 ] - m[ 1 ][ 0 ] * m[ 2 ][ 2 ] ) / det;
	r.m[ 1 ][ 1 ] = ( m[ 0 ][ 0 ] * m[ 2 ][ 2 ] - m[ 0 ][ 2 ] * m[ 2 ][ 0 ] ) / det;
	r.m[ 1 ][ 2 ] = ( m[ 0 ][ 2 ] * m[ 1 ][ 0 ] - m[ 0 ][ 0 ] * m[ 1 ][ 2 ] ) / det;
	r.m[ 2 ][ 0 ] = ( m[ 1 ][ 0 ] * m[ 2 ][ 1 ] - m[ 1 ][ 1 ] * m[ 2 ][ 0 ] ) / det;
	r.m[ 2 ][ 1 ] = ( m[ 0 ][ 1 ] * m[ 2 ][ 0 ] - m[ 0 ][ 0 ] * m[ 2 ][ 1 ] ) / det;
	r.m[ 2 ][ 2 ] = ( m[ 0 ][ 0 ] * m[ 1 ][ 1 ] - m[ 0 ][ 1 ] * m[ 1 ][ 0 ] ) / det;
	return r;
}

void Apply3( const Mat3& a, const double in[ 3 ], double out[ 3 ] )
{
	for( int i = 0; i < 3; ++i )
		out[ i ] = a.m[ i ][ 0 ] * in[ 0 ] + a.m[ i ][ 1 ] * in[ 1 ] + a.m[ i ][ 2 ] * in[ 2 ];
}

/// Linear sRGB to XYZ (Y of white = 1), from the primaries and D65.
const Mat3& RgbToXyz()
{
	static const Mat3 m = [] {
		const double prim[ 3 ][ 2 ] = { { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 } };
		const double wx = 0.3127, wy = 0.3290;
		Mat3 p{};
		for( int c = 0; c < 3; ++c )
		{
			const double x = prim[ c ][ 0 ], y = prim[ c ][ 1 ];
			p.m[ 0 ][ c ] = x / y;
			p.m[ 1 ][ c ] = 1.0;
			p.m[ 2 ][ c ] = ( 1.0 - x - y ) / y;
		}
		const double w[ 3 ] = { wx / wy, 1.0, ( 1.0 - wx - wy ) / wy };
		double s[ 3 ];
		Apply3( Inverse( p ), w, s );
		Mat3 r{};
		for( int i = 0; i < 3; ++i )
			for( int c = 0; c < 3; ++c )
				r.m[ i ][ c ] = p.m[ i ][ c ] * s[ c ];
		return r;
	}();
	return m;
}

double PowerSumDb( const double* db, int n )
{
	double p = 0.0;
	for( int i = 0; i < n; ++i )
		p += std::pow( 10.0, db[ i ] / 10.0 );
	return p > 0.0 ? 10.0 * std::log10( p ) : -400.0;
}

double AmplitudeSumDb( const double* db, int n )
{
	double a = 0.0;
	for( int i = 0; i < n; ++i )
		a += std::pow( 10.0, db[ i ] / 20.0 );
	return a > 0.0 ? 20.0 * std::log10( a ) : -400.0;
}
} // namespace

// ---------------------------------------------------------------------------
Negatives& Neg()
{
	static Negatives n;
	return n;
}

int ColumnCount( VfdBands bands )
{
	return bands == VfdBands::Octaves ? 10 : kMaxColumns;
}

int ColumnX( VfdBands bands, int column )
{
	// Thirds: x = -17 (20 Hz) .. 13 (20 kHz). Octaves: the ISO octaves 31.5 Hz
	// .. 16 kHz, whose centres are every third third from x = -15.
	return bands == VfdBands::Octaves ? -15 + 3 * column : -17 + column;
}

double SrgbDecode( double c )
{
	const double a = std::fabs( c );
	const double l = a <= 0.04045 ? a / 12.92 : std::pow( ( a + 0.055 ) / 1.055, 2.4 );
	return c < 0.0 ? -l : l;
}

double SrgbEncode( double l )
{
	const double a = std::fabs( l );
	const double c = a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow( a, 1.0 / 2.4 ) - 0.055;
	return l < 0.0 ? -c : c;
}

void LinearToXy( const double rgb[ 3 ], double& x, double& y )
{
	double xyz[ 3 ];
	Apply3( RgbToXyz(), rgb, xyz );
	const double s = xyz[ 0 ] + xyz[ 1 ] + xyz[ 2 ];
	x = s > 0.0 ? xyz[ 0 ] / s : 0.0;
	y = s > 0.0 ? xyz[ 1 ] / s : 0.0;
}

void PhosphorLinear( double rgb[ 3 ] )
{
	// (0.24, 0.41) at Y = 1, into linear sRGB. Its red comes out negative: the
	// colour is outside sRGB. Add white until it is not -- the gamut boundary
	// on the line toward D65 -- and scale the peak channel to 1.
	const double xyz[ 3 ] = { kPhosphorX / kPhosphorY, 1.0, ( 1.0 - kPhosphorX - kPhosphorY ) / kPhosphorY };
	double       lin[ 3 ];
	Apply3( Inverse( RgbToXyz() ), xyz, lin );
	const double lift = std::max( 0.0, -std::min( { lin[ 0 ], lin[ 1 ], lin[ 2 ] } ) );
	double peak = 0.0;
	for( double& c : lin )
	{
		c += lift;
		peak = std::max( peak, c );
	}
	for( int i = 0; i < 3; ++i )
		rgb[ i ] = std::max( 0.0, lin[ i ] / peak );
}

int LitFor( double levelDb, double referenceDbfs )
{
	const double steps = ( levelDb - referenceDbfs + kStepDb * kRows ) / kStepDb;
	if( !( steps >= 0.0 ) )
		return 0;
	return std::min( kRows, static_cast< int >( std::floor( steps ) ) + 1 );
}

// ---------------------------------------------------------------------------
void Display::Reset()
{
	for( Column& c : cols )
		c = Column{};
	started = false;
}

void Display::Advance( const earshot::Frame* frame, const DisplaySettings& s )
{
	if( s.bands != bands )
	{
		bands = s.bands;
		Reset();
	}
	count = ColumnCount( bands );

	// Nothing to show: no engine, or Resolume's FFT (which has no thirds; the
	// Bin Law forbids inventing them from 64 unmeasured bins).
	if( frame == nullptr || frame->thirdCount <= 0 || frame->source != earshot::Source::Device )
	{
		Reset();
		return;
	}

	if( !started || frame->generation != gen || frame->audioTime < lastAudio )
	{
		Reset();
		started   = true;
		gen       = frame->generation;
		lastAudio = frame->audioTime;
	}
	double dt = frame->audioTime - lastAudio;
	lastAudio = frame->audioTime;
	if( Neg().holdInFrames )
		dt = 1.0 / 60.0;

	// The thirds by band index, found from each band's own exact centre so
	// nothing depends on earshot's ordering or on which bands fit the rate.
	bool   have[ kMaxColumns ] = {};
	double db[ kMaxColumns ]   = {};
	for( int i = 0; i < frame->thirdCount; ++i )
	{
		const auto& t = frame->thirds[ i ];
		if( !( t.exactHz > 0.0f ) )
			continue;
		const int x = static_cast< int >( std::lround( 10.0 * std::log10( static_cast< double >( t.exactHz ) / 1000.0 ) ) );
		const int k = x + 17;
		if( k >= 0 && k < kMaxColumns )
		{
			have[ k ] = true;
			db[ k ]   = t.db;
		}
	}

	for( int c = 0; c < count; ++c )
	{
		Column& col = cols[ c ];
		const int shown = c + Neg().shift;
		bool   present  = false;
		double level    = -400.0;
		if( shown >= 0 && shown < count )
		{
			const int k = ColumnX( bands, shown ) + 17;
			if( bands == VfdBands::Thirds )
			{
				present = k >= 0 && k < kMaxColumns && have[ k ];
				level   = present ? db[ k ] : -400.0;
			}
			else if( k >= 0 && k < kMaxColumns && have[ k ] )
			{
				// The power sum of the octave's three thirds (IS 6964, note 1
				// to 3.5: base-ten thirds combine into the octave with the
				// octave's own centre and edges). The 16 kHz octave at
				// 44.1 kHz has two of its three.
				double parts[ 3 ];
				int    n = 0;
				const int from = Neg().octaveWrong ? k : k - 1;
				for( int j = from; j <= from + 2; ++j )
					if( j >= 0 && j < kMaxColumns && have[ j ] )
						parts[ n++ ] = db[ j ];
				present = true;
				level   = Neg().amplitudeSum ? AmplitudeSumDb( parts, n ) : PowerSumDb( parts, n );
			}
		}

		col.present = present;
		if( !present )
		{
			col.levelDb = col.holdDb = -400.0;
			col.heldFor = 0.0;
			col.lit     = 0;
			col.dot     = -1;
			continue;
		}
		col.levelDb = level + s.trimDb;

		// The hold, in audio time. Follow the level up at once. The dot stays
		// while the bar still reaches the dot's segment; once it does not, wait
		// holdSeconds, then fall at fallDbPerSecond, never below the level.
		//
		// "While the bar reaches the dot's segment" is the display's own rule,
		// and it is not earshot's: `Third::holdDb` restarts its wait only when
		// the level is >= the hold value, so the wait starts at the level's
		// last maximum. At the end of a note that is before the bar drops
		// below the dot's 3 dB segment, so earshot's rule drops the dot early
		// by however long the bar took to fall out of the segment: with that
		// rule here (the holdAtPeakOnly negative) the dot held 1.375-1.433 s
		// of a 1.5 s Peak Hold after the bar left it (--hold, 24-144 fps).
		// earshot is not patched (AGENTS.md, traps).
		const bool reaches = LitFor( col.levelDb, s.referenceDbfs ) >= LitFor( col.holdDb, s.referenceDbfs );
		if( col.levelDb >= col.holdDb )
		{
			col.holdDb  = col.levelDb;
			col.heldFor = 0.0;
		}
		else if( reaches && !Neg().holdNoWait && !Neg().holdAtPeakOnly )
			col.heldFor = 0.0;
		else if( dt > 0.0 )
		{
			col.heldFor += dt;
			const double over = col.heldFor - ( Neg().holdNoWait ? 0.0 : s.holdSeconds );
			if( over > 0.0 )
				col.holdDb = std::max( col.levelDb, col.holdDb - s.fallDbPerSecond * std::min( dt, over ) );
		}

		col.lit = LitFor( col.levelDb, s.referenceDbfs );
		col.dot = -1;
		if( s.holdSeconds > 0.0 )
		{
			const int d = LitFor( col.holdDb, s.referenceDbfs ) - 1;
			if( d >= col.lit )
				col.dot = d;
		}
	}
}

// ---------------------------------------------------------------------------
bool Input::negListAtConstruction = false;

void Input::SetBackendFactoryForTest( BackendFactory factory )
{
	g_factory = factory;
}

Input::Counters& Input::Calls()
{
	static Counters c;
	return c;
}

Input::Input()
{
	// The menu that needs no enumeration: None, System default, spares. The
	// devices arrive the first time the VFD is drawn.
	slots.push_back( { kHostLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, earshot::kDefaultDevice, earshot::deviceKey( earshot::kDefaultDevice ) } );
	while( static_cast< int >( slots.size() ) < kInputSlots )
		slots.push_back( { kEmptySlot, "", 0.0f } );

	if( negListAtConstruction )
		List();
}

Input::~Input() = default;

void Input::BuildEngine()
{
	if( engine )
		return;
	Calls().engines.fetch_add( 1 );
	earshot::Config c;
	c.maxChannels = 2;
	if( g_factory != nullptr )
		engine = std::make_unique< earshot::Engine >( c, g_factory() );
	else
		engine = std::make_unique< earshot::Engine >( c );
	if( !engine->ok() )
		diag::error( "earshot could not allocate: the VFD will stay dark" );
}

void Input::List()
{
	Calls().lists.fetch_add( 1 );
	std::vector< earshot::DeviceInfo > devices;
	if( g_factory != nullptr )
	{
		// A throwaway backend of the harness's kind: it lists, it opens nothing.
		if( auto lister = g_factory() )
			devices = lister->enumerate();
	}
	else if( !DevicesDisabled() )
		devices = earshot::listDevices();

	slots.clear();
	slots.push_back( { kHostLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, earshot::kDefaultDevice, earshot::deviceKey( earshot::kDefaultDevice ) } );
	for( const earshot::DeviceInfo& d : devices )
	{
		if( static_cast< int >( slots.size() ) >= kInputSlots )
			break;
		const float key = earshot::deviceKey( d.name.c_str() );
		seen[ key ]     = d.name;
		slots.push_back( { d.name.substr( 0, 30 ), d.name, key } );
	}
	while( static_cast< int >( slots.size() ) < kInputSlots )
		slots.push_back( { kEmptySlot, "", 0.0f } );
	if( Neg().menuByIndex )
		for( size_t i = 0; i < slots.size(); ++i )
			slots[ i ].value = static_cast< float >( i );
	listed = true;
	++listCount;
	diag::info( "VFD audio inputs: " + std::to_string( devices.size() ) + " device(s) listed" );
}

void Input::Want( float key )
{
	wanted = key;
	if( active )
		Apply();
}

void Input::Activate()
{
	if( active )
		return;
	active = true;
	BuildEngine();
	if( !listed )
		List();
	Apply();
}

void Input::Deactivate()
{
	if( !active )
		return;
	active = false;
	if( engine && applied != 0.0f )
	{
		Calls().selects.fetch_add( 1 );
		engine->selectDevice( nullptr );
		applied = 0.0f;
		diag::info( "VFD audio input closed: Type left VFD" );
	}
}

bool Input::Rescan()
{
	if( !active )
		return false;
	List();
	// A remembered device that was missing may be back: ask again.
	if( wanted != 0.0f && engine )
	{
		Calls().selects.fetch_add( 1 );
		engine->selectDevice( nullptr );
		applied = 0.0f;
		Apply();
	}
	return true;
}

void Input::Apply()
{
	if( !engine || wanted == applied )
		return;
	std::string name;
	if( wanted == 0.0f )
		name.clear();
	else if( wanted == earshot::deviceKey( earshot::kDefaultDevice ) )
		name = earshot::kDefaultDevice;
	else
	{
		// A key with no device met in this process: ask for a name that cannot
		// match, so earshot says Missing (never the default). Rescan resolves
		// it once the device is back.
		const auto known = seen.find( wanted );
		name = known != seen.end() ? known->second
								   : "(device " + std::to_string( static_cast< long >( wanted ) ) + " not present)";
	}
	applied = wanted;
	if( !name.empty() && DevicesDisabled() )
	{
		diag::info( "VFD audio input refused (NEEDLE_NO_DEVICES): " + name );
		return;
	}
	Calls().selects.fetch_add( 1 );
	engine->selectDevice( name.c_str() );
	diag::info( "VFD audio input: " + ( name.empty() ? std::string( "host FFT" ) : name ) );
}

const earshot::Frame* Input::Update( double hostSeconds, const float* fft, int fftCount )
{
	if( !engine )
		return nullptr;
	const earshot::Frame& f = engine->update( hostSeconds, fft, fftCount );
	lastFromDevice          = f.source == earshot::Source::Device;
	return &f;
}

Lamp Input::LampState() const
{
	if( !engine )
		return Lamp::Amber;
	switch( engine->captureState() )
	{
	case earshot::CaptureState::Silent: return Lamp::Red;
	case earshot::CaptureState::Missing:
	case earshot::CaptureState::Failed: return Lamp::RedBlink;
	default: break;
	}
	return lastFromDevice ? Lamp::Green : Lamp::Amber;
}

void Input::FeedBegin( int sampleRate, int channels )
{
	BuildEngine();
	engine->feedBegin( sampleRate, channels );
}

void Input::Feed( const float* interleaved, int frames )
{
	if( engine )
		engine->feed( interleaved, frames );
}

// ---------------------------------------------------------------------------
void Picture::SegmentCentre( int column, int fromBottom, float& lx, float& ly ) const
{
	const int row = rows - 1 - fromBottom;
	lx = field[ 0 ] + ( static_cast< float >( column ) + 0.5f ) * field[ 2 ];
	ly = field[ 1 ] + ( static_cast< float >( row ) + 0.5f ) * field[ 3 ];
}

} // namespace needle::vfd
