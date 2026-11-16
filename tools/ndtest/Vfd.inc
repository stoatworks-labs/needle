//===========================================================================
// The VFD (Type = VFD): the scripted device, the feed, and the checks.
//
// Included by main.mm after --bench. Every claim below is in
// specs/SPEC-needle-vfd.md with its source; this file measures them.
//
// ## How audio gets in
//
// Through earshot's own `feed()`, on the plugin's own `vfd::Input`, so what
// moves a bar is the shipped engine, the shipped column map and the shipped
// hold. No check opens an audio device: `main` installs a scripted capture
// backend (below) before any plugin exists, so even the device menu lists
// scripted devices. The one exception is `--coreaudio`, which must run the
// real backend to prove the plugin never reaches it with the VFD off.
//
// A signal here is a function of the sample index, evaluated in double, so it
// never depends on how the frames chop it: a 24 fps run and a 144 fps run feed
// exactly the same samples, and differ only in where the updates fall.
//
// ## What a check may read
//
//   * the band edges, centres and levels: earshot's Frame, through the plugin's
//     Input (the bank is earshot's; this harness never re-derives a response);
//   * the columns: the plugin's Display, and the picture;
//   * the bounds: IS 6964 (IEC 61260:1995) Tables 1, A.1 and B.1 and its
//     eq. (12), typed in below from the standard, never from the code.
//
// ## Two rasters, two renderers
//
// Every check with a picture runs at 1280x720 (default Size) and 320x180
// (Size 1), from geometry the plugin itself produced (`BuildVfdPicture`).
// `NDTEST_RENDERER=software` runs any of them on Apple's software renderer.
//===========================================================================
namespace
{
//---------------------------------------------------------------------------
// The scripted capture device. Nothing here talks to the OS: earshot's worker
// calls open() and close(), and a thread of ours delivers the audio, four
// times faster than real time (woofer's backend, the same shape).
//---------------------------------------------------------------------------
std::mutex                 g_deviceMutex;
std::vector< std::string > g_deviceNames = { "Test Mic", "Test Silent" };
std::atomic< int >         g_opens{ 0 };

class ScriptedBackend final : public earshot::testing::CaptureBackend
{
public:
	~ScriptedBackend() override { close(); }

	earshot::testing::OpenResult open( const char* name, earshot::testing::AudioSink& sink, char* openedName,
									   char* error ) noexcept override
	{
		close();
		std::string wanted = name;
		if( wanted == earshot::kDefaultDevice )
			wanted = "Test Mic";
		{
			std::lock_guard< std::mutex > lock( g_deviceMutex );
			if( std::find( g_deviceNames.begin(), g_deviceNames.end(), wanted ) == g_deviceNames.end() )
			{
				std::snprintf( error, 128, "%s is not connected (scripted)", wanted.c_str() );
				return earshot::testing::OpenResult::NotConnected;
			}
		}
		std::snprintf( openedName, 128, "%s", wanted.c_str() );
		++g_opens;
		sink.begin( 48000, 2 );
		running           = true;
		const bool silent = wanted == "Test Silent";
		worker            = std::thread( [ this, &sink, silent ] {
            std::vector< float > block( 2 * 480 );
            uint64_t             n = 0;
            while( running )
            {
                for( int k = 0; k < 480; ++k, ++n )
                {
                    const float v = silent ? 0.0f : static_cast< float >( 0.25 * std::sin( 2.0 * kPi * 1000.0 * n / 48000.0 ) );
                    block[ 2 * k ] = block[ 2 * k + 1 ] = v;
                }
                sink.deliver( block.data(), 480, 2 );
                std::this_thread::sleep_for( std::chrono::microseconds( 2500 ) );
            }
        } );
		return earshot::testing::OpenResult::Opened;
	}
	void close() noexcept override
	{
		running = false;
		if( worker.joinable() )
			worker.join();
	}
	std::vector< earshot::DeviceInfo > enumerate() noexcept override
	{
		std::lock_guard< std::mutex >      lock( g_deviceMutex );
		std::vector< earshot::DeviceInfo > list;
		for( const std::string& name : g_deviceNames )
			list.push_back( { name, 2, false } );
		return list;
	}

private:
	std::atomic< bool > running{ false };
	std::thread         worker;
};

std::unique_ptr< earshot::testing::CaptureBackend > ScriptedFactory()
{
	return std::make_unique< ScriptedBackend >();
}

//---------------------------------------------------------------------------
// Negative controls: NDTEST_NEGATIVE=name plants one bug, and the check it
// names must then FAIL. `--negative` runs every one in a child process.
//---------------------------------------------------------------------------
std::string g_negative;

bool NegIs( const char* name )
{
	return g_negative == name;
}

struct NegativeEntry
{
	const char* name;
	const char* check;
	const char* what;
};

const NegativeEntry kNegatives[] = {
	{ "thirdBase2", "--iec", "earshot: G = 2 instead of 10^(3/10)" },
	{ "thirdNoPrewarp", "--iec", "earshot: the band edges not pre-warped" },
	{ "thirdOrder2", "--skirt", "earshot: a second-order prototype (skirts too shallow)" },
	{ "shift", "--skirt", "column c shows band c + 1" },
	{ "amplitudeSum", "--octaves", "octaves summed in amplitude, not power" },
	{ "octaveWrong", "--octaves", "an octave from thirds x, x+1, x+2" },
	{ "holdInFrames", "--hold", "hold and fall counted in 60 fps frames" },
	{ "holdNoWait", "--hold", "the fall starts the moment the bar leaves the dot" },
	{ "holdAtPeakOnly", "--hold", "earshot's rule: the wait starts at the level's last maximum" },
	{ "normFullSlot", "--multiplex", "the camera normalised by the slot, not the grid's pulse" },
	{ "readoutUp", "--multiplex", "the rolling shutter read bottom to top" },
	{ "phaseFloat", "--clock", "the scan phase computed in float" },
	{ "wireCoreOnly", "--filament", "the wire's 24.5 um core without its oxide" },
	{ "wireTen", "--filament", "the wire ten times thick" },
	{ "willemite", "--phosphor", "the magic eye's willemite green instead of ZnO:Zn" },
	{ "segFull", "--geometry", "segments as wide as the column pitch" },
	{ "resetOnResize", "--resize", "the hold forgotten when the raster changes" },
	{ "menuByIndex", "--devices", "device elements valued by position, not by name" },
	{ "listAtConstruction", "--coreaudio", "woofer's shape: devices listed in the constructor" },
	{ "showAllOnVfd", "--visible", "every control shown on the VFD" },
};

void ArmNegative()
{
	const char* v = std::getenv( "NDTEST_NEGATIVE" );
	g_negative    = v != nullptr ? v : "";
	if( g_negative.empty() )
		return;
	vfd::Negatives& n = vfd::Neg();
	n.holdInFrames    = NegIs( "holdInFrames" );
	n.holdNoWait      = NegIs( "holdNoWait" );
	n.holdAtPeakOnly  = NegIs( "holdAtPeakOnly" );
	n.amplitudeSum    = NegIs( "amplitudeSum" );
	n.octaveWrong     = NegIs( "octaveWrong" );
	n.shift           = NegIs( "shift" ) ? 1 : 0;
	n.phaseFloat      = NegIs( "phaseFloat" );
	n.readoutUp       = NegIs( "readoutUp" );
	n.normFullSlot    = NegIs( "normFullSlot" );
	n.wireCoreOnly    = NegIs( "wireCoreOnly" );
	n.wireTen         = NegIs( "wireTen" );
	n.willemite       = NegIs( "willemite" );
	n.segFull         = NegIs( "segFull" );
	n.resetOnResize   = NegIs( "resetOnResize" );
	n.showAllOnVfd    = NegIs( "showAllOnVfd" );
	n.menuByIndex     = NegIs( "menuByIndex" );
	vfd::Input::negListAtConstruction = NegIs( "listAtConstruction" );
	std::fprintf( stderr, "ndtest: negative control %s planted\n", g_negative.c_str() );
}

/// Build the plugin's Engine (the VFD's own first-frame path), then plant any
/// earshot mutation before the stream begins: the bank is designed when a
/// stream starts, so a mutation set later would change nothing.
void ArmEngine( vfd::Input& in )
{
	in.Activate();
	if( earshot::Engine* e = in.EngineForTest() )
	{
		earshot::testing::Mutations& m = e->mutations();
		m.thirdBase2                   = NegIs( "thirdBase2" );
		m.thirdNoPrewarp               = NegIs( "thirdNoPrewarp" );
		m.thirdOrder2                  = NegIs( "thirdOrder2" );
	}
}

//---------------------------------------------------------------------------
// Signals and the feed.
//---------------------------------------------------------------------------
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

/// A sine with a 20 ms raised-cosine start (the switch-on would otherwise ring
/// every low band), from `start` seconds, cut to silence at `stop` if
/// `stop` >= 0.
Signal Sine( double hz, double amplitude, int rate, double stop = -1.0, double start = 0.0 )
{
	return [ = ]( long long k ) {
		const double t = static_cast< double >( k ) / rate;
		if( t < start || ( stop >= 0.0 && t >= stop ) )
			return 0.0;
		return amplitude * Fade( t - start, 0.02 ) * std::sin( 2.0 * kPi * hz * ( t - start ) );
	};
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

/// The samples frame f covers at `fps`: whole samples, no drift.
long long FrameSamples( long long f, double fps, int rate )
{
	auto at = [ & ]( long long g ) {
		return static_cast< long long >( std::floor( static_cast< double >( g ) * rate / fps + 1e-9 ) );
	};
	return at( f + 1 ) - at( f );
}

struct Feeder
{
	Signal               signal;
	int                  rate = kFeedRate;
	long long            n    = 0;///< samples fed so far
	std::vector< float > buf;

	void Push( vfd::Input& in, long long count )
	{
		buf.resize( static_cast< size_t >( count ) * 2 );
		for( long long k = 0; k < count; ++k )
		{
			const float v = static_cast< float >( signal( n + k ) );
			buf[ static_cast< size_t >( 2 * k ) ] = buf[ static_cast< size_t >( 2 * k + 1 ) ] = v;
		}
		in.Feed( buf.data(), static_cast< int >( count ) );
		n += count;
	}
};

/// Drives a plugin that is already on Type = VFD: earshot's feed, then the
/// plugin's own per-frame call. Host time is audio time plus `host0`.
struct Driver
{
	NeedlePlugin& p;
	Feeder        feed;
	double        fps   = 60.0;
	double        host0 = 0.0;
	long long     frame = 0;

	Driver( NeedlePlugin& plugin, Signal s, int rate = kFeedRate, double framesPerSecond = 60.0, double hostStart = 0.0 )
		: p( plugin ), fps( framesPerSecond ), host0( hostStart )
	{
		feed.signal = std::move( s );
		feed.rate   = rate;
		ArmEngine( p.VfdInput() );
		p.VfdInput().FeedBegin( rate, 2 );
	}

	double Host() const { return host0 + static_cast< double >( feed.n ) / feed.rate; }
	double Audio() const { return static_cast< double >( feed.n ) / feed.rate; }
	void   Push( long long samples ) { feed.Push( p.VfdInput(), samples ); }
	/// Audio, then the plugin's per-frame VFD step: everything ProcessOpenGL
	/// does on a VFD frame except the drawing.
	void Step( long long samples )
	{
		Push( samples );
		p.AdvanceVfd( Host() );
	}
	void Frame() { Step( FrameSamples( frame++, fps, feed.rate ) ); }
	void Settle( double seconds )
	{
		const long long n = std::llround( seconds * fps );
		for( long long k = 0; k < n; ++k )
			Frame();
	}
	const earshot::Frame& Ear() { return p.VfdInput().EngineForTest()->frame(); }
};

void SetVfd( NeedlePlugin& p, VfdBands bands )
{
	p.SetFloatParameter( PT_TYPE, static_cast< float >( kTypeVfd ) );
	p.SetFloatParameter( PT_BANDS, static_cast< float >( static_cast< int >( bands ) ) );
}

/// No GL: a plugin, the feed, and the plugin's per-frame call.
struct Bank
{
	NeedlePlugin            plugin;
	std::unique_ptr< Driver > d;

	Bank( int rate, VfdBands bands, Signal s, double fps = 60.0 )
	{
		SetVfd( plugin, bands );
		d = std::make_unique< Driver >( plugin, std::move( s ), rate, fps );
	}
};

/// The band index x of a Frame's third, from its own exact centre.
int ThirdX( const earshot::Third& t )
{
	return static_cast< int >( std::lround( 10.0 * std::log10( static_cast< double >( t.exactHz ) / 1000.0 ) ) );
}

/// Power average over a whole number of periods of the Fast weighting's ripple
/// at 2 hz, at least 0.2 s, read every millisecond of audio. The ripple is
/// the time weighting's, not the filter's (for a 20 Hz sine it is +-0.14 dB),
/// so averaging over whole periods leaves the filter's own response.
/// `read( i )` returns a dB level; `out[ i ]` gets the average.
template< class Read >
void Averaged( Driver& d, double hz, int n, Read read, double* out )
{
	const double    period  = d.feed.rate / ( 2.0 * hz );
	const long long periods = std::max( 1LL, static_cast< long long >( std::ceil( 0.2 * d.feed.rate / period ) ) );
	const long long window  = std::llround( static_cast< double >( periods ) * period );
	const long long block   = std::max( 1, d.feed.rate / 1000 );
	std::vector< double > acc( static_cast< size_t >( n ), 0.0 );
	long long             fed = 0;
	while( fed < window )
	{
		const long long k = std::min( block, window - fed );
		d.Step( k );
		fed += k;
		for( int i = 0; i < n; ++i )
			acc[ static_cast< size_t >( i ) ] += std::pow( 10.0, read( i ) / 10.0 ) * static_cast< double >( k );
	}
	for( int i = 0; i < n; ++i )
		out[ i ] = 10.0 * std::log10( std::max( 1e-300, acc[ static_cast< size_t >( i ) ] / static_cast< double >( fed ) ) );
}

/// Every third earshot reports, averaged, indexed by x + 17 (-400 where none).
void AveragedThirds( Driver& d, double hz, double out[ vfd::kMaxColumns ] )
{
	const earshot::Frame& f = d.Ear();
	int                   map[ vfd::kMaxColumns ];
	for( int c = 0; c < vfd::kMaxColumns; ++c )
		map[ c ] = -1;
	for( int i = 0; i < f.thirdCount; ++i )
	{
		const int c = ThirdX( f.thirds[ i ] ) + 17;
		if( c >= 0 && c < vfd::kMaxColumns )
			map[ c ] = i;
	}
	double avg[ vfd::kMaxColumns ];
	Averaged( d, hz, vfd::kMaxColumns,
			  [ & ]( int c ) { return map[ c ] >= 0 ? static_cast< double >( d.Ear().thirds[ map[ c ] ].db ) : -400.0; }, avg );
	for( int c = 0; c < vfd::kMaxColumns; ++c )
		out[ c ] = map[ c ] >= 0 ? avg[ c ] : -400.0;
}

//---------------------------------------------------------------------------
// IS 6964:2001 (IEC 61260:1995), typed in from the standard.
//---------------------------------------------------------------------------
/// Table A.1, base-ten exact midbands to five figures, x = -16 .. 13.
const double kTableA1Exact[ 30 ] = { 25.119, 31.623, 39.811, 50.119, 63.096, 79.433, 100.00, 125.89, 158.49, 199.53,
									 251.19, 316.23, 398.11, 501.19, 630.96, 794.33, 1000.0, 1258.9, 1584.9, 1995.3,
									 2511.9, 3162.3, 3981.1, 5011.9, 6309.6, 7943.3, 10000, 12589, 15849, 19953 };
/// Table A.1, nominal midbands, x = -16 .. 13. x = -17 (19.953 Hz) is not in
/// the table; A.3 rounds it to three figures: 20.0.
const float kTableA1Nominal[ 30 ] = { 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
									  800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };

/// Table B.1, base ten, class 1 minimum relative attenuation outside the band:
/// (normalised frequency, dB). The class 1 entry at 1.29437 is illegible in
/// the scan; Table 1's octave row at the same breakpoint gives +17.5 for
/// class 1, and B.1 is that row mapped to thirds by eq. (10)/(11).
struct Breakpoint
{
	double omega, db;
};
const Breakpoint kB1Class1Min[] = { { 1.12202, 2.0 }, { 1.29437, 17.5 }, { 1.88173, 42.0 }, { 3.05365, 61.0 }, { 25.39195, 70.0 } };

/// The class 1 minimum attenuation at normalised frequency omega > 1.12202,
/// by eq. (12): linear in lg(omega) between breakpoints.
double ThirdMaskMin( double omega )
{
	const int n = static_cast< int >( sizeof( kB1Class1Min ) / sizeof( kB1Class1Min[ 0 ] ) );
	for( int i = 0; i + 1 < n; ++i )
	{
		const Breakpoint& a = kB1Class1Min[ i ];
		const Breakpoint& b = kB1Class1Min[ i + 1 ];
		if( omega >= a.omega && omega <= b.omega )
			return a.db + ( b.db - a.db ) * std::log10( omega / a.omega ) / std::log10( b.omega / a.omega );
	}
	return kB1Class1Min[ n - 1 ].db;
}

/// The segment law of the spec (section 3), written out here rather than
/// called: segment i (0 = bottom) is lit when level >= Reference - 3 (12 - i).
int SegmentsFor( double levelDb, double referenceDbfs )
{
	int n = 0;
	for( int i = 0; i < vfd::kRows; ++i )
		n += levelDb >= referenceDbfs - 3.0 * ( vfd::kRows - i ) ? 1 : 0;
	return n;
}

//---------------------------------------------------------------------------
// Pictures.
//---------------------------------------------------------------------------
struct VfdRaster
{
	int         w, h;
	float       size;
	const char* name;
};
const VfdRaster kVfdRasters[] = { { 1280, 720, kSizeDefault, "1280x720" }, { 320, 180, 1.0f, "320x180 Size 1" } };

/// The screen pixel holding a local-frame point (the inverse of the shader's
/// rotation about the centre).
void VfdPixel( const vfd::Picture& p, float lx, float ly, int& px, int& py )
{
	const float c  = std::cos( p.rotation ), s = std::sin( p.rotation );
	const float sx = p.centre[ 0 ] + lx * c - ly * s;
	const float sy = p.centre[ 1 ] + lx * s + ly * c;
	px = static_cast< int >( std::floor( sx ) );
	py = static_cast< int >( std::floor( sy ) );
}

/// A segment's probe: a quarter of its height below its centre, because the
/// wires cross segment centres. Always inside the segment by more than half a
/// pixel at both rasters (1.1 px at 320x180 Size 1, 2.8 px at 1280x720).
bool SegmentLit( const Image& img, const vfd::Picture& p, int column, int fromBottom )
{
	float lx = 0, ly = 0;
	p.SegmentCentre( column, fromBottom, lx, ly );
	ly += 0.25f * p.seg[ 1 ];
	int x = 0, y = 0;
	VfdPixel( p, lx, ly, x, y );
	return img.at( x, y )[ 1 ] > 128;
}

int LitCount( const Image& img, const vfd::Picture& p, int column )
{
	int n = 0;
	for( int s = 0; s < p.rows; ++s )
		n += SegmentLit( img, p, column, s ) ? 1 : 0;
	return n;
}

/// The sRGB transfer function, CSS Color 4 section 10.2, written out here.
double Decode( double code )
{
	const double c = code / 255.0;
	return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );
}

/// A GL plugin on Type = VFD, fed, drawn when asked.
struct VfdRig
{
	Target                      t;
	std::unique_ptr< Instance > i;
	std::unique_ptr< Driver >   d;
	int                         w = 0, h = 0;

