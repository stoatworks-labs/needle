#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Controls.h"

namespace earshot
{
class Engine;
struct Frame;
namespace testing
{
class CaptureBackend;
}
} // namespace earshot

/**
	The VFD spectrum analyser: Type = VFD. Everything here is the fifth
	instrument and nothing else; the four meters never reach this file.

	Three parts, each testable without a host, two of them without GL:

	  `Input`    the only code in needle that calls earshot. It owns the
	             Engine, the device menu and the lamp, and it counts every
	             call that can start CoreAudio, because the rule is that VFD
	             off never starts it -- not even to list devices.
	  `Display`  earshot's third-octave levels into columns: octaves summed
	             from thirds, segments at 3 dB, the peak hold and its fall in
	             audio time.
	  `Picture`  what the shader is handed, in pixels, built on the CPU.

	The physics, and a source for every number below, is in
	specs/SPEC-needle-vfd.md. The short version: the bank is earshot's
	(IEC 61260 base-ten centres, Fast weighting), one grid per column is
	scanned at the Multiplex Rate with 20 us of blanking (Noritake), four
	37.5 um filament wires cross the glass (Futaba US6803709), and the
	phosphor is Noritake's standard green, CIE (0.24, 0.41), which is ZnO:Zn.
*/
namespace needle::vfd
{
// ---------------------------------------------------------------------------
// The tube. Sourced figures first, then the design choices the spec names as
// such.
// ---------------------------------------------------------------------------
constexpr int    kMaxColumns      = 31;     ///< 20 Hz .. 20 kHz thirds
constexpr int    kRows            = 12;     ///< segments per column
constexpr double kStepDb          = 3.0;    ///< per segment (the bargraph's LM3915 step)
constexpr double kBlankingSeconds = 20.0e-6;///< Noritake: "approximately 10 to 50usec"
constexpr double kReadoutSeconds  = 0.016;  ///< the camera's readout, pitch's default
constexpr double kWireMm          = 0.0375; ///< 24.5 um core + 2 x 6.5 um oxide (US6803709)
constexpr int    kWires           = 4;
constexpr double kPhosphorX = 0.24, kPhosphorY = 0.41;///< Noritake Table 6, standard green

// Geometry, millimetres (design: no product drawing was consulted).
constexpr double kGlassWMm   = 100.0;
constexpr double kGlassHMm   = 30.0;
constexpr double kFieldWMm   = 93.0;
constexpr double kFieldHMm   = 24.0;
constexpr double kRowPitchMm = 2.0;
constexpr double kSegHMm     = 1.5;
constexpr double kSegFill    = 2.2 / 3.0;///< segment width over column pitch
constexpr double kCornerMm   = 1.5;
/// Wire centres, millimetres below the top of the bar field: across segment
/// rows 1, 4, 7 and 10 from the top.
constexpr double kWireYMm[ kWires ] = { 3.0, 9.0, 15.0, 21.0 };
constexpr double kLampXMm = 47.75, kLampYMm = -13.5, kLampRMm = 0.9;

/// How many columns a layout has: 31 or 10.
int ColumnCount( VfdBands bands );
/// IEC 61260 base-ten band index x (fm = 1000 * 10^(x/10) Hz) of a column's
/// band; for an octave, its centre third.
int ColumnX( VfdBands bands, int column );

/// Noritake's standard green in linear sRGB, mapped onto the gamut boundary
/// toward D65 and normalised to a peak channel of 1. Derived in double from the
/// CSS Color 4 primaries, not typed in.
void PhosphorLinear( double rgb[ 3 ] );
/// The sRGB transfer function (CSS Color 4, section 10.2), both ways.
double SrgbDecode( double encoded );
double SrgbEncode( double linear );
/// CIE 1931 xy of a linear sRGB colour.
void LinearToXy( const double rgb[ 3 ], double& x, double& y );

// ---------------------------------------------------------------------------
// Negative controls: each is a bug the harness plants to prove a check can
// fail. Only ndtest sets them (NDTEST_NEGATIVE=name); the plugin never does.
// ---------------------------------------------------------------------------
struct Negatives
{
	bool holdInFrames  = false;///< H1: hold and fall counted in 60 fps frames
	bool holdNoWait    = false;///< H1: the fall starts the moment the level drops
	bool holdAtPeakOnly = false;///< H1, H2: earshot's rule, the wait starts at the level's last maximum
	bool amplitudeSum  = false;///< O1: octaves summed in amplitude, not power
	bool octaveWrong   = false;///< O1: an octave from thirds x, x+1, x+2
	int  shift         = 0;    ///< I2: column c shows band c + shift
	bool phaseFloat    = false;///< M3: the scan phase computed in float
	bool readoutUp     = false;///< M2: the camera reads bottom to top
	bool normFullSlot  = false;///< M1: the camera normalised by the slot, not the pulse
	bool wireCoreOnly  = false;///< F1: the wire's core without its oxide coat
	bool wireTen       = false;///< F1: the wire ten times thick
	bool willemite     = false;///< P1: the magic eye's green instead of ZnO:Zn
	bool segFull       = false;///< G1: segments as wide as the column pitch
	bool resetOnResize = false;///< R1: the hold forgotten when the raster changes
	bool showAllOnVfd  = false;///< V1: every control shown on the VFD
	bool menuByIndex   = false;///< D1: device elements valued by position
};
Negatives& Neg();

// ---------------------------------------------------------------------------
// The display.
// ---------------------------------------------------------------------------
struct Column
{
	bool   present = false;  ///< a band exists for this column at this rate
	double levelDb = -400.0; ///< trimmed, dB re a full-scale sine
	double holdDb  = -400.0;
	double heldFor = 0.0;    ///< audio seconds since the hold was set
	int    lit     = 0;      ///< segments lit from the bottom
	int    dot     = -1;     ///< segment index of the hold dot; -1 for none
};

struct DisplaySettings
{
	VfdBands bands           = VfdBands::Thirds;
	double   referenceDbfs   = -18.0;
	double   trimDb          = 0.0;
	double   holdSeconds     = 1.5;
	double   fallDbPerSecond = 12.0;
};

/// Segments lit for a (trimmed) level: segment i lights at
/// Reference - 3 * (12 - i), so the top one 3 dB under Reference.
int LitFor( double levelDb, double referenceDbfs );

class Display
{
public:
	/// One frame. `frame` is null when there is nothing to show (no engine,
	/// or not drawn); a frame from Resolume's FFT has no thirds and shows
	/// nothing either.
	void Advance( const earshot::Frame* frame, const DisplaySettings& s );
	void Reset();

