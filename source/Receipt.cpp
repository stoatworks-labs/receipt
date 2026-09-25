#include "Receipt.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace receiptfx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Receipt >,// Create method
	"RC01",                  // Plugin unique ID of maximum length 4.
	"SW Receipt",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"A thermal receipt printer.\n\nA line of heaters and no ink: the paper darkens where it gets hot enough. Tone is a dither, a heater that fired on the last row is still warm so lines thicken and dark areas block up, the supply can only fire so many dots at once so dense rows print paler and band by strobe block, a slipping roller repeats rows, and an old receipt fades and yellows.\n\nStatic prints the frame as a receipt; Printing scrolls it out of the slot and tears it off.",// Plugin description
	"Receipt FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double wallSeconds()
{
	using namespace std::chrono;
	return duration< double >( steady_clock::now().time_since_epoch() ).count();
}

double msSince( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
}

int64_t floorDiv( int64_t a, int64_t b )
{
	const int64_t q = a / b;
	return ( a % b != 0 && ( ( a < 0 ) != ( b < 0 ) ) ) ? q - 1 : q;
}

int64_t floorMod( int64_t a, int64_t b )
{
	return a - b * floorDiv( a, b );
}

/// A plain R32F texture of our own, Nearest, clamped. Raw binds, cleared
/// by hand: nothing scoped.
GLuint makeFloatTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

/// Client-memory pixel transfers, whatever the host left bound. A host
/// that leaves a pixel buffer object bound turns glTexSubImage2D's pointer
/// into an offset into it and glReadPixels into a write to it; a host that
/// left a row length set tears every row. Set for the call, put back after.
struct ClientTransfer
{
	GLint unpackBuffer = 0, packBuffer = 0;
	GLint unpackRow = 0, packRow = 0, unpackAlign = 4, packAlign = 4;
	ClientTransfer()
	{
		glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer );
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer );
		glGetIntegerv( GL_UNPACK_ROW_LENGTH, &unpackRow );
		glGetIntegerv( GL_PACK_ROW_LENGTH, &packRow );
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &unpackAlign );
		glGetIntegerv( GL_PACK_ALIGNMENT, &packAlign );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
		glPixelStorei( GL_PACK_ROW_LENGTH, 0 );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	}
	~ClientTransfer()
	{
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER, static_cast< GLuint >( unpackBuffer ) );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, static_cast< GLuint >( packBuffer ) );
		glPixelStorei( GL_UNPACK_ROW_LENGTH, unpackRow );
		glPixelStorei( GL_PACK_ROW_LENGTH, packRow );
		glPixelStorei( GL_UNPACK_ALIGNMENT, unpackAlign );
		glPixelStorei( GL_PACK_ALIGNMENT, packAlign );
	}
};
} // namespace