	VfdRig( const VfdRaster& r, Signal s, VfdBands bands = VfdBands::Thirds, double hostStart = 0.0,
			std::function< void( Instance& ) > configure = nullptr, int rate = kFeedRate )
		: w( r.w ), h( r.h )
	{
		t.Create( r.w, r.h );
		i = std::make_unique< Instance >( r.w, r.h );
		SetVfd( i->plugin, bands );
		i->set( PT_SIZE, r.size );
		if( configure )
			configure( *i );
		d = std::make_unique< Driver >( i->plugin, std::move( s ), rate, 60.0, hostStart );
	}
	~VfdRig()
	{
		d.reset();
		i.reset();
		t.Destroy();
	}

	/// One frame as a host runs it: the frame's audio, SetTime, ProcessOpenGL.
	Image Draw()
	{
		d->Push( FrameSamples( d->frame++, d->fps, d->feed.rate ) );
		glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
		glViewport( 0, 0, t.w, t.h );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		i->plugin.SetTime( d->Host() );
		ProcessOpenGLStruct gl = {};
		gl.HostFBO             = t.fbo;
		if( i->plugin.ProcessOpenGL( &gl ) != FF_SUCCESS )
			std::printf( "  ProcessOpenGL FAILED\n" );
		return readBack( t );
	}
	void Resize( const VfdRaster& r )
	{
		t.Destroy();
		t.Create( r.w, r.h );
		w = r.w;
		h = r.h;
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( r.w ), static_cast< FFUInt32 >( r.h ) };
		i->plugin.Resize( &vp );
		i->set( PT_SIZE, r.size );
	}
	vfd::Picture Pic() const { return i->plugin.BuildVfdPicture( w, h ); }
};

int Done( int before )
{
	std::printf( "\n  %s\n", failures == before ? "PASS" : "FAIL" );
	return failures == before ? 0 : 1;
}

double ReferenceDefault()
{
	return ReferenceDbfs( kReferenceDefault );
}
} // namespace

