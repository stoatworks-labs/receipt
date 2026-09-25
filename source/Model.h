#pragma once

#include <cstdint>

/**
	The thermal printer as numbers: what the engine computes, written down
	once, with no GL in it. The plugin's C++ (`Printer.cpp`, `Receipt.cpp`)
	uses these constants, tables and laws; the harness restates the model from
	this description and holds the plugin to it.

	**A thermal printer has no ink.** A fixed line of N heaters (8 dots/mm:
	384 across a 58 mm roll, 576 across 80 mm) sits against heat-sensitive
	paper, and the paper darkens where it gets hot enough. The stepper pulls
	the paper one dot row at a time, and each row is one strobe of the head.
	Per row (a "strobe"), in order:

	  dither      the row's tones t (0 black, 1 white) become fire bits b, by
	              the firmware's dither: an 8 x 8 Bayer matrix (b = 1 - t >
	              ( M + 1/2 ) / 64), Floyd-Steinberg (7, 3, 5, 1 sixteenths to
	              the right, down-left, down, down-right) or Atkinson (an
	              eighth to each of x+1, x+2, the three below and two below,
	              so a quarter of every error is thrown away). Raster order,
	              left to right, every row; no serpentine. The dither runs
	              over DATA rows -- the host's lines -- not paper rows.
	  history     the firmware's history control shortens a heater's strobe
	              by what it predicts is still in it:
	              e = b ( e0 - h c theta ), floored at 0, where e0 is the
	              nominal energy (Density), h the History Control amount and
	              c the Heat Carry. At h = 1 a firing heater always reaches
	              exactly e0; nothing can cool a heater that does not fire.
	  budget      the supply can drive kBudgetDots heaters at full energy at
	              once. The head is strobed in S blocks (Strobe Blocks), block
	              k being columns [ floor( k N / S ), floor( ( k + 1 ) N / S ) );
	              a block with n firing heaters sags to
	              e *= min( 1, kBudgetDots / n ). So dense rows print paler,
	              and a row whose blocks differ in coverage bands at the
	              block boundaries.
	  heat        each heater is a one-pole: theta = c theta + e. The heat
	              carried from the last row is what makes a vertical line
	              thicken and a dark area block up.
	  paper       the paper under heater x sees
	              q = theta_x + kSpread ( theta_{x-1} + theta_{x+1} )
	              (a missing neighbour past the head's end is 0), and
	              darkens by the paper's response curve:
	              D = smoothstep( kThreshold, kSaturation, q ), 0 below the
	              threshold, 1 (full optical density) at saturation.
	  overprint   a paper row struck more than once (a stalled feed) keeps
	              the darkest strike: D = max( D_1, D_2, ... ). Leuco dye
	              does not un-develop.

	and the displayed colour is the paper stock attenuated by the dye's
	optical density, OD = kMaxOD x D x k_rgb (Beer-Lambert: reflectance
	10^-OD), with Age fading D, yellowing the stock and browning the dye.

	**The feed.** A paper row normally carries the next data row. A slip
	event at a paper row (seeded by the row's index, at Slip's rate) is one of
	two things, for m = 1..kMaxSlipRows rows:
	  repeat    the paper advances and the line counter does not: the data
	            row is printed on this paper row and the next m, then the
	            data continues. The picture stretches by m rows.
	  stall     the paper does not advance while the data does: this data
	            row and the next m are all struck onto this paper row
	            (overprinted), then the paper advances. The picture skips m
	            rows under a dark band.

	**Everything random is an integer hash** (a PCG output mix, exact in 32
	bits, the fleet's `hashInt`), seeded by row index and nothing else.
*/
namespace receiptfx::model
{

constexpr double kPi = 3.14159265358979323846;

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
/// Each one perturbs the PLUGIN, never the harness's expectation.
enum Perturb : int
{
	kPerturbNone          = 0,
	kPerturbNoCarry       = 1 << 0,///< the heaters carry no heat from row to row (c = 0 whatever Heat Carry says)
	kPerturbNoBudget      = 1 << 1,///< the supply never sags: no energy budget at all
	kPerturbNoDiffusion   = 1 << 2,///< error diffusion throws its error away (a plain 1/2 threshold)
	kPerturbSlipShort     = 1 << 3,///< a slip repeats or stalls one row fewer than it says
	kPerturbFloatPitch    = 1 << 4,///< the display's dot pitch is W / N, not a whole number of pixels
	kPerturbClockPerFrame = 1 << 5,///< Printing advances a 60th of a second per frame, whatever the clock says
	kPerturbResizeClears  = 1 << 6,///< a change of output size clears the printed paper
};

//---------------------------------------------------------------------------
// The head.
//---------------------------------------------------------------------------

constexpr double kDotsPerMm = 8.0;

enum Width : int
{
	k58mm = 0,
	k80mm,
	kWidthCount
};
inline constexpr const char* kWidthNames[ kWidthCount ] = { "58 mm", "80 mm" };
inline constexpr int kWidthDots[ kWidthCount ]          = { 384, 576 };

/// The supply's budget: this many heaters at full energy at once.
constexpr double kBudgetDots = 96.0;

/// A strobe's heat that reaches the paper under each neighbouring heater.
constexpr double kSpread = 0.22;

/// The paper's response: the heat at which it starts to darken, and the
/// heat at which it is at full optical density. The nominal energy e0 is 1
/// at the default Density, so a lone fresh dot just saturates.
constexpr double kThreshold  = 0.35;
constexpr double kSaturation = 1.0;

/// Full optical density of the developed dye (thermal paper: 1.2 to 1.4).
constexpr double kMaxOD = 1.3;

constexpr int kStrobeBlocksMin = 1;
constexpr int kStrobeBlocksMax = 8;

enum Dither : int
{
	kBayer = 0,
	kFloydSteinberg,
	kAtkinson,
	kDitherCount
};
inline constexpr const char* kDitherNames[ kDitherCount ] = { "Bayer", "Floyd-Steinberg", "Atkinson" };

/// The 8 x 8 Bayer index matrix, [ row ][ column ]; the threshold is
/// ( M + 1/2 ) / 64.
inline constexpr int kBayer8[ 8 ][ 8 ] = {
	{ 0, 32, 8, 40, 2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44, 4, 36, 14, 46, 6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{ 3, 35, 11, 43, 1, 33, 9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47, 7, 39, 13, 45, 5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 },
};

//---------------------------------------------------------------------------
// The feed.
//---------------------------------------------------------------------------

enum Mode : int
{
	kStatic = 0,
	kPrinting,
	kModeCount
};
inline constexpr const char* kModeNames[ kModeCount ] = { "Static", "Printing" };

constexpr int kMaxSlipRows = 4;

enum SlipKind : int
{
	kNoSlip = 0,
	kRepeat,
	kStall
};

/// The paper held for Printing: a ring of this many rows. More than any
/// window a host will ask for (a window is ceil( H / pitch ) rows).
constexpr int kPaperRows = 4096;

/// Printing: the image starts this far below a receipt's leading edge (3 mm)
/// rather than centred on the receipt, which at a long Tear Length would feed
/// seconds of blank paper before the picture. Static centres it.
constexpr int kLeadRows = 24;

/// Printing: the most elapsed time one frame may print. A host that stalls
/// for a second must not spit a whole receipt out in one frame.
constexpr double kMaxFrameDelta = 0.25;

/// The torn edge of a receipt: column c loses this many rows at a torn end,
/// a sawtooth of period 8 dots and depth 2 rows.
inline int TearNotch( int column )
{
	const int r = ( ( column % 8 ) + 8 ) % 8;
	const int d = r < 4 ? 4 - r : r - 4;
	return d / 2;
}

//---------------------------------------------------------------------------
// The paper.
//---------------------------------------------------------------------------

enum Fit : int
{
	kLetterbox = 0,
	kRotate,
	kTile,
	kFitCount
};
inline constexpr const char* kFitNames[ kFitCount ] = { "Letterbox", "Rotate", "Tile" };

enum Tint : int
{
	kWhite = 0,
	kIvory,
	kCanary,
	kPink,
	kBlue,
	kGreen,
	kTintCount
};
inline constexpr const char* kTintNames[ kTintCount ] = { "White", "Ivory", "Canary", "Pink", "Blue", "Green" };
/// The stocks, display-referred RGB.
inline constexpr float kTintRGB[ kTintCount ][ 3 ] = {
	{ 0.955f, 0.955f, 0.945f },
	{ 0.965f, 0.935f, 0.860f },
	{ 0.975f, 0.925f, 0.620f },
	{ 0.975f, 0.840f, 0.875f },
	{ 0.800f, 0.890f, 0.965f },
	{ 0.820f, 0.945f, 0.830f },
};

/// The dye's optical density per channel, relative to kMaxOD: fresh leuco
/// black is a slightly blue black; aged it browns (loses its red and green
/// absorption faster than its blue).
inline constexpr float kInkFresh[ 3 ] = { 1.00f, 0.97f, 0.90f };
inline constexpr float kInkAged[ 3 ]  = { 0.62f, 0.72f, 0.95f };
/// What an aged stock multiplies the paper by.
inline constexpr float kAgedStock[ 3 ] = { 1.00f, 0.93f, 0.76f };

/// Age: the largest fraction of the print that light fades away, the
/// fade's length along the receipt from the tear edge (rows), and the
/// background density the holder's warmth develops at the tear edge, over
/// its own length.
constexpr double kAgeFade        = 0.75;
constexpr double kAgeFadeRows    = 40.0 * kDotsPerMm;
constexpr double kAgeHeldOD      = 0.18;
constexpr double kAgeHeldRows    = 15.0 * kDotsPerMm;

//---------------------------------------------------------------------------
// Where the dots land on the output. The head's dots are square, a whole
// number of output pixels a side, and the head spans as much of the output's
// width as a whole pitch allows.
//---------------------------------------------------------------------------

/// The dot pitch in output pixels: max( 1, floor( W / N ) ).
inline int Pitch( int outW, int dots )
{
	const int p = outW / dots;
	return p < 1 ? 1 : p;
}

/// The output column of dot 0: the strip centred, floor( ( W - N p ) / 2 ).
/// Negative when the head is wider than the output (the ends are cut off).
inline int StripX0( int outW, int dots, int pitch )
{
	const int spare = outW - dots * pitch;
	return spare >= 0 ? spare / 2 : -( ( -spare + 1 ) / 2 );
}

/// The window: how many dot rows the output shows, ceil( H / p ), row 0 at
/// the top.
inline int WindowRows( int outH, int pitch )
{
	return ( outH + pitch - 1 ) / pitch;
}

//---------------------------------------------------------------------------
// The hash.
//---------------------------------------------------------------------------

/// The fleet's integer hash: a PCG output permutation.
inline uint32_t HashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// A slip event at paper row `row` (a receipt row in Static, the absolute
/// row count in Printing) at per-row probability `rate`.
struct Slip
{
	int kind = kNoSlip;
	int rows = 0;
};

inline Slip SlipAt( int64_t row, double rate )
{
	Slip s;
	if( rate <= 0.0 )
		return s;
	const uint32_t key = static_cast< uint32_t >( row ) ^ static_cast< uint32_t >( static_cast< uint64_t >( row ) >> 32 ) * 0x9E3779B9u;
	const uint32_t h   = HashInt( key ^ 0x51F15EEDu );
	const double u     = ( h >> 8 ) * ( 1.0 / 16777216.0 );
	if( u >= rate )
		return s;
	const uint32_t g = HashInt( h );
	s.kind           = ( g & 1u ) ? kStall : kRepeat;
	s.rows           = 1 + static_cast< int >( ( g >> 1 ) % kMaxSlipRows );
	return s;
}

} // namespace receiptfx::model
