#include "Needle.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Audio.h"
#include "Diag.h"
#include "Font.h"

namespace needle
{
static_assert( PT_COUNT_ - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
			   "the About block's size changed with the generated header -- "
			   "add or remove a PT_ABOUT_BUTTON_n to match" );

namespace
{
constexpr float kPi = 3.14159265358979323846f;

/// Turn a normalised option parameter back into an index. Resolume hands option
/// parameters back as the element *value*, which for these is the index
/// already, but a host that normalises would give 0..1 -- so both are accepted
/// and clamped. Getting this wrong silently selects mode 0 for ever.
int ToOption( float v, int count )
{
	if( count <= 1 )
		return 0;
	int i = ( v <= 1.0f && count > 2 && v != std::floor( v ) )
				? static_cast< int >( v * static_cast< float >( count - 1 ) + 0.5f )
				: static_cast< int >( v + 0.5f );
	return std::min( std::max( i, 0 ), count - 1 );
}

bool Bool( float v )
{
	return v > 0.5f;
}

void AppendText( std::vector< int >& out, const char* s )
{
	for( const char* p = s; *p != '\0'; ++p )
	{
		const unsigned char c = static_cast< unsigned char >( *p );
		out.push_back( ( c >= 32 && c < 127 ) ? static_cast< int >( c ) : 63 );
	}
}

/// One tick on a dial, described in the units the scale is marked in.
struct MarkSpec
{
	double      db;
	const char* label;///< nullptr for an unlabelled minor tick
};

// A VU scale, as it is actually printed: every decibel near the top where the
// operator works, and a long gap down to -20 where nobody does. The labelled
// marks are the ones a real face prints large.
constexpr MarkSpec kVuMarks[] = {
	{ -20.0, "20" }, { -10.0, "10" }, { -7.0, "7" }, { -5.0, "5" }, { -3.0, "3" },
	{ -2.0, nullptr }, { -1.0, nullptr }, { 0.0, "0" }, { 1.0, nullptr },
	{ 2.0, nullptr }, { 3.0, "3" },
};

// A BBC PPM: seven evenly spaced marks, 4 dB apart, numbered 1 to 7, and no red
// band anywhere -- a PPM says where the peak is and leaves the judgement to the
// operator.
constexpr MarkSpec kPpmMarks[] = {
	{ -12.0, "1" }, { -8.0, "2" }, { -4.0, "3" }, { 0.0, "4" },
	{ 4.0, "5" }, { 8.0, "6" }, { 12.0, "7" },
};
} // namespace

NeedlePlugin::NeedlePlugin()
{
	static std::atomic< int > sNextInstance{ 1 };
	mInstanceId = sNextInstance.fetch_add( 1 );
	mTag        = "[" + std::to_string( mInstanceId ) + "] ";

	// A source: no inputs at all. Resolume decides where a plugin appears in
	// its browser from this and nothing else.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	auto group = [ this ]( unsigned int from, unsigned int to, const char* name ) {
		for( unsigned int i = from; i <= to; ++i )
			SetParamGroup( i, name );
	};
	auto option = [ this ]( unsigned int id, const char* name,
							std::initializer_list< const char* > elements, float def ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( elements.size() ), def );
		unsigned int i = 0;
		for( const char* e : elements )
		{
			SetParamElementInfo( id, i, e, static_cast< float >( i ) );
			++i;
		}
		mParams[ id ] = def;
	};
	auto boolean = [ this ]( unsigned int id, const char* name, bool def ) {
		SetParamInfo( id, name, FF_TYPE_BOOLEAN, def );
		mParams[ id ] = def ? 1.0f : 0.0f;
	};
	auto standard = [ this ]( unsigned int id, const char* name, float def ) {
		SetParamInfo( id, name, FF_TYPE_STANDARD, def );
		mParams[ id ] = def;
	};
	auto colour = [ this ]( unsigned int first, const char* label, float r, float g, float b ) {
		// SetParamInfo copies the name into a std::string, so temporaries are fine.
		const std::string base = label;
		SetParamInfo( first + 0, ( base + " Red" ).c_str(), FF_TYPE_RED, r );
		SetParamInfo( first + 1, ( base + " Green" ).c_str(), FF_TYPE_GREEN, g );
		SetParamInfo( first + 2, ( base + " Blue" ).c_str(), FF_TYPE_BLUE, b );
		mParams[ first + 0 ] = r;
		mParams[ first + 1 ] = g;
		mParams[ first + 2 ] = b;
	};

	// Declaration order is the order the host draws these, and SetParamGroup
	// collapses runs -- so an id moved out of its run splits its group in two.

	// -- Meter -----------------------------------------------------------------
	option( PT_TYPE, "Type", { "VU", "PPM", "Bargraph", "Magic Eye" }, 0.0f );
	option( PT_COUNT, "Count", { "Mono", "Stereo Pair" }, 0.0f );
	standard( PT_REFERENCE, "Reference Level", kReferenceDefault );
	standard( PT_SENSITIVITY, "Sensitivity", kSensitivityDefault );
	option( PT_BIN_LAW, "Bin Law", { "Magnitude", "Power" }, 0.0f );
	boolean( PT_STANDARD, "Standard", true );
	group( PT_TYPE, PT_STANDARD, "Meter" );

	// -- Ballistics ------------------------------------------------------------
	// Every default here is the value at which Free agrees with Standard, so
	// the switch itself changes nothing until something else is moved.
	standard( PT_RISE, "Rise", kRiseDefault );
	standard( PT_FALL, "Fall", kFallDefault );
	standard( PT_OVERSHOOT, "Overshoot", kOvershootDefault );
	standard( PT_PEAK_HOLD, "Peak Hold", kPeakHoldDefault );
	standard( PT_HOLD_DECAY, "Hold Decay", kHoldDecayDefault );
	group( PT_RISE, PT_HOLD_DECAY, "Ballistics" );

	// -- Look ------------------------------------------------------------------
	colour( PT_FACE_R, "Face", 0.93f, 0.90f, 0.79f );
	colour( PT_NEEDLE_R, "Needle", 0.10f, 0.09f, 0.08f );
	option( PT_SCALE_STYLE, "Scale Style", { "Full", "Marks Only", "Plain", "None" }, 0.0f );
	standard( PT_LAMP, "Lamp", 0.35f );
	standard( PT_GLASS, "Glass", 0.25f );
	standard( PT_WEAR, "Wear", 0.0f );
	standard( PT_PERSISTENCE, "Persistence", kPersistenceDefault );
	group( PT_FACE_R, PT_PERSISTENCE, "Look" );

	// -- Layout ----------------------------------------------------------------
	standard( PT_SIZE, "Size", kSizeDefault );
	standard( PT_POS_X, "Position X", 0.5f );
	standard( PT_POS_Y, "Position Y", 0.5f );
	standard( PT_ROTATION, "Rotation", 0.5f );
	standard( PT_BACKGROUND, "Background", 0.0f );
	colour( PT_BACK_R, "Back", 0.04f, 0.04f, 0.05f );
	standard( PT_MIX, "Mix", 1.0f );
	group( PT_SIZE, PT_MIX, "Layout" );

	// -- Audio -----------------------------------------------------------------
	// Declared with a real element list so the host knows how many bins to
	// fill. It is the only parameter here the operator does not set.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	group( PT_AUDIO, PT_AUDIO, "Audio" );

	// -- About -----------------------------------------------------------------
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	group( PT_ABOUT_TEXT, PT_COUNT_ - 1, "About" );

	mEngine.SetSettings( CurrentSettings() );
}