//---------------------------------------------------------------------------
// --iec (I1): the bands are IEC 61260's, measured through earshot's output.
//---------------------------------------------------------------------------
int runIec()
{
	std::printf( "the VFD's bands against IEC 61260:1995 (IS 6964:2001), class 1\n"
				 "(no GL: earshot's Frame through the plugin's vfd::Input, fed by feed())\n\n" );
	const int before = failures;
	for( int rate : { 48000, 44100 } )
	{
		std::printf( "  -- %d Hz\n", rate );
		// (a) The bands as the Frame describes them.
		{
			Bank b( rate, VfdBands::Thirds, []( long long ) { return 0.0; } );
			b.d->Settle( 0.1 );
			const earshot::Frame& f    = b.d->Ear();
			const int             want = rate >= 48000 ? 31 : 30;
			int    badCentre = 0, badLabel = 0, badEdge = 0, badOrder = 0;
			double worstCentre = 0.0, worstEdge = 0.0;
			for( int i = 0; i < f.thirdCount; ++i )
			{
				const earshot::Third& t = f.thirds[ i ];
				const int             x = ThirdX( t );
				badOrder += x != -17 + i ? 1 : 0;
				// Table A.1 to its five printed figures; x = -17 by eq. (3).
				const double table = x >= -16 && x <= 13 ? kTableA1Exact[ x + 16 ] : BandHz( x );
				const double unit  = std::pow( 10.0, std::floor( std::log10( table ) ) - 4.0 );
				const double off   = std::fabs( t.exactHz - table ) / unit;
				worstCentre        = std::max( worstCentre, off );
				badCentre += off > 0.5 ? 1 : 0;
				const float label = x >= -16 && x <= 13 ? kTableA1Nominal[ x + 16 ] : 20.0f;
				badLabel += t.nominalHz != label ? 1 : 0;
				// eq. (5)/(6): f1,2 = G^(-+1/6) fm, G = 10^0.3, from eq. (3)'s fm.
				const double fm = BandHz( x );
				for( double e : { std::fabs( t.loHz / ( fm * std::pow( 10.0, -0.05 ) ) - 1.0 ),
								  std::fabs( t.hiHz / ( fm * std::pow( 10.0, 0.05 ) ) - 1.0 ) } )
				{
					worstEdge = std::max( worstEdge, e );
					badEdge += e > 1.2e-7 ? 1 : 0;
				}
			}
			Check( f.thirdCount == want && badOrder == 0,
				   std::to_string( f.thirdCount ) + " bands (want " + std::to_string( want ) +
					   "), x = -17 upward in order (at 44.1 kHz the 20 kHz band's top edge, 22.4 kHz, is over 0.95 x Nyquist)" );
			Check( badCentre == 0, "every exact midband within half a unit of the fifth figure of Table A.1 "
								   "(x = -17 by eq. (3)): worst " + F( worstCentre, 3 ) + " units" );
			Check( badLabel == 0, "every label is Table A.1's nominal midband (20 Hz by the A.3 rounding): " +
									  std::to_string( badLabel ) + " wrong" );
			Check( badEdge == 0, "every band edge is eq. (5)/(6)'s G^(-+1/6) fm within float rounding "
								 "(1.2e-7 relative; the Frame carries float): worst " +
									 F( worstEdge * 1e9, 1 ) + "e-9" );
		}

		// (b) Measured through the running filters. 0.5 amplitude: -6.02 dB
		// re a full-scale sine, the unit Third::db is in.
		const double amp = 0.5, inDb = 20.0 * std::log10( amp );
		int    bands = 0, badMid = 0, badEdge = 0;
		double worstMid = 0.0, edgeLo = 1e9, edgeHi = -1e9;
		std::string worstMidAt, edgeAt;
		const int   top = rate >= 48000 ? 13 : 12;
		for( int x = -17; x <= top; ++x )
		{
			Bank b( rate, VfdBands::Thirds, Sine( BandHz( x ), amp, rate ) );
			b.d->Settle( 2.0 );
			double lv[ vfd::kMaxColumns ];
			AveragedThirds( *b.d, BandHz( x ), lv );
			const double err = lv[ x + 17 ] - inDb;
			++bands;
			if( std::fabs( err ) > worstMid )
			{
				worstMid   = std::fabs( err );
				worstMidAt = F( BandHz( x ), 1 ) + " Hz";
			}
			badMid += std::fabs( err ) > 0.3 ? 1 : 0;
		}
		Check( badMid == 0, std::to_string( bands ) + " sines at their exact midbands read 0 +- 0.3 dB in their own band "
						   "(class 1 at Omega = 1, Table B.1), averaged over whole Fast ripple periods: worst " +
							   F( worstMid, 3 ) + " dB at " + worstMidAt );
		int edges = 0;
		for( int x = -17; x <= top + 1; ++x )
		{
			// The edge between bands x - 1 and x: f2 of one is f1 of the other.
			const double hz = BandHz( x ) * std::pow( 10.0, -0.05 );
			Bank         b( rate, VfdBands::Thirds, Sine( hz, amp, rate ) );
			b.d->Settle( 2.0 );
			double lv[ vfd::kMaxColumns ];
			AveragedThirds( *b.d, hz, lv );
			for( int k : { x - 1, x } )
			{
				if( k < -17 || k > top )
					continue;
				const double down = inDb - lv[ k + 17 ];
				++edges;
				if( down < edgeLo )
				{
					edgeLo = down;
					edgeAt = F( hz, 1 ) + " Hz in the " + F( BandHz( k ), 0 ) + " Hz band";
				}
				edgeHi = std::max( edgeHi, down );
				badEdge += ( down < 2.0 || down > 5.0 ) ? 1 : 0;
			}
		}
		Check( badEdge == 0, std::to_string( edges ) + " band-edge readings (a sine at each shared edge, read in both "
							"bands) are 2.0-5.0 dB down (class 1 at the edge, Table B.1): " +
								F( edgeLo, 3 ) + " .. " + F( edgeHi, 3 ) + " dB (lowest: " + edgeAt + ")" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --skirt (I2): a sine lights its own band, and its neighbours only by the
// filter's skirt -- from earshot's output and from the picture.
//---------------------------------------------------------------------------
int runSkirt()
{
	std::printf( "a sine lights its own column, and its neighbours only by the skirt\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	const double ownDb  = ref - 1.5;   // own band 1.5 dB under Reference: 12 segments
	const double amp    = AmpFromDb( ownDb );

	// The class 1 minimum at a neighbour's centre: Omega = 10^(0.1 n), by
	// eq. (12) between Table B.1's breakpoints.
	double bound[ 4 ] = { 0, 0, 0, 0 };
	for( int n = 1; n <= 3; ++n )
		bound[ n ] = ThirdMaskMin( std::pow( 10.0, 0.1 * n ) );
	std::printf( "  class 1 minimum at the neighbours (Table B.1, eq. (12)): k+-1 %.3f dB, k+-2 %.3f dB, k+-3 %.3f dB\n",
				 bound[ 1 ], bound[ 2 ], bound[ 3 ] );
	int maxLit[ 4 ] = { vfd::kRows, 0, 0, 0 };
	for( int n = 1; n <= 3; ++n )
		maxLit[ n ] = SegmentsFor( ownDb - bound[ n ], ref );
	std::printf( "  so at most %d, %d and %d segments in columns k+-1, k+-2, k+-3 (3 dB a segment, spec section 3)\n\n",
				 maxLit[ 1 ], maxLit[ 2 ], maxLit[ 3 ] );

	// (a) earshot's output, 48 kHz, every band.
	{
		double      worst[ 4 ] = { 1e9, 1e9, 1e9, 1e9 };
		std::string where[ 4 ];
		int         bad = 0;
		double      ownWorst = 0.0;
		for( int x = -17; x <= 13; ++x )
		{
			Bank b( kFeedRate, VfdBands::Thirds, Sine( BandHz( x ), amp, kFeedRate ) );
			b.d->Settle( 2.0 );
			double lv[ vfd::kMaxColumns ];
			AveragedThirds( *b.d, BandHz( x ), lv );
			const int k = x + 17;
			ownWorst    = std::max( ownWorst, std::fabs( lv[ k ] - ownDb ) );
			for( int c = 0; c < vfd::kMaxColumns; ++c )
			{
				const int n = std::abs( c - k );
				if( n < 1 || n > 3 )
					continue;
				const double down = lv[ k ] - lv[ c ];
				if( down - bound[ n ] < worst[ n ] )
				{
					worst[ n ] = down - bound[ n ];
					where[ n ] = F( BandHz( x ), 1 ) + " Hz sine in the " + F( BandHz( c - 17 ), 1 ) + " Hz band, " +
								 F( down, 2 ) + " dB down";
				}
				bad += down < bound[ n ] ? 1 : 0;
			}
		}
		Check( bad == 0, "earshot, 31 sines at 48 kHz: every neighbour k+-1..3 at least the class 1 minimum down. "
						 "Smallest margins: k+-1 " + F( worst[ 1 ], 2 ) + " dB (" + where[ 1 ] + "); k+-2 " +
							 F( worst[ 2 ], 2 ) + " dB (" + where[ 2 ] + "); k+-3 " + F( worst[ 3 ], 2 ) + " dB (" +
							 where[ 3 ] + ")" );
		std::printf( "       (own band within %.3f dB of %.1f dBFS)\n", ownWorst, ownDb );
	}

	// (b) The picture, both rasters.
	Host host;
	if( !host.Open() )
		return 1;
	for( const VfdRaster& r : kVfdRasters )
	{
		int         bad = 0, law = 0, ownOk = 0;
		std::string first;
		for( int x = -17; x <= 13; ++x )
		{
			VfdRig rig( r, Sine( BandHz( x ), amp, kFeedRate ), VfdBands::Thirds, 0.0,
						[]( Instance& i ) { i.set( PT_PEAK_HOLD, 0.0f ); } );
			rig.d->Settle( 2.2 );
			const Image        img = rig.Draw();
			const vfd::Picture p   = rig.Pic();
			const earshot::Frame& f = rig.d->Ear();
			double level[ vfd::kMaxColumns ];
			for( int c = 0; c < vfd::kMaxColumns; ++c )
				level[ c ] = -400.0;
			for( int i = 0; i < f.thirdCount; ++i )
			{
				const int c = ThirdX( f.thirds[ i ] ) + 17;
				if( c >= 0 && c < vfd::kMaxColumns )
					level[ c ] = f.thirds[ i ].db;
			}
			const int k = x + 17;
			for( int c = 0; c < vfd::kMaxColumns; ++c )
			{
				const int lit = LitCount( img, p, c );
				const int n   = std::abs( c - k );
				const bool ok = n == 0 ? lit == vfd::kRows : ( n <= 3 ? lit <= maxLit[ n ] : lit == 0 );
				if( n == 0 && lit == vfd::kRows )
					++ownOk;
				// Column c is band x = c - 17 (spec section 1): its count is
				// what that band's level implies.
				const int want = SegmentsFor( level[ c ], ref );
				if( !ok || lit != want )
				{
					if( first.empty() )
						first = F( BandHz( x ), 1 ) + " Hz sine: column " + std::to_string( c ) + " has " +
								std::to_string( lit ) + " lit, its band's level " + F( level[ c ], 2 ) + " dBFS implies " +
								std::to_string( want );
					bad += ok ? 0 : 1;
					law += lit != want ? 1 : 0;
				}
			}
		}
		Check( bad == 0 && law == 0,
			   std::string( r.name ) + ": 31 sines, every segment probed: own column all 12 lit (" + std::to_string( ownOk ) +
				   "/31), k+-1 <= " + std::to_string( maxLit[ 1 ] ) + ", k+-2 <= " + std::to_string( maxLit[ 2 ] ) +
				   ", k+-3 and beyond dark, and every column's count is what its band's measured level implies (" +
				   std::to_string( bad ) + " over the mask, " + std::to_string( law ) + " off the law" +
				   ( first.empty() ? std::string( ")" ) : "; first: " + first + ")" ) );
	}

	// (c) 44.1 kHz has no 20 kHz band: its column stays dark whatever is fed.
	{
		const VfdRaster& r = kVfdRasters[ 1 ];
		VfdRig           rig( r, Programme( ref + 6.0, 0.0, 44100 ), VfdBands::Thirds, 0.0, nullptr, 44100 );
		rig.d->Settle( 1.0 );
		const Image        img = rig.Draw();
		const vfd::Picture p   = rig.Pic();
		const int          lit30 = LitCount( img, p, 30 ), lit29 = LitCount( img, p, 29 );
		Check( lit30 == 0 && lit29 == vfd::kRows && !rig.i->plugin.VfdDisplay().At( 30 ).present,
			   std::string( r.name ) + ", 44.1 kHz, every band 6 dB over Reference: the 20 kHz column dark (" +
				   std::to_string( lit30 ) + " lit, no band), the 16 kHz column full (" + std::to_string( lit29 ) + ")" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --octaves (O1): octaves summed from thirds, against the class 1 OCTAVE mask.
//---------------------------------------------------------------------------
int runOctaves()
{
	std::printf( "10 Octaves: power sums of three thirds, against IS 6964 Table 1, class 1\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	auto octaveRead = []( Driver& d, double hz, double out[ 10 ] ) {
		Averaged( d, hz, 10,
				  [ & ]( int j ) {
					  const vfd::Column& c = d.p.VfdDisplay().At( j );
					  return c.present ? c.levelDb : -400.0;
				  },
				  out );
	};
	// (a) The display's octave levels.
	{
		const double amp = 0.5, inDb = 20.0 * std::log10( amp );
		double       worstMid = 0.0, worstNb = 1e9, edgeLo = 1e9, edgeHi = -1e9;
		int          bad = 0, edges = 0;
		for( int j = 0; j < 10; ++j )
		{
			const double hz = BandHz( vfd::ColumnX( VfdBands::Octaves, j ) );
			Bank         b( kFeedRate, VfdBands::Octaves, Sine( hz, amp, kFeedRate ) );
			b.d->Settle( 2.0 );
			double lv[ 10 ];
			octaveRead( *b.d, hz, lv );
			worstMid = std::max( worstMid, std::fabs( lv[ j ] - inDb ) );
			bad += std::fabs( lv[ j ] - inDb ) > 0.3 ? 1 : 0;
			for( int n : { j - 1, j + 1 } )
				if( n >= 0 && n < 10 )
				{
					worstNb = std::min( worstNb, lv[ j ] - lv[ n ] );
					bad += lv[ j ] - lv[ n ] < 17.5 ? 1 : 0;
				}
		}
		for( int j = 0; j <= 10; ++j )
		{
			// The edge between octaves j - 1 and j: G^(-1/2) of octave j's centre.
			const double hz = BandHz( -15 + 3 * j ) * std::pow( 10.0, -0.15 );
			Bank         b( kFeedRate, VfdBands::Octaves, Sine( hz, amp, kFeedRate ) );
			b.d->Settle( 2.0 );
			double lv[ 10 ];
			octaveRead( *b.d, hz, lv );
			for( int k : { j - 1, j } )
				if( k >= 0 && k < 10 )
				{
					const double down = inDb - lv[ k ];
					++edges;
					edgeLo = std::min( edgeLo, down );
					edgeHi = std::max( edgeHi, down );
					bad += ( down < 2.0 || down > 5.0 ) ? 1 : 0;
				}
		}
		Check( bad == 0, "the display's octaves, 48 kHz: own octave 0 +- 0.3 dB (worst " + F( worstMid, 3 ) +
							 "), neighbours >= 17.5 dB down (least " + F( worstNb, 2 ) + "), " + std::to_string( edges ) +
							 " edge readings 2.0-5.0 dB down (" + F( edgeLo, 3 ) + " .. " + F( edgeHi, 3 ) +
							 ") -- Table 1 at Omega = 1, G and G^(+-1/2)" );
	}
	// (b) The picture.
	Host host;
	if( !host.Open() )
		return 1;
	const double ownDb = ref - 1.5;
	const int    nbMax = SegmentsFor( ownDb - 17.5, ref );
	for( const VfdRaster& r : kVfdRasters )
	{
		int         bad = 0;
		std::string first;
		for( int j = 0; j < 10; ++j )
		{
			const double hz = BandHz( vfd::ColumnX( VfdBands::Octaves, j ) );
			VfdRig       rig( r, Sine( hz, AmpFromDb( ownDb ), kFeedRate ), VfdBands::Octaves, 0.0,
							  []( Instance& i ) { i.set( PT_PEAK_HOLD, 0.0f ); } );
			rig.d->Settle( 2.2 );
			const Image        img = rig.Draw();
			const vfd::Picture p   = rig.Pic();
			for( int c = 0; c < 10; ++c )
			{
				const int  lit  = LitCount( img, p, c );
				const int  want = SegmentsFor( rig.i->plugin.VfdDisplay().At( c ).levelDb, ref );
				const int  n    = std::abs( c - j );
				const bool ok   = ( n == 0 ? lit == vfd::kRows : ( n == 1 ? lit <= nbMax : true ) ) && lit == want;
				if( !ok )
				{
					++bad;
					if( first.empty() )
						first = "; first: " + F( hz, 1 ) + " Hz, column " + std::to_string( c ) + " " + std::to_string( lit ) +
								" lit, its level implies " + std::to_string( want );
				}
			}
		}
		Check( bad == 0, std::string( r.name ) + ": 10 sines, own octave all 12 lit, neighbours <= " +
							 std::to_string( nbMax ) + ", every column's count what its octave level implies (" +
							 std::to_string( bad ) + " wrong" + first + ")" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --hold (H1, H2): the peak hold and its fall, in audio time.
//---------------------------------------------------------------------------
namespace
{
struct HoldTrace
{
	std::vector< double > t, hold, level, ehold, elevel;
	std::vector< int >    lit, dot;
};

/// A 1 kHz tone from kToneStart for `toneSeconds`, 1.5 dB under Reference.
///
/// The quarter second of silence first is not decoration. earshot primes each
/// third's Fast envelope with the first update's block, so with the tone from
/// sample 0 a 24 fps run (2000 samples in its first block) and a 144 fps run
/// (333) start from different envelopes, and read 0.90 dB apart 167 ms in,
/// 0.05 dB at 500 ms (measured; AGENTS.md, traps). Zeros in every rate's
/// first block prime every rate alike.
constexpr double kToneStart = 0.25;

HoldTrace TraceHold( double fps, double toneSeconds, double total )
{
	const double ref = ReferenceDefault();
	Bank         b( kFeedRate, VfdBands::Thirds,
					Sine( 1000.0, AmpFromDb( ref - 1.5 ), kFeedRate, kToneStart + toneSeconds, kToneStart ), fps );
	HoldTrace    tr;
	const long long frames = std::llround( total * fps );
	for( long long f = 0; f < frames; ++f )
	{
		b.d->Frame();
		const vfd::Column& c = b.plugin.VfdDisplay().At( 17 );
		tr.t.push_back( b.d->Audio() );
		tr.hold.push_back( c.holdDb );
		tr.level.push_back( c.levelDb );
		tr.lit.push_back( c.lit );
		tr.dot.push_back( c.dot );
		const earshot::Frame& e = b.d->Ear();
		double eh = -400.0, el = -400.0;
		for( int i = 0; i < e.thirdCount; ++i )
			if( ThirdX( e.thirds[ i ] ) == 0 )
			{
				eh = e.thirds[ i ].holdDb;
				el = e.thirds[ i ].db;
			}
		tr.ehold.push_back( eh );
		tr.elevel.push_back( el );
	}
	return tr;
}

/// When the bar leaves the dot (the first frame after `after` whose bar no
/// longer reaches the held segment), when the hold first drops below its value
/// then, and the most it ever dropped from one frame to the next before that.
void LeaveAndFall( const HoldTrace& tr, double after, double& leave, double& fall, double& sag )
{
	const double ref = ReferenceDefault();
	leave = fall = -1.0;
	sag          = 0.0;
	size_t at    = 0;
	for( size_t i = 1; i < tr.t.size(); ++i )
	{
		if( tr.t[ i ] <= after )
			continue;
		if( tr.lit[ i ] < vfd::LitFor( tr.hold[ i ], ref ) )
		{
			leave = tr.t[ i ];
			at    = i;
			break;
		}
		sag = std::max( sag, tr.hold[ i - 1 ] - tr.hold[ i ] );
	}
	for( size_t i = at + 1; at > 0 && i < tr.t.size(); ++i )
		if( tr.hold[ i ] < tr.hold[ at ] - 1e-12 )
		{
			fall = tr.t[ i ];
			break;
		}
}
} // namespace

int runHold()
{
	std::printf( "the peak hold: frame-rate independent, and it waits\n(no GL: the plugin's Display, fed by feed())\n\n" );
	const int before = failures;
	NeedlePlugin defaults;
	const vfd::DisplaySettings s = defaults.VfdSettings();
	std::printf( "  Peak Hold %.2f s, Hold Decay %.1f dB/s (the defaults); a 1 kHz tone 1.5 dB under Reference\n\n",
				 s.holdSeconds, s.fallDbPerSecond );
	// The 125 ms weighting's ripple at 2 kHz on a 1 kHz tone: the only thing
	// besides the frame that can move the captured peak.
	const double r      = 1.0 / std::sqrt( 1.0 + std::pow( 4.0 * kPi * 1000.0 * 0.125, 2.0 ) );
	const double ripple = 10.0 * std::log10( 1.0 + r );

	// H1: a 1 s tone, then silence, at four frame rates.
	const double rates[] = { 144.0, 60.0, 30.0, 24.0 };
	HoldTrace    traces[ 4 ];
	for( int k = 0; k < 4; ++k )
		traces[ k ] = TraceHold( rates[ k ], 1.0, 5.0 );
	const double toneEnd = kToneStart + 1.0;
	for( int k = 1; k < 4; ++k )
	{
		// Times every rate lands on exactly: multiples of 1/6 s (8000 samples).
		double worst = 0.0;
		int    compared = 0;
		for( size_t i = 0; i < traces[ k ].t.size(); ++i )
			for( size_t j = 0; j < traces[ 0 ].t.size(); ++j )
				if( traces[ 0 ].t[ j ] == traces[ k ].t[ i ] &&
					std::fabs( traces[ k ].t[ i ] * 6.0 - std::round( traces[ k ].t[ i ] * 6.0 ) ) < 1e-9 )
				{
					worst = std::max( worst, std::fabs( traces[ k ].hold[ i ] - traces[ 0 ].hold[ j ] ) );
					++compared;
				}
		const double tol = s.fallDbPerSecond / rates[ k ] + ripple;
		Check( compared >= 25 && worst <= tol,
			   F( rates[ k ], 0 ) + " fps against 144 fps at " + std::to_string( compared ) +
				   " common audio times (k/6 s): held level within " + F( worst, 4 ) + " dB (limit Hold Decay x one " +
				   F( rates[ k ], 0 ) + " fps frame + the 2 kHz ripple = " + F( tol, 4 ) + ")" );
	}
	for( int k = 0; k < 4; ++k )
	{
		double leave, fall, sag;
		LeaveAndFall( traces[ k ], toneEnd - 0.5, leave, fall, sag );
		const double held = fall - leave;
		// The fall's slope, least squares over 0.1 .. 1.0 s into it.
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		int    n  = 0;
		for( size_t i = 0; i < traces[ k ].t.size(); ++i )
			if( traces[ k ].t[ i ] >= fall + 0.1 && traces[ k ].t[ i ] <= fall + 1.0 )
			{
				const double x = traces[ k ].t[ i ], y = traces[ k ].hold[ i ];
				sx += x;
				sy += y;
				sxx += x * x;
				sxy += x * y;
				++n;
			}
		const double slope = n > 2 ? ( n * sxy - sx * sy ) / ( n * sxx - sx * sx ) : 0.0;
		Check( leave > 0.0 && fall > 0.0 && std::fabs( held - s.holdSeconds ) <= 1.0 / rates[ k ] + 1e-9 &&
				   std::fabs( -slope - s.fallDbPerSecond ) <= 1e-6 * s.fallDbPerSecond,
			   F( rates[ k ], 0 ) + " fps: the dot held " + F( held, 4 ) + " s after the bar left it (Peak Hold +- one frame, " +
				   F( 1.0 / rates[ k ], 4 ) + " s), then fell at " + F( -slope, 6 ) +
				   " dB/s (Hold Decay within 1e-6: the fall is Hold Decay x dt in double)" );
	}

	// H2: an 8 s note, far longer than the hold. The dot must not move for
	// the whole note, and must wait Peak Hold after the bar leaves it.
	//
	// earshot's own Third::holdDb restarts its wait only when the level is
	// >= the hold value, so its wait starts at the level's last maximum. At
	// the end of a note that comes BEFORE the bar drops below the dot's 3 dB
	// segment, so measured from what the eye sees the dot falls early
	// (AGENTS.md, traps). That rule is planted as holdAtPeakOnly, and the
	// note below measures earshot's own hold in the same run.
	const double noteEnd = kToneStart + 8.0;
	for( int k = 0; k < 4; ++k )
	{
		const HoldTrace tr = TraceHold( rates[ k ], 8.0, 11.0 );
		double leave, fall, sag;
		LeaveAndFall( tr, kToneStart + 0.5, leave, fall, sag );
		Check( leave >= noteEnd && sag == 0.0 && fall - leave >= s.holdSeconds - 1.0 / rates[ k ] - 1e-9,
			   F( rates[ k ], 0 ) + " fps, an 8 s note: the hold never dropped from one frame to the next before the bar "
				   "left it at " + F( leave - kToneStart, 3 ) + " s (largest drop " + F( sag, 6 ) + " dB), then waited " +
				   F( fall - leave, 3 ) + " s (Peak Hold less one frame at least)" );
		// The trap, on earshot's own hold (its defaults: 1.0 s, 20 dB/s), in
		// the same run: when it began to fall, counted from the frame the bar
		// left the dot.
		double eFall = -1.0;
		size_t at    = 0;
		for( size_t i = 0; i < tr.t.size(); ++i )
			if( tr.t[ i ] == leave )
				at = i;
		for( size_t i = at + 1; at > 0 && i < tr.t.size(); ++i )
			if( tr.ehold[ i ] < tr.ehold[ at ] - 1e-6 )
			{
				eFall = tr.t[ i ];
				break;
			}
		std::printf( "       earshot trap (a note, not a check): its own Third::holdDb, hold 1.000 s, began to fall %.3f s "
					 "after the bar left the dot\n",
					 eFall - leave );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --multiplex (M1, M2): the scan against a camera.
//---------------------------------------------------------------------------
int runMultiplex()
{
	std::printf( "the multiplexed scan against a rolling-shutter camera\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	Host         host;
	if( !host.Open() )
		return 1;
	for( const VfdRaster& r : kVfdRasters )
	{
		// Every band 3 dB over Reference: every segment of every column lit.
		VfdRig rig( r, Programme( ref + 3.0, 0.0, kFeedRate ) );
		rig.d->Settle( 1.5 );
		const Image        eye = rig.Draw();
		const vfd::Picture p   = rig.Pic();
		int                all = 0;
		for( int c = 0; c < p.cols; ++c )
			all += LitCount( eye, p, c );

		// M1: an exposure of exactly two scans (1/50 s at 100 Hz) sees every
		// grid's pulse twice: the time average, so the Eye picture.
		const double f = MultiplexHz( kMultiplexDefault );
		rig.i->set( PT_SHUTTER, ShutterParam( 2.0 / f ) );
		const Image two = rig.Draw();
		int         worst = 0;
		long        differ = 0;
		for( size_t k = 0; k < eye.px.size(); ++k )
		{
			const int d = std::abs( static_cast< int >( eye.px[ k ] ) - static_cast< int >( two.px[ k ] ) );
			worst       = std::max( worst, d );
			differ += d != 0 ? 1 : 0;
		}
		Check( all == p.cols * p.rows && worst <= 1,
			   std::string( r.name ) + " M1: a 2-scan exposure (E f = " + F( ShutterSeconds( ShutterParam( 2.0 / f ) ) * f, 6 ) +
				   ") gives the Eye picture within " + std::to_string( worst ) + " code value(s) (limit 1, the float to "
				   "8-bit step; " + std::to_string( differ ) + " subpixels differ; " + std::to_string( all ) + "/" +
				   std::to_string( p.cols * p.rows ) + " segments lit)" );

		// M2: 1/8000 s, shorter than a slot (322.6 us at 100 Hz, 31 grids).
		rig.i->set( PT_SHUTTER, 1.0f );
		const Image        cam = rig.Draw();
		const vfd::Picture q   = rig.Pic();
		const double       E   = ShutterSeconds( 1.0f );
		const double       S   = 1.0 / ( q.cols * f );
		const int          maxCols = static_cast< int >( std::ceil( E / S ) ) + 1;
		std::vector< double > rows, phase;
		int                   tooMany = 0;
		double                prev    = -1.0, unwrap = 0.0;
		for( int y = 0; y < r.h; ++y )
		{
			double wsum = 0.0, csum = 0.0;
			int    lit  = 0, best = -1;
			double bestW = 0.0, weight[ vfd::kMaxColumns ] = {};
			for( int c = 0; c < q.cols; ++c )
			{
				float lx = 0, ly = 0;
				q.SegmentCentre( c, 0, lx, ly );
				const int x0 = static_cast< int >( std::ceil( q.centre[ 0 ] + lx - 0.5f * q.seg[ 0 ] + 0.5f ) );
				const int x1 = static_cast< int >( std::floor( q.centre[ 0 ] + lx + 0.5f * q.seg[ 0 ] - 1.5f ) );
				double    g  = 0.0;
				for( int x = x0; x <= x1; ++x )
					g = std::max( g, Decode( cam.at( x, y )[ 1 ] ) );
				weight[ c ] = g;
				if( g > 0.02 )
				{
					++lit;
					if( g > bestW )
					{
						bestW = g;
						best  = c;
					}
				}
			}
			if( lit == 0 )
				continue;
			tooMany += lit > maxCols ? 1 : 0;
			// Centroid on the circle of columns, around the brightest.
			for( int c = 0; c < q.cols; ++c )
				if( weight[ c ] > 0.02 )
				{
					int cc = c;
					if( cc - best > q.cols / 2 )
						cc -= q.cols;
					if( best - cc > q.cols / 2 )
						cc += q.cols;
					wsum += weight[ c ];
					csum += weight[ c ] * cc;
				}
			double ph = ( csum / wsum + 0.5 ) / q.cols;
			if( prev >= 0.0 )
			{
				if( ph + unwrap - prev < -0.5 )
					unwrap += 1.0;
				else if( ph + unwrap - prev > 0.5 )
					unwrap -= 1.0;
			}
			ph += unwrap;
			prev = ph;
			rows.push_back( y );
			phase.push_back( ph );
		}
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		const double n = static_cast< double >( rows.size() );
		for( size_t k = 0; k < rows.size(); ++k )
		{
			sx += rows[ k ];
			sy += phase[ k ];
			sxx += rows[ k ] * rows[ k ];
			sxy += rows[ k ] * phase[ k ];
		}
		const double slope = n > 2 ? ( n * sxy - sx * sy ) / ( n * sxx - sx * sx ) : 0.0;
		const double want  = vfd::kReadoutSeconds * f / r.h;
		const double span  = rows.empty() ? 1.0 : rows.back() - rows.front();
		// Each row's centroid can sit up to half a column off the scan's
		// true position; a least-squares slope over rows spanning L moves by
		// at most 3 e / L for errors bounded by e.
		const double tol = 3.0 * ( 0.5 / q.cols ) / span;
		Check( tooMany == 0 && rows.size() > 20 && std::fabs( slope - want ) <= tol,
			   std::string( r.name ) + " M2: 1/8000 s: at most " + std::to_string( maxCols ) + " columns lit in any row (" +
				   std::to_string( tooMany ) + " rows over), and the lit column advances " + F( slope * q.cols, 5 ) +
				   " columns a row down the picture over " + std::to_string( rows.size() ) + " rows (Tr N f / H = " +
				   F( want * q.cols, 5 ) + ", limit +-" + F( tol * q.cols, 5 ) + ")" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --clock (M3): a host clock of 499 000 s draws what 0 s draws.
//---------------------------------------------------------------------------
int runClock()
{
	std::printf( "the scan phase survives Resolume's clock: 499 000 s (5.8 days) against 0 s\n"
				 "(Multiplex Rate 500 Hz, where both are whole numbers of scans; Shutter 1/8000 s)\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	Host         host;
	if( !host.Open() )
		return 1;
	auto camera = []( Instance& i ) {
		i.set( PT_MULTIPLEX, 1.0f );
		i.set( PT_SHUTTER, 1.0f );
	};
	for( const VfdRaster& r : kVfdRasters )
	{
		VfdRig a( r, Programme( ref, 6.0, kFeedRate ), VfdBands::Thirds, 0.0, camera );
		VfdRig b( r, Programme( ref, 6.0, kFeedRate ), VfdBands::Thirds, 499000.0, camera );
		a.d->Settle( 1.0 );
		b.d->Settle( 1.0 );
		int  worst = 0;
		long differ = 0, moved = 0;
		Image last;
		for( int k = 0; k < 6; ++k )
		{
			const Image ia = a.Draw(), ib = b.Draw();
			for( size_t j = 0; j < ia.px.size(); ++j )
			{
				const int d = std::abs( static_cast< int >( ia.px[ j ] ) - static_cast< int >( ib.px[ j ] ) );
				worst       = std::max( worst, d );
				differ += d != 0 ? 1 : 0;
				if( k > 0 )
					moved += ia.px[ j ] != last.px[ j ] ? 1 : 0;
			}
			last = ia;
		}
		// The host time 499000 + n / 48000 rounds in double by at most
		// 2^-34 s (5.8e-11 s), 2.9e-8 scans at 500 Hz, under the float
		// phase's own step of 6e-8: at most one float step in phase0, which
		// moves a camera row's g by 4.3e-5, a hundredth of a code value.
		Check( worst <= 1 && moved > 0,
			   std::string( r.name ) + ": six frames, the 499 000 s run within " + std::to_string( worst ) +
				   " code value(s) of the 0 s run (limit 1; " + std::to_string( differ ) +
				   " subpixels differ), while consecutive frames differ by " + std::to_string( moved ) +
				   " subpixels (the bands move)" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --filament (F1): the wires darken by exactly their width.
//---------------------------------------------------------------------------
int runFilament()
{
	std::printf( "the filament wires: the darkening summed down a wire is its width in pixels\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	Host         host;
	if( !host.Open() )
		return 1;
	for( const VfdRaster& r : kVfdRasters )
	{
		VfdRig rig( r, Programme( ref + 3.0, 0.0, kFeedRate ) );
		rig.d->Settle( 1.5 );
		const Image        img  = rig.Draw();
		const vfd::Picture p    = rig.Pic();
		const double       want = vfd::kWireMm * p.pxPerMm;
		// Each row's code can be half a step off; near full scale one step is
		// Decode'(1) / 255 = 0.0089 of linear.
		double worst = 0.0, lo = 1e9, hi = -1e9, tolMax = 0.0;
		int    bad = 0, measured = 0, outside = 0;
		for( int c = 0; c < p.cols; ++c )
			for( int w = 0; w < vfd::kWires; ++w )
			{
				float lx = 0, ly = 0;
				// The segment the wire crosses: rows 1, 4, 7, 10 from the top.
				const int fromBottom = vfd::kRows - 1 - ( 1 + 3 * w );
				p.SegmentCentre( c, fromBottom, lx, ly );
				int x = 0, yc = 0;
				VfdPixel( p, lx, p.wireY[ w ], x, yc );
				// Every row whose centre is within 1.5 px of the wire's: the
				// wire (under 0.3 px) can touch at most two of them.
				const double wy     = p.centre[ 1 ] + p.wireY[ w ];
				const double segTop = p.centre[ 1 ] + ly - 0.5 * p.seg[ 1 ];
				const double segBot = p.centre[ 1 ] + ly + 0.5 * p.seg[ 1 ];
				double       sum = 0.0, tol = 0.0;
				for( int y = static_cast< int >( std::floor( wy - 2.0 ) ); y <= static_cast< int >( std::ceil( wy + 2.0 ) ); ++y )
				{
					if( std::fabs( y + 0.5 - wy ) > 1.5 )
						continue;
					// Inside the segment by half a pixel: the phosphor alone
					// is full green there.
					if( y + 0.5 < segTop + 0.5 || y + 0.5 > segBot - 0.5 )
						++outside;
					const int g = img.at( x, y )[ 1 ];
					sum += 1.0 - Decode( g );
					// The true value lies within half a code of g (255 is the
					// clamp, so only below it).
					tol += std::max( Decode( g ) - Decode( std::max( 0.0, g - 0.5 ) ),
									 Decode( std::min( 255.0, g + 0.5 ) ) - Decode( g ) );
				}
				++measured;
				worst  = std::max( worst, std::fabs( sum - want ) - tol );
				tolMax = std::max( tolMax, tol );
				lo     = std::min( lo, sum );
				hi     = std::max( hi, sum );
				bad += std::fabs( sum - want ) > tol ? 1 : 0;
			}
		Check( bad == 0 && outside == 0,
			   std::string( r.name ) + ": " + std::to_string( measured ) + " crossings (4 wires x 31 lit columns): summed "
				   "darkening " + F( lo, 4 ) + " .. " + F( hi, 4 ) + " px against the wire's " + F( want, 4 ) +
				   " px (37.5 um x " + F( p.pxPerMm, 3 ) + " px/mm; limit the rows' 8-bit rounding, at most " +
				   F( tolMax, 4 ) + ")" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --phosphor (P1): ZnO:Zn's chromaticity, gamut-mapped toward D65.
//---------------------------------------------------------------------------
int runPhosphor()
{
	std::printf( "the phosphor: on the line from D65 to Noritake's green (0.24, 0.41), at the sRGB boundary\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	// CSS Color 4, sample code lin_sRGB_to_XYZ (D65), typed in from the spec.
	const double M[ 3 ][ 3 ] = { { 506752.0 / 1228815.0, 87881.0 / 245763.0, 12673.0 / 70218.0 },
								 { 87098.0 / 409605.0, 175762.0 / 245763.0, 12673.0 / 175545.0 },
								 { 7918.0 / 409605.0, 87881.0 / 737289.0, 1001167.0 / 1053270.0 } };
	auto xy = [ & ]( double r, double g, double b, double& x, double& y ) {
		const double X = M[ 0 ][ 0 ] * r + M[ 0 ][ 1 ] * g + M[ 0 ][ 2 ] * b;
		const double Y = M[ 1 ][ 0 ] * r + M[ 1 ][ 1 ] * g + M[ 1 ][ 2 ] * b;
		const double Z = M[ 2 ][ 0 ] * r + M[ 2 ][ 1 ] * g + M[ 2 ][ 2 ] * b;
		x = X / ( X + Y + Z );
		y = Y / ( X + Y + Z );
	};
	double wx, wy;
	xy( 1, 1, 1, wx, wy );
	const double tx = vfd::kPhosphorX, ty = vfd::kPhosphorY;
	const double dx = tx - wx, dy = ty - wy, len = std::hypot( dx, dy );
	auto off = [ & ]( double x, double y ) { return std::fabs( dx * ( y - wy ) - dy * ( x - wx ) ) / len; };
	auto along = [ & ]( double x, double y ) { return ( ( x - wx ) * dx + ( y - wy ) * dy ) / ( len * len ); };

	Host host;
	if( !host.Open() )
		return 1;
	for( const VfdRaster& r : kVfdRasters )
	{
		VfdRig rig( r, Programme( ref + 3.0, 0.0, kFeedRate ) );
		rig.d->Settle( 1.5 );
		const Image        img = rig.Draw();
		const vfd::Picture p   = rig.Pic();
		int                bad = 0;
		double             worstOff = 0.0, spreadMin = 1e9, s = 0.0, px = 0, py = 0;
		std::string        triple;
		for( int c = 0; c < p.cols; ++c )
		{
			float lx = 0, ly = 0;
			p.SegmentCentre( c, 0, lx, ly );// the bottom segment: no wire crosses it
			int x = 0, y = 0;
			VfdPixel( p, lx, ly, x, y );
			const unsigned char* q = img.at( x, y );
			triple = "(" + std::to_string( q[ 0 ] ) + ", " + std::to_string( q[ 1 ] ) + ", " + std::to_string( q[ 2 ] ) + ")";
			xy( Decode( q[ 0 ] ), Decode( q[ 1 ] ), Decode( q[ 2 ] ), px, py );
			// How far half a code value in any channel can move the point.
			double spread = 0.0;
			for( int m = 0; m < 8; ++m )
			{
				double cx, cy;
				auto   nudge = [ & ]( int ch ) {
                    const double v = q[ ch ] + ( ( m >> ch ) & 1 ? 0.5 : -0.5 );
                    return Decode( std::min( 255.0, std::max( 0.0, v ) ) );
				};
				xy( nudge( 0 ), nudge( 1 ), nudge( 2 ), cx, cy );
				spread = std::max( spread, std::hypot( cx - px, cy - py ) );
			}
			const double o = off( px, py );
			s              = along( px, py );
			worstOff       = std::max( worstOff, o - spread );
			spreadMin      = std::min( spreadMin, spread );
			bad += ( q[ 0 ] != 0 || q[ 1 ] != 255 || o > spread || !( s > 0.0 && s < 1.0 ) ) ? 1 : 0;
		}
		Check( bad == 0, std::string( r.name ) + ": all 31 columns " + triple + ", red 0 (on the sRGB boundary) and "
						 "green 255 (peak 1); xy (" + F( px, 4 ) + ", " + F( py, 4 ) + ") lies on the line from D65 to "
						 "(0.24, 0.41) within half a code value's spread (worst excess " + F( worstOff, 5 ) + "), " +
							 F( s * 100.0, 1 ) + " % of the way from D65" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --geometry (G1): every segment's edges where the spec's millimetres put them.
//---------------------------------------------------------------------------
namespace
{
/// One edge of a box drawn with needle's coverage, `clamp( 0.5 - d, 0, 1 )`.
/// That ramp is exactly the overlap of a pixel with the half-plane behind an
/// axis-aligned edge, so the coverage summed from a fully covered pixel out
/// to the cell's end is the distance from that pixel's far side to the edge.
/// `cov` holds the pixels of one cell along one axis, from index `lo`; `m` is
/// a fully covered pixel. Returns the edge's coordinate and adds each
/// partly covered pixel's half-code rounding to `tol`.
double EdgeFrom( const std::vector< int >& code, int lo, int m, int dir, double& tol )
{
	double sum = 0.0;
	for( int i = m; i >= lo && i < lo + static_cast< int >( code.size() ); i += dir )
	{
		const int g = code[ static_cast< size_t >( i - lo ) ];
		sum += g / 255.0;
		if( g > 0 && g < 255 )
			tol += 0.5 / 255.0;
	}
	return dir < 0 ? ( m + 1 ) - sum : m + sum;
}
} // namespace

int runGeometry()
{
	std::printf( "the segments: every edge where the spec's millimetres put it\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	// Float positions near 1280 px carry 1.2e-4 px of rounding and the shader
	// does a handful of operations on them: 1e-3 px covers that tenfold.
	const double kFloatPx = 1e-3;
	Host         host;
	if( !host.Open() )
		return 1;
	struct Case
	{
		VfdBands bands;
		bool     turned;///< Rotation +90 degrees: still axis-aligned, so edges are still sums
	};
	for( const VfdRaster& r : kVfdRasters )
		for( Case k : { Case{ VfdBands::Thirds, false }, Case{ VfdBands::Octaves, false }, Case{ VfdBands::Thirds, true } } )
		{
			// The programme 22.5 dB under Reference (it tilts +-2 dB across the
			// columns; an octave sums three thirds, +4.8 dB): between four and
			// seven segments lit from the bottom and the top five always dark,
			// so a mirrored or upside-down display cannot pass for this one.
			VfdRig rig( r, Programme( ref - 22.5, 0.0, kFeedRate ), k.bands, 0.0, [ & ]( Instance& i ) {
				if( k.turned )
					i.set( PT_ROTATION, 0.75f );
			} );
			rig.d->Settle( 1.5 );
			const Image img = rig.Draw();
			// Spec section 8 through the controls' own laws, and nothing from
			// vfd::Picture: glass 30 mm tall at SizeFraction of the short side,
			// a 93 x 24 mm field, segments 2.2/3 of the pitch wide and 1.5 mm
			// tall on a 2 mm row pitch, the bottom row (no wire crosses it).
			const int    n     = k.bands == VfdBands::Thirds ? 31 : 10;
			const double mm    = SizeFraction( r.size ) * std::min( r.w, r.h ) * 0.5 / 30.0;
			const double pitch = 93.0 / n * mm, rowPitch = 2.0 * mm;
			const double segW = 93.0 / n * 2.2 / 3.0 * mm, segH = 1.5 * mm;
			const double ly   = ( -12.0 + 11.5 * 2.0 ) * mm;
			// Turned +90 degrees a local point (lx, ly) lands at (-ly, lx) from
			// the centre: the shader's rotation, inverted by hand here.
			const double cellX = k.turned ? rowPitch : pitch, cellY = k.turned ? pitch : rowPitch;
			const double extX = k.turned ? segH : segW, extY = k.turned ? segW : segH;
			double       worst = 0.0, tolMax = 0.0, wLo = 1e9, wHi = -1e9, hLo = 1e9, hHi = -1e9;
			int          bad = 0, measured = 0;
			for( int c = 0; c < n; ++c )
			{
				const double lx = ( -46.5 + ( c + 0.5 ) * 93.0 / n ) * mm;
				const double cx = r.w * 0.5 + ( k.turned ? -ly : lx );
				const double cy = r.h * 0.5 + ( k.turned ? lx : ly );
				// The pixels whose centres lie in this cell, across and down.
				const int x0 = static_cast< int >( std::ceil( cx - 0.5 * cellX - 0.5 ) );
				const int x1 = static_cast< int >( std::ceil( cx + 0.5 * cellX - 0.5 ) ) - 1;
				const int y0 = static_cast< int >( std::ceil( cy - 0.5 * cellY - 0.5 ) );
				const int y1 = static_cast< int >( std::ceil( cy + 0.5 * cellY - 0.5 ) ) - 1;
				// Turned, the glass is taller than a 16:9 raster: only the
				// cells wholly on it are measured.
				if( x0 < 0 || y0 < 0 || x1 >= r.w || y1 >= r.h )
					continue;
				++measured;
				const int mx = static_cast< int >( std::floor( cx ) ), my = static_cast< int >( std::floor( cy ) );
				std::vector< int > across, down;
				for( int x = x0; x <= x1; ++x )
					across.push_back( img.at( x, my )[ 1 ] );
				for( int y = y0; y <= y1; ++y )
					down.push_back( img.at( mx, y )[ 1 ] );
				double tol = 0.0;
				const double left = EdgeFrom( across, x0, mx, -1, tol ), right = EdgeFrom( across, x0, mx, +1, tol );
				const double top = EdgeFrom( down, y0, my, -1, tol ), bottom = EdgeFrom( down, y0, my, +1, tol );
				tol += kFloatPx;
				const double e = std::max( { std::fabs( left - ( cx - 0.5 * extX ) ), std::fabs( right - ( cx + 0.5 * extX ) ),
											 std::fabs( top - ( cy - 0.5 * extY ) ), std::fabs( bottom - ( cy + 0.5 * extY ) ) } );
				const bool interior = img.at( mx, my )[ 1 ] == 255;
				bad += ( !interior || e > tol ) ? 1 : 0;
				worst  = std::max( worst, e );
				tolMax = std::max( tolMax, tol );
				wLo = std::min( wLo, right - left );
				wHi = std::max( wHi, right - left );
				hLo = std::min( hLo, bottom - top );
				hHi = std::max( hHi, bottom - top );
			}
			Check( bad == 0 && measured >= std::min( n, 15 ),
				   std::string( r.name ) + ", " + std::to_string( n ) + " columns" + ( k.turned ? ", turned 90 degrees" : "" ) +
					   ": " + std::to_string( measured ) + " bottom segments' four edges within " + F( worst, 4 ) +
					   " px of the spec's (limit the edge pixels' half-code rounding + 0.001 px of float, at most " +
					   F( tolMax, 4 ) + "); " + F( wLo, 3 ) + " .. " + F( wHi, 3 ) + " x " + F( hLo, 3 ) + " .. " +
					   F( hHi, 3 ) + " px against " + F( extX, 3 ) + " x " + F( extY, 3 ) );
		}
	return Done( before );
}

//---------------------------------------------------------------------------
// --resize (R1): the hold survives a raster change mid-run.
//---------------------------------------------------------------------------
int runResize()
{
	std::printf( "a raster change mid-hold changes the drawing and nothing else\n\n" );
	const int    before = failures;
	const double ref    = ReferenceDefault();
	Host         host;
	if( !host.Open() )
		return 1;
	for( int k = 0; k < 2; ++k )
	{
		const VfdRaster& from = kVfdRasters[ k ];
		const VfdRaster& to   = kVfdRasters[ 1 - k ];
		// Programme for 1 s, then silence: 0.4 s later every dot is held.
		VfdRig a( from, Programme( ref, 6.0, kFeedRate, 1.0 ) );
		VfdRig b( to, Programme( ref, 6.0, kFeedRate, 1.0 ) );
		a.d->Settle( 1.4 );
		b.d->Settle( 1.4 );
		a.Draw();
		b.Draw();
		int dotsBefore = 0;
		int barsA[ vfd::kMaxColumns ], dotsA[ vfd::kMaxColumns ];
		for( int c = 0; c < vfd::kMaxColumns; ++c )
		{
			dotsBefore += a.i->plugin.VfdDisplay().At( c ).dot >= 0 ? 1 : 0;
			barsA[ c ] = a.i->plugin.VfdDisplay().At( c ).lit;
			dotsA[ c ] = a.i->plugin.VfdDisplay().At( c ).dot;
		}
		a.Resize( to );
		const Image ia = a.Draw();
		const Image ib = b.Draw();
		int same = 0;
		for( int c = 0; c < vfd::kMaxColumns; ++c )
			same += ( a.i->plugin.VfdDisplay().At( c ).dot == b.i->plugin.VfdDisplay().At( c ).dot &&
					  a.i->plugin.VfdDisplay().At( c ).lit == b.i->plugin.VfdDisplay().At( c ).lit )
						? 1
						: 0;
		(void)barsA;
		(void)dotsA;
		long differ = 0;
		for( size_t j = 0; j < ia.px.size() && j < ib.px.size(); ++j )
			differ += ia.px[ j ] != ib.px[ j ] ? 1 : 0;
		Check( dotsBefore >= 20 && same == vfd::kMaxColumns && ia.px.size() == ib.px.size() && differ == 0,
			   std::string( from.name ) + " -> " + to.name + " with " + std::to_string( dotsBefore ) +
				   " dots held: every column's bar and dot equal a run drawn at " + to.name + " throughout (" +
				   std::to_string( same ) + "/31), and the picture is byte-identical to it (" + std::to_string( differ ) +
				   " subpixels differ)" );
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --devices (D1): the menu, the states and the lamp, on the scripted backend.
//---------------------------------------------------------------------------
namespace
{
/// An element's stored value, as the host reads it (FF_GET_PARAM_ELEMENT_DEFAULT).
float ElementValue( NeedlePlugin& p, unsigned int id, unsigned int e )
{
	const FFMixed mixed = p.GetParamElementDefault( id, e );
	float         value = 0.0f;
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

bool LampIs( const unsigned char* px, double r, double g, double b )
{
	return std::fabs( px[ 0 ] - r * 255.0 ) <= 1.0 && std::fabs( px[ 1 ] - g * 255.0 ) <= 1.0 &&
		   std::fabs( px[ 2 ] - b * 255.0 ) <= 1.0;
}
} // namespace

int runDevices()
{
	std::printf( "the device menu, the states and the lamp, on a scripted backend (no audio device is touched)\n\n" );
	const int before = failures;
	// NEEDLE_NO_DEVICES is for hosts: it refuses every device name, scripted
	// or real, so inherited from verify.sh it would fail (d)-(i) -- and make
	// menuByIndex look caught for the wrong reason. This backend opens
	// nothing real; (j) sets the gate itself to prove it refuses.
	unsetenv( "NEEDLE_NO_DEVICES" );
	Host      host;
	if( !host.Open() )
		return 1;
	for( const VfdRaster& r : kVfdRasters )
	{
		{
			std::lock_guard< std::mutex > lock( g_deviceMutex );
			g_deviceNames = { "Test Mic", "Test Silent" };
		}
		std::printf( "  -- %s\n", r.name );
		Target t;
		t.Create( r.w, r.h );
		auto inst = std::make_unique< Instance >( r.w, r.h );
		NeedlePlugin& p = inst->plugin;
		p.SetFloatParameter( PT_SIZE, r.size );
		double host_ = 0.0;
		auto   draw  = [ & ]() {
            glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
            glViewport( 0, 0, t.w, t.h );
            glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
            glClear( GL_COLOR_BUFFER_BIT );
            glBindFramebuffer( GL_FRAMEBUFFER, 0 );
            p.SetTime( host_ );
            host_ += 1.0 / 60.0;
            ProcessOpenGLStruct gl = {};
            gl.HostFBO             = t.fbo;
            p.ProcessOpenGL( &gl );
            return readBack( t );
		};
		auto lamp = [ & ]( const Image& img ) {
			const vfd::Picture q = p.BuildVfdPicture( r.w, r.h );
			int                x = 0, y = 0;
			VfdPixel( q, q.lamp[ 0 ], q.lamp[ 1 ], x, y );
			return std::vector< unsigned char >( img.at( x, y ), img.at( x, y ) + 4 );
		};
		auto state = [ & ]() {
			earshot::Engine* e = p.VfdInput().EngineForTest();
			return e != nullptr ? e->captureState() : earshot::CaptureState::Closed;
		};
		auto waitFor = [ & ]( std::function< bool() > done, double seconds ) {
			const auto start = std::chrono::steady_clock::now();
			while( std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() < seconds )
			{
				draw();
				if( done() )
					return true;
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			return false;
		};
		auto keyOf = [ & ]( const char* label ) {
			for( unsigned int e = 0; e < p.GetNumParamElements( PT_INPUT ); ++e )
				if( std::string( p.GetParamElementName( PT_INPUT, e ) ) == label )
					return ElementValue( p, PT_INPUT, e );
			return -1.0f;
		};
		auto dark = [ & ]( const Image& img ) {
			const vfd::Picture q = p.BuildVfdPicture( r.w, r.h );
			int                n = 0;
			for( int c = 0; c < q.cols; ++c )
				n += LitCount( img, q, c );
			return n;
		};
		const vfd::Input::Counters& calls = vfd::Input::Calls();
		const int lists0 = calls.lists.load(), engines0 = calls.engines.load();

		// (a) Before the first VFD frame: nothing listed, nothing built.
		{
			bool ok = p.GetNumParamElements( PT_INPUT ) == static_cast< unsigned >( kInputSlots );
			ok      = ok && std::string( p.GetParamElementName( PT_INPUT, 0 ) ) == "None (host FFT)" &&
				 ElementValue( p, PT_INPUT, 0 ) == 0.0f &&
				 std::string( p.GetParamElementName( PT_INPUT, 1 ) ) == "System default" &&
				 ElementValue( p, PT_INPUT, 1 ) == earshot::deviceKey( earshot::kDefaultDevice );
			for( int e = 2; e < kInputSlots; ++e )
				ok = ok && std::string( p.GetParamElementName( PT_INPUT, static_cast< unsigned >( e ) ) ) == "(none)";
			draw();
			Check( ok && p.VfdInput().EngineForTest() == nullptr && calls.lists.load() == lists0,
				   "(a) VU drawn: the menu is None, System default and 14 (none), and nothing has listed or built" );
		}
		// (b) The first VFD frame lists, once, and tells the host.
		{
			ParamEventStruct events[ PT_COUNT_ ];
			p.ConsumeParamEvents( events, PT_COUNT_ );
			p.SetFloatParameter( PT_TYPE, static_cast< float >( kTypeVfd ) );
			p.ConsumeParamEvents( events, PT_COUNT_ );
			const Image img = draw();
			const FFUInt32 n = p.ConsumeParamEvents( events, PT_COUNT_ );
			bool raised = false;
			for( FFUInt32 e = 0; e < n; ++e )
				raised = raised || ( events[ e ].ParameterNumber == PT_INPUT && ( events[ e ].eventFlags & FF_EVENT_FLAG_ELEMENTS ) != 0 );
			const char* names[] = { "None (host FFT)", "System default", "Test Mic", "Test Silent" };
			const float keys[]  = { 0.0f, earshot::deviceKey( earshot::kDefaultDevice ), earshot::deviceKey( "Test Mic" ),
									earshot::deviceKey( "Test Silent" ) };
			bool        ok = true;
			std::string shown;
			for( unsigned int e = 0; e < p.GetNumParamElements( PT_INPUT ); ++e )
			{
				const std::string label = p.GetParamElementName( PT_INPUT, e );
				const float       value = ElementValue( p, PT_INPUT, e );
				if( e < 4 )
				{
					ok = ok && label == names[ e ] && value == keys[ e ];
					shown += " \"" + label + "\"=" + F( value, 0 );
				}
				else
					ok = ok && label == "(none)" && value == 0.0f;
			}
			const auto l = lamp( img );
			Check( ok && raised && calls.lists.load() == lists0 + 1 && calls.engines.load() == engines0 + 1,
				   "(b) the first VFD frame lists once and raises FF_EVENT_FLAG_ELEMENTS: by name and key" + shown +
					   ", then (none) = 0" );
			Check( LampIs( l.data(), 1.0, 0.62, 0.05 ) && dark( img ) == 0,
				   "(c) no device: the lamp amber (" + std::to_string( l[ 0 ] ) + ", " + std::to_string( l[ 1 ] ) + ", " +
					   std::to_string( l[ 2 ] ) + ") and the display dark (Resolume's FFT has no thirds)" );
		}
		// (d) A device that delivers.
		p.SetFloatParameter( PT_INPUT, keyOf( "Test Mic" ) );
		{
			const bool running = waitFor(
				[ & ] { return state() == earshot::CaptureState::Running && p.VfdInput().LampState() == vfd::Lamp::Green; }, 5.0 );
			waitFor( [] { return false; }, 0.3 );
			const Image img = draw();
			const auto  l   = lamp( img );
			Check( running && LampIs( l.data(), 0.15, 1.0, 0.25 ) && dark( img ) > 0,
				   "(d) Test Mic: " + std::string( earshot::toString( state() ) ) + ", the lamp green (" + std::to_string( l[ 0 ] ) +
					   ", " + std::to_string( l[ 1 ] ) + ", " + std::to_string( l[ 2 ] ) + "), " +
					   std::to_string( dark( img ) ) + " segments lit by its 1 kHz sine" );
		}
		// (e) A device that delivers zeros.
		p.SetFloatParameter( PT_INPUT, keyOf( "Test Silent" ) );
		{
			const bool  silent = waitFor( [ & ] { return state() == earshot::CaptureState::Silent; }, 8.0 );
			const Image i1 = draw();
			host_ += 0.5;
			const Image i2 = draw();
			const auto  l1 = lamp( i1 ), l2 = lamp( i2 );
			Check( silent && LampIs( l1.data(), 1.0, 0.05, 0.02 ) && LampIs( l2.data(), 1.0, 0.05, 0.02 ),
				   "(e) Test Silent: " + std::string( earshot::toString( state() ) ) + ", the lamp steady red (" +
					   std::to_string( l1[ 0 ] ) + ", " + std::to_string( l1[ 1 ] ) + ", " + std::to_string( l1[ 2 ] ) +
					   ") and half a second later (" + std::to_string( l2[ 0 ] ) + ", " + std::to_string( l2[ 1 ] ) + ", " +
					   std::to_string( l2[ 2 ] ) + ")" );
		}
		// (f) A saved device that is not here: Missing, nothing in its place.
		const float gone       = earshot::deviceKey( "Gone Mic" );
		const int   opensBefore = g_opens.load();
		p.SetFloatParameter( PT_INPUT, gone );
		{
			const bool missing = waitFor( [ & ] { return state() == earshot::CaptureState::Missing; }, 5.0 );
			host_              = std::floor( host_ ) + 1.1;
			const Image i1     = draw();// 0.1 into a second: on
			host_ += 0.5 - 1.0 / 60.0;
			const Image i2 = draw();    // 0.6: off
			const auto  l1 = lamp( i1 ), l2 = lamp( i2 );
			Check( missing && g_opens.load() == opensBefore && LampIs( l1.data(), 1.0, 0.03, 0.02 ) &&
					   LampIs( l2.data(), 0.15, 0.03, 0.02 ),
				   "(f) a saved key with no device: " + std::string( earshot::toString( state() ) ) +
					   ", nothing opened in its place, the lamp blinking red at 1 Hz (R " + std::to_string( l1[ 0 ] ) +
					   " then " + std::to_string( l2[ 0 ] ) + ")" );
		}
		// (g) The device comes back; Rescan lists it and reopens it by name.
		{
			std::lock_guard< std::mutex > lock( g_deviceMutex );
			g_deviceNames.push_back( "Gone Mic" );
		}
		p.SetFloatParameter( PT_RESCAN, 1.0f );
		p.SetFloatParameter( PT_RESCAN, 0.0f );
		{
			const bool listed  = keyOf( "Gone Mic" ) == gone;
			const bool running = waitFor( [ & ] { return state() == earshot::CaptureState::Running; }, 5.0 );
			earshot::Status st;
			p.VfdInput().EngineForTest()->status( st );
			Check( listed && running && std::string( st.opened ) == "Gone Mic",
				   "(g) Rescan with Gone Mic connected: listed with its key, reopened by name (opened \"" +
					   std::string( st.opened ) + "\")" );
		}
		// (h) Leaving VFD closes the device; Rescan does nothing off VFD;
		// coming back reopens it.
		{
			p.SetFloatParameter( PT_TYPE, 2.0f );
			draw();
			const bool closed = waitFor( [ & ] { return state() == earshot::CaptureState::Closed; }, 5.0 );
			const int  lists  = calls.lists.load();
			p.SetFloatParameter( PT_RESCAN, 1.0f );
			p.SetFloatParameter( PT_RESCAN, 0.0f );
			draw();
			const bool quiet = calls.lists.load() == lists;
			p.SetFloatParameter( PT_TYPE, static_cast< float >( kTypeVfd ) );
			const bool back = waitFor( [ & ] { return state() == earshot::CaptureState::Running; }, 5.0 );
			Check( closed && quiet && back, "(h) Type to the bargraph closes the device (" + std::string( closed ? "Closed" : "still open" ) +
												"), Rescan there lists nothing, back to VFD reopens it" );
		}
		inst.reset();
		// (i) A composition saved with a device: the key arrives with the VFD
		// off, and the device opens on the first VFD frame, by name.
		{
			auto          inst2 = std::make_unique< Instance >( r.w, r.h );
			NeedlePlugin& p2    = inst2->plugin;
			const int     sel   = calls.selects.load();
			p2.SetFloatParameter( PT_INPUT, earshot::deviceKey( "Test Mic" ) );
			const bool quietOff = calls.selects.load() == sel && p2.VfdInput().EngineForTest() == nullptr;
			p2.SetFloatParameter( PT_TYPE, static_cast< float >( kTypeVfd ) );
			ProcessOpenGLStruct gl = {};
			gl.HostFBO             = t.fbo;
			bool       running = false;
			const auto start   = std::chrono::steady_clock::now();
			while( !running && std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() < 5.0 )
			{
				p2.SetTime( host_ += 1.0 / 60.0 );
				p2.ProcessOpenGL( &gl );
				earshot::Engine* e = p2.VfdInput().EngineForTest();
				running            = e != nullptr && e->captureState() == earshot::CaptureState::Running;
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			earshot::Status st;
			if( p2.VfdInput().EngineForTest() )
				p2.VfdInput().EngineForTest()->status( st );
			Check( quietOff && running && std::string( st.opened ) == "Test Mic",
				   "(i) a saved Test Mic key: nothing selected while the VFD is off, opened by name on the first VFD frame" );
		}
		// (j) NEEDLE_NO_DEVICES refuses a device name before earshot sees it.
		{
			setenv( "NEEDLE_NO_DEVICES", "1", 1 );
			auto          inst3 = std::make_unique< Instance >( r.w, r.h );
			NeedlePlugin& p3    = inst3->plugin;
			p3.SetFloatParameter( PT_TYPE, static_cast< float >( kTypeVfd ) );
			p3.SetFloatParameter( PT_INPUT, earshot::deviceKey( "Test Mic" ) );
			ProcessOpenGLStruct gl = {};
			gl.HostFBO             = t.fbo;
			const int sel          = calls.selects.load();
			for( int k = 0; k < 20; ++k )
			{
				p3.SetTime( host_ += 1.0 / 60.0 );
				p3.ProcessOpenGL( &gl );
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			earshot::Engine* e = p3.VfdInput().EngineForTest();
			Check( calls.selects.load() == sel && e != nullptr && e->captureState() == earshot::CaptureState::Closed &&
					   p3.VfdInput().LampState() == vfd::Lamp::Amber,
				   "(j) NEEDLE_NO_DEVICES=1: a chosen device is never handed to earshot (Closed, lamp amber)" );
			unsetenv( "NEEDLE_NO_DEVICES" );
		}
		t.Destroy();
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --coreaudio (C1): with the VFD off, nothing reaches earshot at all.
//---------------------------------------------------------------------------
int runCoreAudio()
{
	std::printf( "VFD off never touches CoreAudio: the real backend, every control, every other Type\n\n" );
	const int before = failures;
	unsetenv( "NEEDLE_NO_DEVICES" );
	Host host;
	if( !host.Open() )
		return 1;
	const vfd::Input::Counters& calls = vfd::Input::Calls();
	for( const VfdRaster& r : kVfdRasters )
	{
		Target t;
		t.Create( r.w, r.h );
		bool engines = false;
		{
			Instance i( r.w, r.h );
			ProcessOpenGLStruct gl = {};
			gl.HostFBO             = t.fbo;
			double now             = 0.0;
			for( int type = 0; type < kMeterTypeCount; ++type )
			{
				i.set( PT_TYPE, static_cast< float >( type ) );
				for( unsigned int id = 0; id < PT_COUNT_; ++id )
				{
					if( id == PT_TYPE || id == PT_AUDIO || ( id >= PT_ABOUT_TEXT && id < kAboutEnd ) )
						continue;// About buttons open a browser
					const float values[] = { 0.0f, 1.0f, 0.5f };
					for( float v : values )
						i.set( id, v );
				}
				i.set( PT_INPUT, earshot::deviceKey( "Test Mic" ) );
				i.set( PT_INPUT, earshot::deviceKey( earshot::kDefaultDevice ) );
				i.set( PT_RESCAN, 1.0f );
				i.set( PT_RESCAN, 0.0f );
				i.inject( -12.0 );
				for( int f = 0; f < 5; ++f )
				{
					i.plugin.SetTime( now += 1.0 / 60.0 );
					i.plugin.ProcessOpenGL( &gl );
				}
			}
			engines = i.plugin.VfdInput().EngineForTest() != nullptr;
		}
		Check( calls.engines.load() == 0 && calls.lists.load() == 0 && calls.selects.load() == 0 && !engines,
			   std::string( r.name ) + ": VU, PPM, bargraph and eye, every control at 0, 1 and 0.5, Audio Input set to "
				   "two device keys, Rescan pressed, 20 frames drawn: earshot Engines built " +
				   std::to_string( calls.engines.load() ) + ", device lists " + std::to_string( calls.lists.load() ) +
				   ", selects " + std::to_string( calls.selects.load() ) + " (all must be 0)" );
		t.Destroy();
	}
	return Done( before );
}

//---------------------------------------------------------------------------
// --negative: every planted bug must make its check fail.
//---------------------------------------------------------------------------
int runNegatives( const char* self )
{
	std::printf( "negative controls: each plants one bug, and its check must FAIL\n\n" );
	int missed = 0;
	for( const NegativeEntry& n : kNegatives )
	{
		std::vector< std::string > env;
		for( char** e = environ; *e != nullptr; ++e )
			if( std::strncmp( *e, "NDTEST_NEGATIVE=", 16 ) != 0 )
				env.emplace_back( *e );
		env.push_back( std::string( "NDTEST_NEGATIVE=" ) + n.name );
		std::vector< char* > envp;
		for( std::string& s : env )
			envp.push_back( s.data() );
		envp.push_back( nullptr );
		const char*                argv[] = { self, n.check, nullptr };
		posix_spawn_file_actions_t fa;
		posix_spawn_file_actions_init( &fa );
		posix_spawn_file_actions_addopen( &fa, 1, "/dev/null", O_WRONLY, 0 );
		posix_spawn_file_actions_addopen( &fa, 2, "/dev/null", O_WRONLY, 0 );
		pid_t pid    = 0;
		int   status = 0;
		const int rc = posix_spawn( &pid, self, &fa, nullptr, const_cast< char* const* >( argv ), envp.data() );
		posix_spawn_file_actions_destroy( &fa );
		if( rc == 0 )
			waitpid( pid, &status, 0 );
		const bool caught = rc == 0 && WIFEXITED( status ) && WEXITSTATUS( status ) != 0;
		std::printf( "  %s %-20s %-12s %s\n", caught ? "caught" : "MISSED", n.name, n.check, n.what );
		missed += caught ? 0 : 1;
	}
	std::printf( "\n  %d of %zu caught\n", static_cast< int >( sizeof( kNegatives ) / sizeof( kNegatives[ 0 ] ) ) - missed,
				 sizeof( kNegatives ) / sizeof( kNegatives[ 0 ] ) );
	return missed == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --out / --pipe on Type = VFD, and the bench: the programme through feed().
//---------------------------------------------------------------------------
namespace
{
/// The programme at a TOTAL level: 31 equal sines sum to 10 lg 31 = 14.9 dB
/// over each one, so each sits that far under `--level`. At the default
/// -18 dBFS a band reads about -33 dBFS, eight segments under Reference.
double PerBandDb( double totalDb )
{
	return totalDb - 10.0 * std::log10( 31.0 );
}

/// Feeds a plugin on Type = VFD the harness's programme, a frame at a time.
/// Built on the first VFD frame, so a render that never selects the VFD never
/// builds an Engine.
struct VfdFeed
{
	std::unique_ptr< Driver > d;
	double                    level = -18.0, stop = -1.0, fps = 60.0;

	void Before( NeedlePlugin& p )
	{
		if( !p.IsVfd() )
			return;
		if( !d )
			d = std::make_unique< Driver >( p, Programme( PerBandDb( level ), 9.0, kFeedRate, stop ), kFeedRate, fps );
		d->Push( FrameSamples( d->frame++, fps, kFeedRate ) );
	}
};
} // namespace

//---------------------------------------------------------------------------
// --bench's VFD rows: Type = VFD with the programme fed, so earshot's
// update() (all of its analysis, every frame) is in the number. The samples
// are made once beforehand: generating 31 sines is the harness's cost, not
// the plugin's.
//---------------------------------------------------------------------------
int benchVfd()
{
	std::printf( "\n  VFD (Type = VFD; the programme through feed(), earshot's update() included):\n" );
	auto buffer = std::make_shared< std::vector< float > >( kFeedRate );
	{
		const Signal s = Programme( PerBandDb( ReferenceDefault() ), 9.0, kFeedRate );
		for( int k = 0; k < kFeedRate; ++k )
			( *buffer )[ static_cast< size_t >( k ) ] = static_cast< float >( s( k + kFeedRate ) );
	}
	const Signal looped = [ buffer ]( long long k ) { return static_cast< double >( ( *buffer )[ static_cast< size_t >( k % kFeedRate ) ] ); };
	struct Size
	{
		int         w, h;
		const char* name;
	};
	for( const Size& s : { Size{ 1280, 720, "1280x720" }, Size{ 1920, 1080, "1920x1080" } } )
	{
		Target t;
		t.Create( s.w, s.h );
		{
			Instance i( s.w, s.h );
			SetVfd( i.plugin, VfdBands::Thirds );
			Driver              d( i.plugin, looped );
			ProcessOpenGLStruct gl = {};
			gl.HostFBO             = t.fbo;
			auto frame             = [ & ]() {
                d.Push( FrameSamples( d.frame++, 60.0, kFeedRate ) );
                i.plugin.SetTime( d.Host() );
                i.plugin.ProcessOpenGL( &gl );
			};
			for( int f = 0; f < 30; ++f )
				frame();
			glFinish();
			double best = 1e9, worst = 0.0;
			for( int pass = 0; pass < 5; ++pass )
			{
				const auto start = std::chrono::steady_clock::now();
				for( int f = 0; f < 200; ++f )
					frame();
				glFinish();
				const double ms =
					std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / 200.0;
				best  = std::min( best, ms );
				worst = std::max( worst, ms );
			}
			std::printf( "  %-12s %-10s %-14s %.1f%%\n", s.name, F( best, 3 ).c_str(),
						 ( F( best, 3 ) + "-" + F( worst, 3 ) ).c_str(), 100.0 * best / ( 1000.0 / 60.0 ) );
		}
		t.Destroy();
	}
	return 0;
}
