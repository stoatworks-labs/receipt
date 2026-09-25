#pragma once

#include "PassBuffer.h"
#include "Printer.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Receipt -- a thermal receipt printer, as an FFGL effect.

	**The one idea.** A thermal printer has a fixed line of tiny heaters and
	no ink: the paper darkens where it gets hot enough. The head prints one
	dot row at a time as the stepper pulls the paper, so the physics of HEAT
	is the whole look -- a dot is on or off, so tone is a dither; a heater
	that fired on the last row is still warm, so vertical lines thicken and
	dark areas block up unless the firmware's history control takes it off;
	the supply can only fire so many dots at once, so dense rows print paler
	and band by strobe block; a slipping roller repeats or skips rows; and an
	old receipt fades and yellows, darkest at the tear edge where it was held.

	Two modes. Static prints the frame as a receipt, afresh every frame.
	Printing scrolls the receipt out of the slot in real elapsed time, each
	new row printed from the frame showing when the head reaches it, and
	tears it off at Tear Length. The printed paper lives at head resolution
	(N dots across, a ring of rows), so a change of output size keeps it.
	See AGENTS.md.
*/
class Receipt : public CFFGLPlugin
{
public:
	Receipt();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by rctest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The harness DECLARES its clock's unit (1 = seconds) rather than
	/// leaving the plugin to vote on it against the wall clock.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}

	/// Replace the seeded slips with one: at paper row `row` (a receipt row
	/// in Static, the absolute row count in Printing), `kind` for `rows`.
	/// kind 0 clears the override and returns to the seeded slips.
	void ForceSlipForTest( int64_t row, int kind, int rows )
	{
		forcedSlip     = kind != 0;
		forcedSlipRow  = row;
		forcedSlipKind = kind;
		forcedSlipRows = rows;
	}

	/// What the last frame cost, by stage, and what Printing has printed.
	struct Stats
	{
		double sampleReadMs = 0.0;///< the sample pass and the read-back (the read waits for the pass)
		double engineMs     = 0.0;///< dither, heat, budget, paper: the CPU
		double uploadMs     = 0.0;///< the printed rows to the paper texture
		int64_t printed     = 0;  ///< Printing: paper rows printed since the plugin started
		int64_t receiptStart = 0; ///< Printing: the paper row the current receipt began at
		int rowsThisFrame   = 0;
		int dots = 0, pitch = 0, x0 = 0, windowRows = 0, imageRows = 0;
	};
	const Stats& StatsForTest() const
	{
		return stats;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Printer
		PT_WIDTH,
		PT_DITHER,
		PT_DENSITY,
		PT_HEAT_CARRY,
		PT_HISTORY,
		PT_STROBE_BLOCKS,

		//Feed
		PT_PRINT_SPEED,
		PT_SLIP,
		PT_MODE,
		PT_TEAR_LENGTH,

		//Paper
		PT_AGE,
		PT_PAPER_TINT,
		PT_FIT,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	/// What a data row's tones are, and which image row a receipt's data
	/// row prints, for the frame being rendered.
	const float* dataTones( int64_t dataRow );

	/// One paper row through the feed: the slip, the dither, the strobe(s).
	struct Feed
	{
		int64_t dataRow = 0;
		int repeatLeft  = 0;
		std::vector< uint8_t > lastBits;
	};
	void printPaperRow( int64_t slipKey, Feed& feed, float* density );

	void resetPrinting();

	ffglex::FFGLShader sampleShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	receiptfx::PassBuffer image;///< the head's image of this frame, N x R_img, R32F tone
	GLuint staticTexture = 0;   ///< Static: this frame's window, N x R_w
	int staticRows       = 0;
	int staticDots       = 0;
	GLuint ringTexture   = 0;   ///< Printing: the paper, N x kPaperRows, a ring
	int ringDots         = 0;

	receiptfx::Printer printer;

	//--- this frame --------------------------------------------------------
	std::vector< float > imageTones;///< the read-back, N x R_img
	std::vector< float > whiteRow;
	std::vector< uint8_t > bits, extraBits;
	std::vector< float > strike;
	int imageRows   = 0;
	int imageOffset = 0;///< where the image starts on the receipt
	int fit         = 0;

	//--- the paper, for Printing ---------------------------------------------
	std::vector< float > ring;///< N x kPaperRows densities
	int64_t printed      = 0;
	int64_t receiptStart = 0;
	int64_t uploadedTo   = 0;
	double pendingRows   = 0.0;
	Feed printingFeed;
	bool wasPrinting = false;
	int lastOutW = 0, lastOutH = 0;

	std::vector< float > window;///< Static: N x R_w densities

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;

	int perturb = 0;
	bool forcedSlip        = false;
	int64_t forcedSlipRow  = 0;
	int forcedSlipKind     = 0;
	int forcedSlipRows     = 0;
	double slipRate        = 0.0;

	Stats stats;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