FFResult NeedlePlugin::InitGL( const FFGLViewportStruct* vp )
{
	// Idempotent: a host may call this again, and the offline harness does.
	if( mGlReady )
		return CFFGLPlugin::InitGL( vp );

	diag::init();

	auto glString = []( GLenum name ) {
		const GLubyte* s = glGetString( name );
		return s != nullptr ? std::string( reinterpret_cast< const char* >( s ) )
							: std::string( "?" );
	};
	diag::info( mTag + "GL vendor=" + glString( GL_VENDOR ) + " renderer=" + glString( GL_RENDERER ) +
				" version=" + glString( GL_VERSION ) );

	{
		char buffer[ 192 ];
		std::snprintf( buffer, sizeof( buffer ),
					   "ANSI C16.5 solved: zeta=%.6f wn=%.4f rad/s (%.4f Hz); "
					   "IEC 268-10 II: rise=%.4f ms fall=%.4f s",
					   standards::VuDamping(), standards::VuNaturalFrequency(),
					   standards::VuNaturalFrequency() / ( 2.0 * kPi ),
					   standards::PpmRiseTau() * 1000.0, standards::PpmFallTau() );
		diag::info( mTag + buffer );
	}

	if( !mRenderer.InitGL() )
	{
		diag::error( mTag + "InitGL failed: " + mRenderer.Note() );
		return FF_FAIL;
	}
	mGlReady = true;
	return CFFGLPlugin::InitGL( vp );
}

