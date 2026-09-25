#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Strobe Blocks is a real FF_TYPE_INTEGER with a real range.
	Every conversion to a physical unit lives here and nowhere else; rctest
	states the same laws from their definitions and --laws holds the two
	together.
*/
namespace receiptfx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Strobe Blocks: the integer, clamped to 1..8.
int StrobeBlocks( float value );

/// Density: the nominal strobe energy e0, 0.5 + v -- half the saturation
/// energy at 0, exactly the saturation energy at the default 0.5, one and a
/// half times it at 1.
double Energy( float value );

/// Heat Carry: the fraction of a heater's heat left at the next row, 0.9 v.
double Carry( float value );

/// History Control: how much of the predicted residual the firmware takes
/// off a firing heater's strobe, v.
double History( float value );

/// Print Speed: mm/s, 2 + 248 v^2 -- 2 at 0, about 42 at the default 0.4,
/// 250 (a fast receipt printer) at 1.
double SpeedMmPerSecond( float value );

/// The same, in dot rows a second: 8 dots/mm.
double RowsPerSecond( float value );

/// Slip: the per-row probability of a slip event, 0.02 v^2 -- none at 0,
/// one row in fifty at 1.
double SlipRate( float value );

/// Tear Length: mm, 30 + 570 v^2 -- 30 at 0, about 121 at the default 0.4,
/// 600 at 1.
double TearMm( float value );

/// The same in dot rows, rounded: never fewer than 1.
int TearRows( float value );

/// Age: v.
double Age( float value );

} // namespace receiptfx::controls
