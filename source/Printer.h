#pragma once

#include <cstdint>
#include <vector>

namespace receiptfx
{

/**
	The print engine: the firmware and the head, one strobe at a time, on
	the CPU in double. See Model.h for the model it runs.

	**Why the CPU.** Floyd-Steinberg and Atkinson are serial in both
	directions: a dot's bit depends on the error of the dot before it in the
	row and of three (Atkinson: four) dots in the rows above, and the heat
	model is a recurrence down every column. GLSL 4.10 has no image stores,
	so a fragment can only write itself; the GPU forms are a wavefront of
	N + 2R draws (1,300 a frame for a 576 x 360 window) or slope's chunked
	re-run, which is exact only along ONE axis. At head resolution the whole
	window is 576 x 360 dots, a fifth of a megapixel whatever the output's
	size, and the CPU runs it in about a millisecond. AGENTS.md has the
	measurements.

	Nothing here knows about GL, time or the paper's position: the caller
	(Receipt.cpp) owns the feed and says which data row comes next.
*/
class Printer
{
public:
	struct Settings
	{
		int dots        = 576;
		int blocks      = 4;  ///< strobe blocks across the head
		int dither      = 1;  ///< model::Dither
		double energy   = 1.0;///< e0, the nominal strobe energy
		double carry    = 0.0;///< c, the heat left at the next row
		double history  = 0.0;///< h, the firmware's history control
		int perturb     = 0;  ///< model::Perturb bits
	};

	/// Adopt new settings. A different head width starts a new head: no
	/// heat, no error.
	void Configure( const Settings& s );

	/// A cold head.
	void ResetHeat();

	/// A new job: the dither's carried error is dropped.
	void ResetDither();

	/// One data row's tones (0 black .. 1 white, `dots` of them) to fire
	/// bits. `dataRow` is the row's index in its job (Bayer's row phase).
	/// Rows must be dithered in order: the error carries to the next.
	void Dither( const float* tones, int64_t dataRow, uint8_t* bits );

	/// One strobe of `bits`: history control, the supply's budget, the heat,
	/// and the paper's response. Writes each dot's density (0..1) and moves
	/// the heaters on by one row.
	void Strobe( const uint8_t* bits, float* density );

	/// Test hook: a heater's heat now.
	double HeatForTest( int x ) const
	{
		return x >= 0 && x < settings.dots ? theta[ static_cast< size_t >( x ) ] : 0.0;
	}

	const Settings& Current() const
	{
		return settings;
	}

private:
	Settings settings;

	std::vector< double > theta; ///< each heater's heat
	std::vector< double > energy;///< this strobe's energy per heater

	/// The dither's carried error: three rows (this, the next, the one after),
	/// each padded by two either side so the ends need no test. What lands in
	/// the padding is lost off the head's ends, as it is in the firmware.
	static constexpr int kPad = 2;
	std::vector< double > error[ 3 ];
	int errorRow = 0;
};

} // namespace receiptfx