FFResult NeedlePlugin::DeInitGL()
{
	mRenderer.DeInitGL();
	mGlReady = false;
	return FF_SUCCESS;
}

FFResult NeedlePlugin::SetTime( double time )
{
	mHostTimeSeen = true;
	mHostTime     = time;
	return CFFGLPlugin::SetTime( time );
}

FFResult NeedlePlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT_ )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	mParams[ index ] = value;
	return FF_SUCCESS;
}

float NeedlePlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT_ ? mParams[ index ] : 0.0f;
}

FFResult NeedlePlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Must return FF_SUCCESS for the About block, or no host can instantiate
	// the plugin at all: the base class fails an unknown text parameter, and a
	// host that sets one during instantiation treats that as the plugin
	// refusing to load.
	(void)value;
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return FF_FAIL;
}

char* NeedlePlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return const_cast< char* >( "" );
}

int NeedlePlugin::OptionIndex( unsigned int param, int count ) const
{
	return ToOption( mParams[ param ], count );
}

Settings NeedlePlugin::CurrentSettings() const
{
	Settings s;
	s.type     = static_cast< MeterType >( OptionIndex( PT_TYPE, kMeterTypeCount ) );
	s.channels = OptionIndex( PT_COUNT, 2 ) + 1;

	s.referenceDbfs = ReferenceDbfs( mParams[ PT_REFERENCE ] );
	s.trimDb        = TrimDb( mParams[ PT_SENSITIVITY ] );
	s.standard      = Bool( mParams[ PT_STANDARD ] );

	s.riseSeconds = RiseSeconds( mParams[ PT_RISE ] );
	s.fallSeconds = FallSeconds( mParams[ PT_FALL ] );
	s.overshoot   = OvershootFraction( mParams[ PT_OVERSHOOT ] );

	s.holdSeconds          = PeakHoldSeconds( mParams[ PT_PEAK_HOLD ] );
	s.holdDecayDbPerSecond = HoldDecayDbPerSecond( mParams[ PT_HOLD_DECAY ] );
	s.persistenceSeconds   = PersistenceSeconds( mParams[ PT_PERSISTENCE ] );
	return s;
}

float NeedlePlugin::InputLevel() const
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return 0.0f;

	float bins[ audio::kBins ] = { 0.0f };
	const int n = std::min( audio::kBins, static_cast< int >( info->elements.size() ) );
	for( int i = 0; i < n; ++i )
		bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;

	const bool power = OptionIndex( PT_BIN_LAW, kBinLawCount ) == static_cast< int >( BinLaw::Power );
	return audio::LevelFromSpectrum( bins, n, power );
}