	int           Count() const { return count; }
	const Column& At( int c ) const { return cols[ c ]; }
	VfdBands      Bands() const { return bands; }

private:
	Column   cols[ kMaxColumns ];
	int      count     = kMaxColumns;
	VfdBands bands     = VfdBands::Thirds;
	double   lastAudio = 0.0;
	uint32_t gen       = 0;
	bool     started   = false;
};

// ---------------------------------------------------------------------------
// Devices.
// ---------------------------------------------------------------------------
enum class Lamp : int
{
	Amber = 0,///< Resolume's FFT (no device, or the device is down): the display is dark
	Green,    ///< a device delivers
	Red,      ///< open and bit-exactly silent
	RedBlink, ///< chosen but missing, or the backend refused it
};

struct Slot
{
	std::string label;///< what the menu shows
	std::string name; ///< what selectDevice is handed ("" for the host FFT)
	float       value = 0.0f;
};

class Input
{
public:
	using BackendFactory = std::unique_ptr< earshot::testing::CaptureBackend > ( * )();

	/// For the harness: every Engine built afterwards uses this backend and
	/// lists ITS devices. Set before any plugin exists; never by the plugin.
	static void SetBackendFactoryForTest( BackendFactory factory );

	/// Every call that can start CoreAudio, counted process-wide (C1).
	struct Counters
	{
		std::atomic< int > engines{ 0 };///< earshot::Engine constructed
		std::atomic< int > lists{ 0 };  ///< devices enumerated
		std::atomic< int > selects{ 0 };///< selectDevice called
	};
	static Counters& Calls();
	/// C1's negative control: woofer's shape, which lists at construction.
	static bool negListAtConstruction;