//---------------------------------------------------------------------------
Receipt::Receipt()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Printing scrolls in real elapsed time; Static ignores the clock.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. Filled BEFORE any declaration: SetParamInfof reads its
	// default out of GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_WIDTH ]         = static_cast< float >( model::k80mm );
	params[ PT_DITHER ]        = static_cast< float >( model::kFloydSteinberg );
	params[ PT_DENSITY ]       = 0.5f; //e0 = 1: a lone dot just saturates
	params[ PT_HEAT_CARRY ]    = 0.4f; //c = 0.36
	params[ PT_HISTORY ]       = 0.5f;
	params[ PT_STROBE_BLOCKS ] = 2.0f; //288 dots a block on 80 mm: a solid row sags to a third, so dark footage prints a banded grey, not a black slab
	params[ PT_PRINT_SPEED ]   = 0.4f; //about 42 mm/s
	params[ PT_SLIP ]          = 0.15f;
	params[ PT_MODE ]          = static_cast< float >( model::kStatic );
	params[ PT_TEAR_LENGTH ]   = 0.4f; //about 121 mm
	params[ PT_AGE ]           = 0.15f;
	params[ PT_PAPER_TINT ]    = static_cast< float >( model::kWhite );
	params[ PT_FIT ]           = static_cast< float >( model::kLetterbox );
	params[ PT_MIX ]           = 1.0f;

	SetOptionParamInfo( PT_WIDTH, "Width", model::kWidthCount, params[ PT_WIDTH ] );
	for( int i = 0; i < model::kWidthCount; ++i )
		SetParamElementInfo( PT_WIDTH, static_cast< unsigned int >( i ), model::kWidthNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_DITHER, "Dither", model::kDitherCount, params[ PT_DITHER ] );
	for( int i = 0; i < model::kDitherCount; ++i )
		SetParamElementInfo( PT_DITHER, static_cast< unsigned int >( i ), model::kDitherNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_DENSITY, "Density", FF_TYPE_STANDARD );
	SetParamInfof( PT_HEAT_CARRY, "Heat Carry", FF_TYPE_STANDARD );
	SetParamInfof( PT_HISTORY, "History Control", FF_TYPE_STANDARD );
	//A real integer with a real range: FF_TYPE_INTEGER is exempt from the
	//0..1 clamp of a STANDARD default.
	SetParamInfo( PT_STROBE_BLOCKS, "Strobe Blocks", FF_TYPE_INTEGER, params[ PT_STROBE_BLOCKS ] );
	SetParamRange( PT_STROBE_BLOCKS, static_cast< float >( model::kStrobeBlocksMin ), static_cast< float >( model::kStrobeBlocksMax ) );

	SetParamInfof( PT_PRINT_SPEED, "Print Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_SLIP, "Slip", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_MODE, "Mode", model::kModeCount, params[ PT_MODE ] );
	for( int i = 0; i < model::kModeCount; ++i )
		SetParamElementInfo( PT_MODE, static_cast< unsigned int >( i ), model::kModeNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_TEAR_LENGTH, "Tear Length", FF_TYPE_STANDARD );

	SetParamInfof( PT_AGE, "Age", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_PAPER_TINT, "Paper Tint", model::kTintCount, params[ PT_PAPER_TINT ] );
	for( int i = 0; i < model::kTintCount; ++i )
		SetParamElementInfo( PT_PAPER_TINT, static_cast< unsigned int >( i ), model::kTintNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_FIT, "Fit", model::kFitCount, params[ PT_FIT ] );
	for( int i = 0; i < model::kFitCount; ++i )
		SetParamElementInfo( PT_FIT, static_cast< unsigned int >( i ), model::kFitNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_WIDTH; i <= PT_STROBE_BLOCKS; ++i )
		SetParamGroup( i, "Printer" );
	for( FFUInt32 i = PT_PRINT_SPEED; i <= PT_TEAR_LENGTH; ++i )
		SetParamGroup( i, "Feed" );
	for( FFUInt32 i = PT_AGE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Paper" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Receipt effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Receipt::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &sampleShader, shaders::kSample, "sample" },
		{ &displayShader, shaders::kDisplay, "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Receipt: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Receipt::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's (by way of filament), unchanged: the ratio of
//the host's clock delta to a steady clock's names the unit outright, and
//nothing plausible sits between 1 and 1000.
double Receipt::nowSeconds()
{
	constexpr int kClockVotes = 8;
	const double wallNow      = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
const float* Receipt::dataTones( int64_t dataRow )
{
	const int N = printer.Current().dots;
	if( imageRows <= 0 )
		return whiteRow.data();
	int64_t row = dataRow - imageOffset;
	if( fit == model::kTile )
		row = floorMod( row, imageRows );
	if( row < 0 || row >= imageRows )
		return whiteRow.data();
	return imageTones.data() + static_cast< size_t >( row ) * N;
}

void Receipt::printPaperRow( int64_t slipKey, Feed& feed, float* density )
{
	const int N = printer.Current().dots;

	//A repeat in progress: the line counter is stuck, the paper moves on.
	if( feed.repeatLeft > 0 )
	{
		printer.Strobe( feed.lastBits.data(), density );
		--feed.repeatLeft;
		return;
	}

	model::Slip slip;
	if( forcedSlip )
	{
		if( slipKey == forcedSlipRow )
		{
			slip.kind = forcedSlipKind;
			slip.rows = forcedSlipRows;
		}
	}
	else
		slip = model::SlipAt( slipKey, slipRate );
	if( ( perturb & model::kPerturbSlipShort ) && slip.kind != model::kNoSlip )
		--slip.rows;//Perturb 8: one row fewer than the slip says (a negative control)

	printer.Dither( dataTones( feed.dataRow ), feed.dataRow, bits.data() );
	++feed.dataRow;
	printer.Strobe( bits.data(), density );

	if( slip.kind == model::kRepeat && slip.rows > 0 )
	{
		feed.lastBits.assign( bits.begin(), bits.begin() + N );
		feed.repeatLeft = slip.rows;
	}
	else if( slip.kind == model::kStall )
	{
		//The paper sits still while the data moves on: every row is struck
		//onto this one, and the dye keeps the darkest.
		for( int k = 0; k < slip.rows; ++k )
		{
			printer.Dither( dataTones( feed.dataRow ), feed.dataRow, extraBits.data() );
			++feed.dataRow;
			printer.Strobe( extraBits.data(), strike.data() );
			for( int x = 0; x < N; ++x )
				density[ x ] = std::max( density[ x ], strike[ static_cast< size_t >( x ) ] );
		}
	}
}

void Receipt::resetPrinting()
{
	receiptStart = printed;
	printingFeed = Feed();
	printer.ResetDither();
}

//---------------------------------------------------------------------------
FFResult Receipt::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int srcW = static_cast< int >( picture.Width );
	const int srcH = static_cast< int >( picture.Height );
	const int outW = hostViewport[ 2 ] > 0 ? hostViewport[ 2 ] : srcW;
	const int outH = hostViewport[ 3 ] > 0 ? hostViewport[ 3 ] : srcH;

	//---------------------------------------------------------------------
	// The clock: the frame's seconds, reduced here in double. Resolume's
	// clock is hundreds of millions of milliseconds, where a float resolves
	// tens of milliseconds.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = 0.0;//the first frame primes the clock and prints nothing
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, model::kMaxFrameDelta );
	lastNow = now;
	if( perturb & model::kPerturbClockPerFrame )
		dt = 1.0 / 60.0;//Perturb 32: a frame is a 60th whatever the clock says (a negative control)

	//---------------------------------------------------------------------
	// The settings, in physical units.
	//---------------------------------------------------------------------
	const int widthOption = controls::OptionIndex( params[ PT_WIDTH ], model::kWidthCount );
	const int N           = model::kWidthDots[ widthOption ];
	Printer::Settings head;
	head.dots    = N;
	head.blocks  = controls::StrobeBlocks( params[ PT_STROBE_BLOCKS ] );
	head.dither  = controls::OptionIndex( params[ PT_DITHER ], model::kDitherCount );
	head.energy  = controls::Energy( params[ PT_DENSITY ] );
	head.carry   = controls::Carry( params[ PT_HEAT_CARRY ] );
	head.history = controls::History( params[ PT_HISTORY ] );
	head.perturb = perturb;
	printer.Configure( head );

	const double rowsPerSecond = controls::RowsPerSecond( params[ PT_PRINT_SPEED ] );
	slipRate                   = controls::SlipRate( params[ PT_SLIP ] );
	const int mode             = controls::OptionIndex( params[ PT_MODE ], model::kModeCount );
	const int tearRows         = controls::TearRows( params[ PT_TEAR_LENGTH ] );
	const double age           = controls::Age( params[ PT_AGE ] );
	const int tint             = controls::OptionIndex( params[ PT_PAPER_TINT ], model::kTintCount );
	fit                        = controls::OptionIndex( params[ PT_FIT ], model::kFitCount );

	//The head's geometry on the output.
	const int pitch      = model::Pitch( outW, N );
	const int x0         = model::StripX0( outW, N, pitch );
	const int windowRows = std::min( model::WindowRows( outH, pitch ), model::kPaperRows );

	//The clip on the paper: U is the source length a dot spans, in 1/N of a
	//source pixel.
	const bool rotate = fit == model::kRotate;
	const int unit    = rotate ? srcH : srcW;
	const int along   = rotate ? srcW : srcH;
	imageRows         = static_cast< int >( std::min< int64_t >( 8192, ( static_cast< int64_t >( along ) * N + unit - 1 ) / unit ) );
	//Where the image sits on the receipt: centred in Static (so on the
	//window), a lead margin below the torn edge in Printing.
	imageOffset = mode == model::kPrinting ? model::kLeadRows : static_cast< int >( floorDiv( static_cast< int64_t >( tearRows ) - imageRows, 2 ) );

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	if( !image.Ensure( N, imageRows, GL_R32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the head's image: " + std::to_string( N ) + " x " + std::to_string( imageRows ) );
		return FF_FAIL;
	}
	const size_t n = static_cast< size_t >( N );
	whiteRow.assign( n, 1.0f );
	bits.resize( n );
	extraBits.resize( n );
	strike.resize( n );
	imageTones.resize( n * static_cast< size_t >( imageRows ) );

	const bool outputResized = lastOutW != 0 && ( lastOutW != outW || lastOutH != outH );
	lastOutW = outW;
	lastOutH = outH;

	if( mode == model::kPrinting )
	{
		if( ringTexture == 0 || ringDots != N )
		{
			//A new head: new paper. (A change of OUTPUT size is not one: the
			//paper is held at head resolution and survives it.)
			if( ringTexture != 0 )
				glDeleteTextures( 1, &ringTexture );
			ring.assign( n * model::kPaperRows, 0.0f );
			ringTexture  = makeFloatTexture( N, model::kPaperRows, ring.data() );
			ringDots     = N;
			uploadedTo   = printed;
			wasPrinting  = false;
		}
		if( outputResized && ( perturb & model::kPerturbResizeClears ) )
		{
			//Perturb 64: a resize loses the paper (photofinish's bug, as a
			//negative control).
			std::fill( ring.begin(), ring.end(), 0.0f );
			resetPrinting();
			ClientTransfer transfer;
			glBindTexture( GL_TEXTURE_2D, ringTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, N, model::kPaperRows, GL_RED, GL_FLOAT, ring.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );
			uploadedTo = printed;
		}
	}
	else if( staticTexture == 0 || staticRows != windowRows || staticDots != N )
	{
		if( staticTexture != 0 )
			glDeleteTextures( 1, &staticTexture );
		window.assign( n * static_cast< size_t >( windowRows ), 0.0f );
		staticTexture = makeFloatTexture( N, windowRows, window.data() );
		staticRows    = windowRows;
		staticDots    = N;
	}

	//---------------------------------------------------------------------
	// 1. sample: the host's picture -> the head's image; read it back.
	//---------------------------------------------------------------------
	const auto sampleStart = std::chrono::steady_clock::now();
	{
		glBindFramebuffer( GL_FRAMEBUFFER, image.GetGLID() );
		image.ResizeViewPort();
		ScopedShaderBinding shader( sampleShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, picture.Handle );
		sampleShader.Set( "InputTexture", 0 );
		sampleShader.Set( "SrcW", srcW );
		sampleShader.Set( "SrcH", srcH );
		sampleShader.Set( "Dots", N );
		sampleShader.Set( "Unit", unit );
		sampleShader.Set( "Rotate", rotate ? 1 : 0 );
		quad.Draw();
		glBindTexture( GL_TEXTURE_2D, 0 );

		ClientTransfer transfer;
		glReadPixels( 0, 0, N, imageRows, GL_RED, GL_FLOAT, imageTones.data() );
	}
	stats.sampleReadMs = msSince( sampleStart );

	//---------------------------------------------------------------------
	// 2. the print, on the CPU.
	//---------------------------------------------------------------------
	const auto engineStart = std::chrono::steady_clock::now();
	int receiptTop = 0, receiptBottom = windowRows, rowBase = 0, texRows = windowRows;
	bool notchBottom = true;
	int rowsThisFrame = 0;
	if( mode == model::kPrinting )
	{
		if( !wasPrinting )
			resetPrinting();//a new receipt out of the slot
		wasPrinting = true;

		pendingRows += rowsPerSecond * dt;
		const double whole = std::floor( pendingRows );
		pendingRows -= whole;
		rowsThisFrame = static_cast< int >( std::min( whole, static_cast< double >( model::kPaperRows / 2 ) ) );

		for( int k = 0; k < rowsThisFrame; ++k )
		{
			if( printed - receiptStart >= tearRows )
				resetPrinting();//torn off at the length: the next receipt starts here
			printPaperRow( printed, printingFeed, ring.data() + static_cast< size_t >( floorMod( printed, model::kPaperRows ) ) * n );
			++printed;
		}

		//The slot is the bottom row; row w shows paper row printed - R_w + w.
		const int64_t firstShown = printed - windowRows;
		const int64_t oldestHeld = std::max< int64_t >( { receiptStart, printed - model::kPaperRows, 0 } );
		//A receipt running off the top of the window keeps its notched end
		//out of sight: the notch is at most two rows deep.
		receiptTop    = static_cast< int >( std::max< int64_t >( oldestHeld - firstShown, -8 ) );
		receiptBottom = windowRows;
		notchBottom   = false;
		rowBase       = static_cast< int >( floorMod( firstShown, model::kPaperRows ) );
		texRows       = model::kPaperRows;
	}
	else
	{
		wasPrinting = false;

		//The receipt, centred on the window, printed afresh from its first
		//visible row.
		receiptTop    = static_cast< int >( floorDiv( static_cast< int64_t >( windowRows ) - tearRows, 2 ) );
		receiptBottom = receiptTop + tearRows;
		printer.ResetHeat();
		printer.ResetDither();
		Feed feed;
		const int first = std::max( 0, receiptTop );
		const int last  = std::min( windowRows, receiptBottom );
		feed.dataRow    = first - receiptTop;
		for( int w = first; w < last; ++w )
			printPaperRow( w - receiptTop, feed, window.data() + static_cast< size_t >( w ) * n );
		rowsThisFrame = std::max( 0, last - first );
	}
	stats.engineMs = msSince( engineStart );

	//---------------------------------------------------------------------
	// 3. the printed rows to the GPU.
	//---------------------------------------------------------------------
	const auto uploadStart = std::chrono::steady_clock::now();
	{
		ClientTransfer transfer;
		if( mode == model::kPrinting )
		{
			glBindTexture( GL_TEXTURE_2D, ringTexture );
			int64_t from = std::max( uploadedTo, printed - model::kPaperRows );
			while( from < printed )
			{
				const int row   = static_cast< int >( floorMod( from, model::kPaperRows ) );
				const int count = static_cast< int >( std::min< int64_t >( printed - from, model::kPaperRows - row ) );
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, row, N, count, GL_RED, GL_FLOAT, ring.data() + static_cast< size_t >( row ) * n );
				from += count;
			}
			uploadedTo = printed;
		}
		else
		{
			glBindTexture( GL_TEXTURE_2D, staticTexture );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, N, windowRows, GL_RED, GL_FLOAT, window.data() );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	stats.uploadMs = msSince( uploadStart );

	//---------------------------------------------------------------------
	// 4. display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, picture.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, mode == model::kPrinting ? ringTexture : staticTexture );
		glActiveTexture( GL_TEXTURE0 );

		float paper[ 3 ], ink[ 3 ];
		for( int ch = 0; ch < 3; ++ch )
		{
			paper[ ch ] = static_cast< float >( model::kTintRGB[ tint ][ ch ] * ( 1.0 + age * ( model::kAgedStock[ ch ] - 1.0 ) ) );
			ink[ ch ]   = static_cast< float >( model::kMaxOD * ( model::kInkFresh[ ch ] + age * ( model::kInkAged[ ch ] - model::kInkFresh[ ch ] ) ) );
		}

		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "PaperTexture", 1 );
		glUniform2i( displayShader.FindUniform( "Origin" ), hostViewport[ 0 ], hostViewport[ 1 ] );
		displayShader.Set( "OutH", outH );
		displayShader.Set( "Dots", N );
		displayShader.Set( "Pitch", pitch );
		displayShader.Set( "X0", x0 );
		displayShader.Set( "FloatPitch", ( perturb & model::kPerturbFloatPitch ) ? 1 : 0 );
		displayShader.Set( "PitchF", static_cast< float >( static_cast< double >( outW ) / N ) );
		displayShader.Set( "TexRows", texRows );
		displayShader.Set( "RowBase", rowBase );
		displayShader.Set( "ReceiptTop", receiptTop );
		displayShader.Set( "ReceiptBottom", receiptBottom );
		displayShader.Set( "NotchTop", 1 );
		displayShader.Set( "NotchBottom", notchBottom ? 1 : 0 );
		displayShader.Set( "Paper", paper[ 0 ], paper[ 1 ], paper[ 2 ] );
		displayShader.Set( "Ink", ink[ 0 ], ink[ 1 ], ink[ 2 ] );
		displayShader.Set( "FadeAmount", static_cast< float >( model::kAgeFade * age ) );
		displayShader.Set( "FadeRows", static_cast< float >( model::kAgeFadeRows ) );
		displayShader.Set( "HeldOD", static_cast< float >( model::kAgeHeldOD * age ) );
		displayShader.Set( "HeldRows", static_cast< float >( model::kAgeHeldRows ) );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	stats.printed       = printed;
	stats.receiptStart  = receiptStart;
	stats.rowsThisFrame = rowsThisFrame;
	stats.dots          = N;
	stats.pitch         = pitch;
	stats.x0            = x0;
	stats.windowRows    = windowRows;
	stats.imageRows     = imageRows;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Receipt::DeInitGL()
{
	sampleShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();
	image.Destroy();
	if( staticTexture != 0 )
		glDeleteTextures( 1, &staticTexture );
	if( ringTexture != 0 )
		glDeleteTextures( 1, &ringTexture );
	staticTexture = ringTexture = 0;
	staticRows = staticDots = ringDots = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Receipt::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Receipt::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Receipt::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Receipt::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