void NeedlePlugin::AdvanceEngine( double hostSeconds )
{
	mEngine.SetSettings( CurrentSettings() );

	// One spectrum arrives per frame and it is the programme, not a channel --
	// FFGL 2.1 has exactly one FF_USAGE_FFT buffer to offer. A stereo pair
	// therefore shows the same level twice: it is the instrument people
	// recognise rather than two measurements, and the README says so.
	const double level = InputLevel();
	const double a[ Engine::kMaxChannels ] = { level, level };
	mEngine.Frame( hostSeconds, a, CurrentSettings().channels );
}

void NeedlePlugin::LocalToScreen( const Unit& unit, float lx, float ly, float& sx, float& sy )
{
	const float cs = std::cos( unit.rotation );
	const float sn = std::sin( unit.rotation );
	sx = unit.centre[ 0 ] + lx * cs - ly * sn;
	sy = unit.centre[ 1 ] + lx * sn + ly * cs;
}

Frame NeedlePlugin::BuildFrame( int width, int height ) const
{
	Frame f;
	f.width  = width;
	f.height = height;

	const Settings settings = CurrentSettings();
	f.type  = static_cast< int >( settings.type );
	f.count = settings.channels;
	f.style = OptionIndex( PT_SCALE_STYLE, kScaleStyleCount );

	for( int i = 0; i < 3; ++i )
	{
		f.face[ i ]   = mParams[ PT_FACE_R + static_cast< unsigned >( i ) ];
		f.needle[ i ] = mParams[ PT_NEEDLE_R + static_cast< unsigned >( i ) ];
		f.back[ i ]   = mParams[ PT_BACK_R + static_cast< unsigned >( i ) ];
	}
	f.backAlpha = mParams[ PT_BACKGROUND ];
	f.lamp      = mParams[ PT_LAMP ];
	f.glass     = mParams[ PT_GLASS ];
	f.wear      = mParams[ PT_WEAR ];
	f.mix       = mParams[ PT_MIX ];

	const float minDim = static_cast< float >( std::min( width, height ) );
	const float S      = SizeFraction( mParams[ PT_SIZE ] ) * minDim * 0.5f;
	const float rot    = RotationRadians( mParams[ PT_ROTATION ] );

	// A VU window is a letterbox, not a square: 1.55 puts the arc's chord at
	// 82 % of the width at the sweep above.
	float aspect = 1.55f;
	if( settings.type == MeterType::Bargraph )
		aspect = 0.34f;
	else if( settings.type == MeterType::Eye )
		aspect = 1.0f;

	const float halfH = S;
	const float halfW = S * aspect;

	const float assemblyX = static_cast< float >( width ) * 0.5f + Offset( mParams[ PT_POS_X ] ) * minDim * 0.5f;
	const float assemblyY = static_cast< float >( height ) * 0.5f + Offset( mParams[ PT_POS_Y ] ) * minDim * 0.5f;
	const float cs = std::cos( rot );
	const float sn = std::sin( rot );
	const float gap = halfW * 0.14f;

	const Scale scale    = ScaleFor( settings.type );

	for( int u = 0; u < f.count; ++u )
	{
		Unit& U = f.unit[ u ];

		// Instruments sit side by side ALONG THE LOCAL X AXIS, so a rotated
		// pair tilts together like one panel rather than orbiting the centre.
		const float dx = ( f.count == 1 ) ? 0.0f
										  : ( u == 0 ? -( halfW + gap ) : ( halfW + gap ) );
		U.centre[ 0 ] = assemblyX + dx * cs;
		U.centre[ 1 ] = assemblyY + dx * sn;
		U.rotation    = rot;
		U.halfW       = halfW;
		U.halfH       = halfH;

		const ChannelState& st = mEngine.Channel( u );

		if( settings.type == MeterType::Bargraph )
		{
			U.corner = S * 0.08f;

			const float colHalfW = halfW * 0.64f;
			const float top      = -halfH * 0.90f;
			const float span     = halfH * 1.80f;
			const float pitch    = span / static_cast< float >( standards::kBargraphSteps );
			const float gapFrac  = 0.22f;

			for( int k = 0; k < standards::kBargraphSteps; ++k )
			{
				// k counts from the BOTTOM, as the thresholds do; on screen
				// that is the (kBargraphSteps - 1 - k)th row from the top.
				const int row = standards::kBargraphSteps - 1 - k;
				U.seg[ k ][ 0 ] = -colHalfW;
				U.seg[ k ][ 1 ] = top + static_cast< float >( row ) * pitch + pitch * gapFrac * 0.5f;
				U.seg[ k ][ 2 ] = colHalfW * 2.0f;
				U.seg[ k ][ 3 ] = pitch * ( 1.0f - gapFrac );
			}
			U.segLit = st.litSegments;

			if( st.holdDeflection > 0.0 )
			{
				const float h = std::max( 2.0f, pitch * 0.22f );
				const float y = top + ( 1.0f - static_cast< float >( st.holdDeflection ) ) * span;
				U.hold[ 0 ]   = -colHalfW;
				U.hold[ 1 ]   = std::min( std::max( y - h * 0.5f, top ), top + span - h );
				U.hold[ 2 ]   = colHalfW * 2.0f;
				U.hold[ 3 ]   = h;
			}
		}
		else if( settings.type == MeterType::Eye )
		{
			U.corner     = S * 0.18f;
			U.eyeRadius  = S * 0.72f;
			U.eyeShadow  = static_cast< float >( st.eyeShadowDeg * 0.5 * kPi / 180.0 );
			U.eyeOverlap = static_cast< float >( st.eyeOverlapDeg * 0.5 * kPi / 180.0 );
			U.eyeWarm    = static_cast< float >( st.eyeWarm );
		}
		else
		{
			// The proportions of a moving-coil meter, and they are not free
			// either. A VU's scale arc is SHALLOW -- it crosses the top of the
			// window nearly flat -- and that is a consequence of two things
			// that are fixed: the pointer sweeps about 54 degrees end to end,
			// and the arc has to span most of the window's width. Those two
			// together force a radius near three times the window's half
			// height, which puts the pivot well below the glass. Hence the
			// mask in drawDial: on a real meter the hub is behind the bezel.
			U.corner       = S * 0.09f;
			U.pivot[ 0 ]   = 0.0f;
			U.pivot[ 1 ]   = S * 2.35f;
			U.arcRadius    = S * 2.80f;
			U.arcHalfAngle = 27.0f * kPi / 180.0f;
			U.needleLen    = U.arcRadius * 0.970f;
			// Wide enough that the pixel check can probe its centreline and
			// find coverage of exactly 1.0 -- see tools/ndtest/main.mm. A
			// pointer thinner than about two and a half pixels is a pointer
			// whose colour depends on the rasteriser.
			U.needleWidth  = std::max( 3.0f, S * 0.026f );

			// A needle that has run out of scale rests on its stop, a hair past
			// the last mark. It does not leave the face and it does not stop
			// exactly on the mark, because neither does a real one.
			const double d = std::min( std::max( st.deflection, -0.02 ), 1.035 );
			U.needleAngle  = static_cast< float >( ( 2.0 * d - 1.0 ) * U.arcHalfAngle );

			if( settings.type == MeterType::Vu )
			{
				// Where 0 VU falls on a scale that is linear in VOLTAGE:
				// 1 / 10^(3/20) = 70.8 % of full deflection, which is the
				// familiar "0 VU sits about three quarters along". On a scale
				// that was linear in dB it would sit at 87 %, and the red band
				// would start in the wrong place on every face ever printed.
				const double zero = DeflectionFor( scale, 1.0 );
				U.redFrom = static_cast< float >( ( 2.0 * zero - 1.0 ) * U.arcHalfAngle );
			}
			else
			{
				// A BBC PPM has no red band at all: it says where the peak is
				// and leaves the judgement to the operator. Anything at or
				// above the arc's half angle is the sentinel for "none".
				U.redFrom = 10.0f;
			}
		}
	}

	// -- the scale ------------------------------------------------------------
	if( settings.type == MeterType::Vu || settings.type == MeterType::Ppm )
	{
		const MarkSpec* specs = ( settings.type == MeterType::Vu ) ? kVuMarks : kPpmMarks;
		const int       n     = ( settings.type == MeterType::Vu )
									? static_cast< int >( sizeof( kVuMarks ) / sizeof( kVuMarks[ 0 ] ) )
									: static_cast< int >( sizeof( kPpmMarks ) / sizeof( kPpmMarks[ 0 ] ) );

		const float alpha = f.unit[ 0 ].arcHalfAngle;
		for( int m = 0; m < n && static_cast< int >( f.marks.size() ) < Frame::kMaxMarks; ++m )
		{
			// A mark is specified in decibels; where it lands is whatever
			// deflection that level produces. `DeflectionFor` is the same
			// function the engine drives the pointer with, so the mark and the
			// pointer cannot disagree about what a decibel is -- which is the
			// only bug that matters on a scale.
			const double where =
				DeflectionFor( scale, std::pow( 10.0, specs[ m ].db / 20.0 ) );

			Mark mark;
			mark.angle      = static_cast< float >( ( 2.0 * where - 1.0 ) * alpha );
			mark.lengthFrac = specs[ m ].label != nullptr ? 1.0f : 0.55f;
			mark.red        = ( settings.type == MeterType::Vu && specs[ m ].db >= 0.0 ) ? 1 : 0;
			if( specs[ m ].label != nullptr &&
				f.text.size() + std::strlen( specs[ m ].label ) <= Frame::kMaxText )
			{
				mark.textOffset = static_cast< int >( f.text.size() );
				AppendText( f.text, specs[ m ].label );
				mark.textLength = static_cast< int >( f.text.size() ) - mark.textOffset;
			}
			f.marks.push_back( mark );
		}

		const char* legend = ( settings.type == MeterType::Vu ) ? "VU" : "PPM";
		if( f.text.size() + std::strlen( legend ) <= Frame::kMaxText )
		{
			f.legendOffset = static_cast< int >( f.text.size() );
			AppendText( f.text, legend );
			f.legendLength = static_cast< int >( f.text.size() ) - f.legendOffset;
		}
		f.legendScale   = std::max( 1, static_cast< int >( std::lround( S / 42.0f ) ) );
		f.markTextScale = std::max( 1, static_cast< int >( std::lround( S / 64.0f ) ) );

		const float lw = static_cast< float >( f.legendLength * 6 * f.legendScale - f.legendScale );
		f.legendPos[ 0 ] = std::floor( -lw * 0.5f );
		f.legendPos[ 1 ] = std::floor( S * 0.42f );
	}

	return f;
}

FFResult NeedlePlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL == nullptr || !mGlReady )
		return FF_FAIL;

	mClock.Tick( mHostTime, mHostTimeSeen );
	diag::stateChanged( mTag + "clock", mTag + "host clock is " + mClock.Unit() );

	const int width  = static_cast< int >( currentViewport.width );
	const int height = static_cast< int >( currentViewport.height );
	if( width <= 0 || height <= 0 )
		return FF_SUCCESS;

	AdvanceEngine( mClock.Seconds() );

	// A meter that never moves is the commonest support question there is, and
	// from the front it is indistinguishable from a broken plugin. One line per
	// transition says which it is.
	{
		const float level = InputLevel();
		diag::stateChanged( mTag + "audio", mTag + ( level > 1e-6f
														 ? "audio present on the layer"
														 : "no audio on this layer -- every FFT bin is zero" ) );
	}

	const Frame frame = BuildFrame( width, height );
	mRenderer.Draw( frame, pGL->HostFBO );
	return FF_SUCCESS;
}

} // namespace needle