	Input();
	~Input();

	const std::vector< Slot >& Slots() const { return slots; }
	bool Active() const { return active; }
	/// Bumps every time the menu is rebuilt, so the plugin knows to tell the host.
	int ListCount() const { return listCount; }

	/// The Audio Input value the host stored. Remembered; acted on only while
	/// the VFD is being drawn, and only when it changes.
	void Want( float key );
	/// The first VFD frame (and every one after: cheap when nothing changed):
	/// build the Engine, list the devices once, apply the wanted device.
	void Activate();
	/// Type left VFD: close any open device. Coming back reopens it.
	void Deactivate();
	/// Rescan Inputs: list again and ask for the wanted device again. Does
	/// nothing (and touches nothing) unless active. Returns whether it listed.
	bool Rescan();

	/// Run earshot for this frame. Null when there is no Engine.
	const earshot::Frame* Update( double hostSeconds, const float* fft, int fftCount );
	Lamp LampState() const;

	//-- harness ----------------------------------------------------------
	earshot::Engine* EngineForTest() { return engine.get(); }
	void FeedBegin( int sampleRate, int channels );
	void Feed( const float* interleaved, int frames );

private:
	void BuildEngine();
	void List();
	void Apply();

	std::unique_ptr< earshot::Engine > engine;
	std::vector< Slot >                slots;
	std::map< float, std::string >     seen;///< every device key met, and its name
	float  wanted       = 0.0f;
	float  applied      = 0.0f;
	bool   active       = false;
	bool   listed       = false;
	bool   lastFromDevice = false;
	int    listCount    = 0;
};

// ---------------------------------------------------------------------------
// The picture, in the shader's units: pixels, y downward, the instrument's
// local frame centred on `centre` and turned by `rotation`.
// ---------------------------------------------------------------------------
struct Picture
{
	int   width = 0, height = 0;
	float centre[ 2 ] = { 0.0f, 0.0f };
	float rotation    = 0.0f;
	float halfW = 0.0f, halfH = 0.0f, corner = 0.0f;

	float field[ 4 ] = { 0, 0, 0, 0 };///< x0, y0 of the column-0 row-0 cell; pitch x, pitch y
	float seg[ 2 ]   = { 0, 0 };      ///< segment w, h
	int   cols = 0, rows = kRows;
	float lit[ kMaxColumns ] = {};
	float dot[ kMaxColumns ] = {};

	float phosphor[ 3 ] = { 0, 0, 0 };///< linear
	float wireY[ kWires ] = {};
	float wireW = 0.0f, wireX0 = 0.0f, wireX1 = 0.0f;

	int   camera  = 0;     ///< 0: the eye
	float phase0  = 0.0f;  ///< frac( t0 * f ), from double
	float rowStep = 0.0f;  ///< scans per raster row (readout * f / H)
	float window  = 0.0f;  ///< scans per exposure (E * f)
	float slot    = 0.0f;  ///< 1 / N, in scans
	float blank   = 0.0f;  ///< blanking, in scans
	float pulse   = 0.0f;  ///< the camera's normaliser: the grid's pulse, slot - blank

	float lamp[ 4 ]       = { 0, 0, 0, 0 };///< x, y, radius, on
	float lampColour[ 3 ] = { 0, 0, 0 };
	float back[ 3 ]       = { 0, 0, 0 };
	float backAlpha       = 0.0f;
	float mix             = 1.0f;

	double pxPerMm = 0.0;

	/// Local-frame centre of a segment (`fromBottom` 0 is the bottom one).
	void SegmentCentre( int column, int fromBottom, float& lx, float& ly ) const;
};

} // namespace needle::vfd
