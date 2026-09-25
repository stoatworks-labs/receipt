/**
	rctest -- render Receipt offline, and read the printer back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock and measures the answer out of the picture it
	made:

		rctest --out /tmp/frame.png     a picture, on the moving test card
		rctest --list                   every parameter, its kind and default
		rctest --dither                 the mean dot density of each dither on
		                                flat greys is the tone, within that
		                                dither's quantisation bound
		rctest --history                a vertical line thickens, and runs on
		                                below its end, by what the heat-carry
		                                model predicts -- in dots -- with History
		                                Control off, and does not thicken with it
		                                on
		rctest --budget                 a block over the supply's budget prints
		                                at the predicted lower density, and the
		                                banding sits on the block boundaries
		rctest --slip                   a forced slip repeats (or overprints)
		                                exactly the stated rows, and no others
		rctest --grid                   every dot lands on its whole-pixel cell
		                                of the 8 dots/mm grid scaled to the output
		rctest --printing               Printing scrolls in real elapsed time,
		                                whatever the frame rate, and tears at the
		                                length
		rctest --resize                 a change of output size mid-print keeps
		                                every printed row
		rctest --alpha                  the paper is opaque, off the paper is
		                                transparent, Mix fades both; a clear
		                                source prints blank paper
		rctest --negative               every check above that measures the
		                                model can FAIL
		rctest --laws --names           the checks that need no GL
		rctest --bench                  the render cost, by stage
		rctest --dump-shaders DIR       the exact GLSL the plugin compiles
		rctest --pipe                   raw frames in, raw frames out

	The control laws, the head's geometry, the paper's response, the heat
	recurrence's closed forms, the budget and the clock are stated HERE, from
	their definitions (Controls.h's comments and Model.h's description), and
	never read out of the plugin: a constant typed wrong there has to show up
	as a failed check, not as an agreement. AGENTS.md has one line per check
	on where each tolerance comes from.

	RCTEST_RENDERER=software asks CGL for Apple's software renderer, which is
	what a GPU-less CI runner gets.
*/

#include "Controls.h"
#include "Model.h"
#include "Receipt.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = receiptfx::model;

int g_checks   = 0;
int g_failures = 0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The model, stated from its definitions (Model.h's description and
// Controls.h's comments). Nothing here calls into the plugin.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}

double statedEnergy( float v ) { return 0.5 + unit( v ); }
double statedCarry( float v ) { return 0.9 * unit( v ); }
double statedSpeed( float v ) { return 2.0 + 248.0 * unit( v ) * unit( v ); }
double statedRowsPerSecond( float v ) { return 8.0 * statedSpeed( v ); }
double statedSlipRate( float v ) { return 0.02 * unit( v ) * unit( v ); }
double statedTearMm( float v ) { return 30.0 + 570.0 * unit( v ) * unit( v ); }
int statedTearRows( float v ) { return std::max( 1, static_cast< int >( std::lround( 8.0 * statedTearMm( v ) ) ) ); }

constexpr int kDots80       = 576;
constexpr double kBudget    = 96.0;
constexpr double kSpread    = 0.22;
constexpr double kQ0        = 0.35;
constexpr double kQ1        = 1.0;
constexpr double kQHalf     = 0.5 * ( kQ0 + kQ1 );//smoothstep is 1/2 at the midpoint
constexpr double kODMax     = 1.3;
constexpr double kPaperRed  = 0.955;//White stock, red channel, Age 0
constexpr double kInkRed    = 1.0;  //fresh dye, red channel, relative to kODMax

double statedResponse( double q )
{
	const double t = std::clamp( ( q - kQ0 ) / ( kQ1 - kQ0 ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

/// The recursive Bayer construction: M_2n = [ 4M + 0, 4M + 2 ; 4M + 3, 4M + 1 ]
/// by blocks.
int statedBayer( int row, int col )
{
	int value = 0;
	for( int bit = 2; bit >= 0; --bit )
	{
		const int r = ( row >> bit ) & 1, c = ( col >> bit ) & 1;
		const int q = r == 0 ? ( c == 0 ? 0 : 2 ) : ( c == 0 ? 3 : 1 );
		value       = value + q * ( 1 << ( 2 * ( 2 - bit ) ) );
	}
	return value;
}

int floorDiv( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}

/// Where everything sits for a Letterbox print on an output of W x H, from
/// Model.h's laws.
struct Layout
{
	int W = 0, H = 0, N = kDots80, p = 1, x0 = 0, Rw = 0, T = 0, top = 0, imageRows = 0, imageOffset = 0;

	Layout( int w, int h, int dots, int tearRows )
		: W( w ), H( h ), N( dots ), T( tearRows )
	{
		p           = std::max( 1, W / N );
		x0          = floorDiv( W - N * p, 2 );
		Rw          = ( H + p - 1 ) / p;
		top         = floorDiv( Rw - T, 2 );
		imageRows   = ( H * N + W - 1 ) / W;
		imageOffset = floorDiv( T - imageRows, 2 );
	}

	/// The window row an image row lands on in Static, with no slips.
	int windowRowOf( int imageRow ) const { return top + imageOffset + imageRow; }
	int imageRowOf( int windowRow ) const { return windowRow - top - imageOffset; }
	/// The data row (the receipt row) a window row prints in Static.
	int dataRowOf( int windowRow ) const { return windowRow - top; }

	/// Dots whose whole cell is inside the output.
	int firstDot() const { return std::max( 0, -floorDiv( x0, p ) + ( x0 % p != 0 && x0 < 0 ? 0 : 0 ) ); }
	bool dotVisible( int c ) const { return c >= 0 && c < N && x0 + c * p >= 0 && x0 + ( c + 1 ) * p <= W; }
	bool rowVisible( int w ) const { return w >= 0 && ( w + 1 ) * p <= H; }

	/// The dots a source-aligned rectangle may start and end on: a dot edge
	/// lands on a source pixel edge where c W / N is an integer.
	int alignStep() const { return N / std::gcd( W, N ); }
};

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level, double alpha = 1.0 )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = static_cast< float >( alpha );
	}
	return p;
}

/// The first source-aligned image row at or after `i`, and not before 0.
int alignedRow( const Layout& L, int i )
{
	const int step = L.alignStep();
	i              = std::max( 0, i );
	return ( ( i + step - 1 ) / step ) * step;
}

/// Paint dot rectangle [ c0, c1 ) x image rows [ i0, i1 ) of a Letterbox
/// print: the source pixels [ c0 W / N, c1 W / N ) x [ i0 W / N, i1 W / N ).
/// Every edge must be source-aligned, so every dot in it is exactly `level`
/// and every dot outside it untouched.
bool paintDots( Picture& pic, const Layout& L, int c0, int c1, int i0, int i1, double level )
{
	for( int e : { c0, c1, i0, i1 } )
		if( ( static_cast< int64_t >( e ) * L.W ) % L.N != 0 )
		{
			std::fprintf( stderr, "rctest: dot edge %d is not source-aligned at %dx%d (step %d)\n", e, L.W, L.H, L.alignStep() );
			return false;
		}
	const int x0 = static_cast< int >( static_cast< int64_t >( c0 ) * L.W / L.N ), x1 = static_cast< int >( static_cast< int64_t >( c1 ) * L.W / L.N );
	const int y0 = static_cast< int >( static_cast< int64_t >( i0 ) * L.W / L.N ), y1 = static_cast< int >( static_cast< int64_t >( i1 ) * L.W / L.N );
	for( int y = std::max( 0, y0 ); y < std::min( L.H, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( L.W, x1 ); ++x )
		{
			float* px = pic.data() + ( static_cast< size_t >( y ) * L.W + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( level );
		}
	return true;
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//RCTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "RCTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "rctest: RCTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Receipt::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Receipt& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Receipt::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Receipt& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Receipt& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Receipt& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// Every control a check can move, as the sliders the plugin sees. The
/// defaults here are the CLEAN printer: 80 mm, Floyd-Steinberg, the nominal
/// energy, no heat carried, no history control, eight strobe blocks (72
/// dots each, under the 96-dot budget), no slips, Static, the longest
/// receipt (so it covers every window), no age, white stock, Letterbox, all
/// wet.
struct Knobs
{
	int width        = 1;
	int dither       = 1;
	float density    = 0.5f;
	float heatCarry  = 0.0f;
	float history    = 0.0f;
	int blocks       = 8;
	float speed      = 0.4f;
	float slip       = 0.0f;
	int mode         = 0;
	float tearLength = 1.0f;
	float age        = 0.0f;
	int tint         = 0;
	int fit          = 0;
	float mix        = 1.0f;
};

void apply( Receipt& p, const Knobs& k )
{
	set( p, "Width", static_cast< float >( k.width ) );
	set( p, "Dither", static_cast< float >( k.dither ) );
	set( p, "Density", k.density );
	set( p, "Heat Carry", k.heatCarry );
	set( p, "History Control", k.history );
	set( p, "Strobe Blocks", static_cast< float >( k.blocks ) );
	set( p, "Print Speed", k.speed );
	set( p, "Slip", k.slip );
	set( p, "Mode", static_cast< float >( k.mode ) );
	set( p, "Tear Length", k.tearLength );
	set( p, "Age", k.age );
	set( p, "Paper Tint", static_cast< float >( k.tint ) );
	set( p, "Fit", static_cast< float >( k.fit ) );
	set( p, "Mix", k.mix );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Receipt plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8, so densities
	/// can be read back as floats and their tolerances float-derived.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps seconds, the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	bool render( long frame, const std::vector< unsigned char >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	bool render( long frame, const Picture& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// Reading dots back. A dot's density comes out of its cell's centre pixel,
// red channel, white stock, Age 0: R = paper x 10^( -kODMax x D ), so
// D = -log2( R / paper ) / ( kODMax log2 10 ).
//
// Tolerance on D, derived: the display computes 10^-OD as exp2( -OD log2 10 )
// (GLSL 4.10 s8.2: exp2 within 3 ulp) and multiplies by the paper (0.5 ulp);
// the read is the float itself. So R is within 4 ulp relative, 4 x 2^-23,
// which through the log is 4 x 2^-23 / ln 10 in OD and that over kODMax in D:
// 1.2e-7. D itself was rounded to float in the R32F paper texture, 2^-24 at
// 1. kDTolerance is 1e-6, a factor of six over the two.
//---------------------------------------------------------------------------
constexpr double kDTolerance = 1e-6;

/// A dot's density, or -1 where the cell is not paper (alpha 0).
double dotDensity( const std::vector< float >& out, const Layout& L, int c, int w )
{
	const int x = L.x0 + c * L.p + L.p / 2;
	const int y = w * L.p + L.p / 2;
	if( x < 0 || x >= L.W || y < 0 || y >= L.H )
		return -2.0;
	if( at( out, L.W, y, x, 3 ) < 0.5f )
		return -1.0;
	const double r = std::max( 1e-30, static_cast< double >( at( out, L.W, y, x, 0 ) ) );
	return -std::log2( r / kPaperRed ) / ( kODMax * kInkRed * std::log2( 10.0 ) );
}

bool isInk( double D )
{
	return D >= 0.5;
}

/// One Static frame of one picture through a fresh session, read back.
bool renderStatic( const Knobs& k, int perturb, int W, int H, const Picture& pic, std::vector< float >& out,
                   std::function< void( Receipt& ) > hook = nullptr )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	if( hook )
		hook( s.plugin );
	if( !s.begin( W, H ) || !s.render( 0, pic ) )
		return false;
	out = s.readBackFloat();
	s.end();
	return true;
}

int tooSmall( const char* what, int W, int H )
{
	std::printf( "   FAIL %s needs a larger raster than %dx%d\n", what, W, H );
	++g_checks;
	++g_failures;
	return 1;
}

//---------------------------------------------------------------------------
// --dither: on flat greys, the fraction of dots that fire is the tone, within
// the dither's quantisation bound. Bounds, derived (AGENTS.md):
//
//   Bayer    over whole 8 x 8 tiles of a flat field, exactly
//            #{ k : ( k + 1/2 ) / 64 < d } / 64, so within 1/128 of d.
//   F-S      error is conserved except what leaves the region: with every
//            error within +-1/2 (by induction, for d in 0..1), a w x h
//            region's sum is off by at most 1/2 ( 22/16 h + 18/16 w + 2 ).
//   Atkinson throws a quarter of every error away, so a region's sum is off
//            by at most A/8 plus the boundary's 1/2 ( 10 h + 10 w ) / 8; and
//            below d = 1/8 the carried value can never reach 1/2 (it is at
//            most d + 3/4 of itself, 4d), so NO dot fires, exactly.
//
// A dot is read as fired where D >= 1/2: a fired dot is at q >= e0 = 1 (D = 1)
// and an unfired one at q <= 2 x kSpread = 0.44 (D <= 0.053), so the reading
// is exact.
//---------------------------------------------------------------------------
int runDither( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== dither at %dx%d: the mean dot density on flat greys is the tone, within each dither's bound\n", W, H );
	Knobs k;
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	int failed = 0;

	//The visible region, in dots and window rows.
	int c0 = 0, c1 = 0, w0 = 0, w1 = 0;
	while( c0 < L.N && !L.dotVisible( c0 ) )
		++c0;
	c1 = c0;
	while( c1 < L.N && L.dotVisible( c1 ) )
		++c1;
	//Rows: visible, and inside the image (a flat grey is only on the image;
	//the receipt's margin above and below it is blank paper).
	w0 = std::max( 0, L.windowRowOf( 0 ) );
	w1 = w0;
	while( L.rowVisible( w1 ) && L.imageRowOf( w1 ) < L.imageRows )
		++w1;
	if( c1 - c0 < 64 || w1 - w0 < 64 )
		return tooSmall( "--dither", W, H );

	const double levels[] = { 0.03, 0.1, 0.2, 0.3, 0.45, 0.5, 0.62, 0.8, 0.9, 0.97 };//tones (1 = white)
	for( int dither = 0; dither < 3; ++dither )
	{
		k.dither      = dither;
		double worst  = 0.0, worstBound = 0.0;
		bool allOk    = true;
		bool atkinsonBlank = true;
		for( double tone : levels )
		{
			std::vector< float > out;
			if( !renderStatic( k, perturb, W, H, flat( W, H, tone ), out ) )
				return failed + report( false, quiet, "render failed" );

			//Bayer: whole tiles only, in the head's column phase and the
			//data row's phase.
			int rc0 = c0, rc1 = c1, rw0 = w0, rw1 = w1;
			if( dither == model::kBayer )
			{
				rc0 = ( c0 + 7 ) / 8 * 8;
				rc1 = rc0 + ( c1 - rc0 ) / 8 * 8;
				while( ( ( L.dataRowOf( rw0 ) % 8 ) + 8 ) % 8 != 0 )
					++rw0;
				rw1 = rw0 + ( w1 - rw0 ) / 8 * 8;
			}
			const int w = rc1 - rc0, h = rw1 - rw0;
			double fired = 0.0;
			for( int row = rw0; row < rw1; ++row )
				for( int c = rc0; c < rc1; ++c )
					fired += isInk( dotDensity( out, L, c, row ) ) ? 1.0 : 0.0;
			const double mean = fired / ( static_cast< double >( w ) * h );
			//The tone the plugin saw: luma of a grey through the float
			//weights, which sum to 1 within 2 ulp.
			const double d = 1.0 - static_cast< double >( static_cast< float >( tone ) );
			double bound   = 0.0, expected = d;
			if( dither == model::kBayer )
			{
				int count = 0;
				for( int m = 0; m < 64; ++m )
					count += ( m + 0.5 ) / 64.0 < d ? 1 : 0;
				expected = count / 64.0;
				bound    = 1e-12;//exact: an integer count over a whole number of tiles
				if( std::fabs( expected - d ) > 1.0 / 128.0 + 1e-12 )
					allOk = false;//the stated law's own promise
			}
			//The sample pass's tone for a flat grey is a float sum of at most
			//a few dozen integer-weighted terms, a few ulp from the grey:
			//1e-6 covers it with room, and it shifts every mean by at most that.
			else if( dither == model::kFloydSteinberg )
				bound = 0.5 * ( 22.0 / 16.0 * h + 18.0 / 16.0 * w + 2.0 ) / ( static_cast< double >( w ) * h ) + 1e-6;
			else
			{
				bound = 0.125 + 0.5 * ( 10.0 * h + 10.0 * w ) / 8.0 / ( static_cast< double >( w ) * h ) + 1e-6;
				if( d < 0.125 || d > 0.875 )
				{
					expected = d < 0.125 ? 0.0 : 1.0;
					bound    = 1e-12;
					atkinsonBlank = atkinsonBlank && std::fabs( mean - expected ) <= bound;
				}
			}
			const double err = std::fabs( mean - expected );
			if( err > bound )
				allOk = false;
			if( err / std::max( bound, 1e-12 ) > worst / std::max( worstBound, 1e-12 ) || worstBound == 0.0 )
			{
				worst      = err;
				worstBound = bound;
			}
			if( !quiet && err > bound )
				std::printf( "        %s at tone %.2f: density %.5f, expected %.5f +- %.2g\n", model::kDitherNames[ dither ], tone, mean, expected, bound );
		}
		failed += report( allOk, quiet, "%-15s 10 greys over %dx%d dots: worst |density - %s| %.2g against a bound of %.2g",
		                  model::kDitherNames[ dither ], c1 - c0, w1 - w0, dither == 0 ? "count/64" : "tone", worst, worstBound );
		if( dither == model::kAtkinson )
			failed += report( atkinsonBlank, quiet, "Atkinson prints nothing below d = 1/8 and everything above 7/8 (tones 0.03, 0.9, 0.97), exactly" );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --history: a 9-dot vertical line printed with Heat Carry c.
//
// With History Control off, every row of the line fires e0 and a line
// heater's heat is theta_j = e0 ( 1 - c^( j + 1 ) ) / ( 1 - c ) on the j-th
// row. The unfired column beside it sees kSpread theta_j, so the line is two
// dots wider (one each side) from the first row where that reaches the
// paper's half-density heat, kQHalf, onward. After the line ends, the heat
// decays as c^k: an interior column sees c^k theta ( 1 + 2 kSpread ), an edge
// column c^k theta ( 1 + kSpread ), the outside column c^k theta kSpread, and
// the line runs on below its end for as many rows as those stay at kQHalf.
//
// With History Control at 1 a firing heater is always exactly e0, so the
// neighbour sees kSpread e0 (0.22, under the threshold): no thickening at
// all; the run-on below the end remains (nothing cools a heater that does
// not fire), on theta = e0.
//
// Measured in dots: the number of dark dots in each row across the line.
// Every predicted density is also held to kDTolerance.
//---------------------------------------------------------------------------
int runHistory( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== history at %dx%d: a vertical line thickens by what the heat carry predicts, and not under history control\n", W, H );
	Knobs k;
	k.heatCarry = 0.9f;
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	const int step = L.alignStep();
	int failed     = 0;

	//A line of `step` dots, source-aligned, in view, and away from a block
	//boundary (the budget is not in play: 9 dots is under 96 anyway).
	const int lineW = step;
	int cLine       = ( ( L.N / 2 ) / step ) * step;
	while( !( L.dotVisible( cLine - 3 ) && L.dotVisible( cLine + lineW + 3 ) ) && cLine > 0 )
		cLine -= step;
	//Rows: start and end source-aligned, the start a third of the way into the
	//visible window, 60 rows long or what fits with 20 rows of run-on after.
	int iStart = alignedRow( L, L.imageRowOf( 0 ) + 12 );
	int lengthRows = ( 60 / step ) * step;
	while( !L.rowVisible( L.windowRowOf( iStart + lengthRows ) + 20 ) && lengthRows > step )
		lengthRows -= step;
	const int iEnd = iStart + lengthRows;
	if( !L.dotVisible( cLine - 3 ) || !L.rowVisible( L.windowRowOf( iStart ) - 2 ) || !L.rowVisible( L.windowRowOf( iEnd ) + 20 ) || lengthRows < 18 )
		return tooSmall( "--history", W, H );

	Picture pic = flat( W, H, 1.0 );
	if( !paintDots( pic, L, cLine, cLine + lineW, iStart, iEnd, 0.0 ) )
		return failed + report( false, quiet, "the line could not be painted source-aligned" );

	const double c  = statedCarry( k.heatCarry );
	const double e0 = statedEnergy( k.density );

	for( float history : { 0.0f, 1.0f } )
	{
		k.history = history;
		std::vector< float > out;
		if( !renderStatic( k, perturb, W, H, pic, out ) )
			return failed + report( false, quiet, "render failed" );

		int worstRow = -1, rowsWrong = 0, onsetPredicted = -1, onsetMeasured = -1;
		int runOnPredicted = 0, runOnMeasured = 0, widest = 0;
		double worstD = 0.0;
		const int wStart = L.windowRowOf( iStart ), wEnd = L.windowRowOf( iEnd );
		double theta = 0.0;//a line heater's heat
		for( int w = wStart - 2; w < wEnd + 20; ++w )
		{
			//The stated heat in the line's heaters on this row.
			const bool firing = w >= wStart && w < wEnd;
			if( firing )
			{
				if( history >= 1.0f )
					theta = e0;
				else
				{
					const int j = w - wStart;
					theta       = e0 * ( 1.0 - std::pow( c, j + 1 ) ) / ( 1.0 - c );
				}
			}
			else if( w >= wEnd )
				theta = ( history >= 1.0f ? e0 : e0 * ( 1.0 - std::pow( c, lengthRows ) ) / ( 1.0 - c ) ) * std::pow( c, w - wEnd + 1 );
			else
				theta = 0.0;

			int predicted = 0, measured = 0;
			for( int col = cLine - 3; col < cLine + lineW + 3; ++col )
			{
				const bool in      = col >= cLine && col < cLine + lineW;
				const double own   = in ? theta : 0.0;
				const double left  = ( col - 1 >= cLine && col - 1 < cLine + lineW ) ? theta : 0.0;
				const double right = ( col + 1 >= cLine && col + 1 < cLine + lineW ) ? theta : 0.0;
				const double q     = own + kSpread * ( left + right );
				const double Dp    = statedResponse( q );
				const double Dm    = dotDensity( out, L, col, w );
				predicted += isInk( Dp ) ? 1 : 0;
				measured += isInk( Dm ) ? 1 : 0;
				worstD = std::max( worstD, std::fabs( Dm - Dp ) );
			}
			if( predicted != measured )
			{
				++rowsWrong;
				if( worstRow < 0 )
					worstRow = w;
			}
			widest = std::max( widest, measured );
			if( firing && predicted > lineW && onsetPredicted < 0 )
				onsetPredicted = w - wStart;
			if( firing && measured > lineW && onsetMeasured < 0 )
				onsetMeasured = w - wStart;
			if( w >= wEnd && predicted > 0 )
				runOnPredicted = w - wEnd + 1;
			if( w >= wEnd && measured > 0 )
				runOnMeasured = w - wEnd + 1;
		}
		const bool ok = rowsWrong == 0 && worstD <= kDTolerance;
		if( history < 1.0f )
			failed += report( ok && onsetPredicted >= 0, quiet,
			                  "History Control 0, Heat Carry %.2f: the %d-dot line is %d dots at its widest (+%d, predicted +2), from row %d (predicted %d); runs on %d rows below its end (predicted %d); every row's width right; worst |D - model| %.1e (tolerance %.0e)",
			                  c, lineW, widest, widest - lineW, onsetMeasured, onsetPredicted, runOnMeasured, runOnPredicted, worstD, kDTolerance );
		else
			failed += report( ok && onsetPredicted < 0 && widest == lineW, quiet,
			                  "History Control 1: the line stays %d dots (widest %d, predicted no thickening); runs on %d rows (predicted %d); worst |D - model| %.1e",
			                  lineW, widest, runOnMeasured, runOnPredicted, worstD );
		if( !quiet && rowsWrong > 0 )
			std::printf( "        %d rows with the wrong width, the first at window row %d\n", rowsWrong, worstRow );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --budget: rows of chosen coverage per strobe block, no heat carried. A
// block's k-th firing heater gets e0 min( 1, kBudget / n_block ); each dot's
// density is S( e + kSpread ( e_left + e_right ) ). Every visible dot of each
// band's middle row is held to that (kDTolerance), and the banding property
// is measured out of the picture alone: between neighbouring fired dots
// whose own neighbours are fired too, the density changes only within one
// dot of a block boundary.
//---------------------------------------------------------------------------
int runBudget( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== budget at %dx%d: a block over the supply's budget prints paler, and bands on the block boundaries\n", W, H );
	int failed = 0;
	Knobs k;
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	const int step = L.alignStep();

	//Visible dots and the band rows.
	int c0 = 0;
	while( c0 < L.N && !L.dotVisible( c0 ) )
		++c0;
	int c1 = c0;
	while( c1 < L.N && L.dotVisible( c1 ) )
		++c1;
	const int bandRows = step;
	int i0             = alignedRow( L, L.imageRowOf( 0 ) + 1 );
	const int bands    = 4;
	if( !L.rowVisible( L.windowRowOf( i0 + bands * bandRows ) ) )
		return tooSmall( "--budget", W, H );

	auto snap = [ & ]( int c ) { return ( c / step ) * step; };
	for( int S : { 2, 3, 4 } )
	{
		k.blocks = S;
		//The bands' black runs, source-aligned: full; the left half; one
		//run per block of a different length (so every block's count
		//differs); a sparse row under budget everywhere.
		std::vector< std::vector< std::pair< int, int > > > runs( bands );
		runs[ 0 ].push_back( { 0, L.N } );
		runs[ 1 ].push_back( { 0, snap( L.N / 2 ) } );
		for( int b = 0; b < S; ++b )
		{
			const int x0 = static_cast< int >( static_cast< int64_t >( b ) * L.N / S );
			const int x1 = static_cast< int >( static_cast< int64_t >( b + 1 ) * L.N / S );
			const int from = ( ( x0 + step - 1 ) / step ) * step;
			const int len  = snap( ( x1 - from ) * ( S - b ) / S );
			if( len > 0 )
				runs[ 2 ].push_back( { from, from + len } );
		}
		for( int x = 0; x + step <= L.N; x += 8 * step )
			runs[ 3 ].push_back( { x, x + step } );

		Picture pic = flat( W, H, 1.0 );
		for( int band = 0; band < bands; ++band )
			for( const auto& r : runs[ band ] )
				if( !paintDots( pic, L, r.first, r.second, i0 + band * bandRows, i0 + ( band + 1 ) * bandRows, 0.0 ) )
					return failed + report( false, quiet, "a band could not be painted source-aligned" );

		std::vector< float > out;
		if( !renderStatic( k, perturb, W, H, pic, out ) )
			return failed + report( false, quiet, "render failed" );

		double worstD = 0.0, palest = 1.0;
		int jumps = 0, strayJumps = 0;
		const double e0 = statedEnergy( k.density );
		for( int band = 0; band < bands; ++band )
		{
			const int w = L.windowRowOf( i0 + band * bandRows + bandRows / 2 );
			std::vector< int > fired( static_cast< size_t >( L.N ), 0 );
			for( const auto& r : runs[ band ] )
				for( int x = r.first; x < r.second; ++x )
					fired[ static_cast< size_t >( x ) ] = 1;
			std::vector< double > e( static_cast< size_t >( L.N ), 0.0 );
			for( int b = 0; b < S; ++b )
			{
				const int x0 = static_cast< int >( static_cast< int64_t >( b ) * L.N / S );
				const int x1 = static_cast< int >( static_cast< int64_t >( b + 1 ) * L.N / S );
				int n        = 0;
				for( int x = x0; x < x1; ++x )
					n += fired[ static_cast< size_t >( x ) ];
				const double scale = n > kBudget ? kBudget / n : 1.0;
				for( int x = x0; x < x1; ++x )
					e[ static_cast< size_t >( x ) ] = fired[ static_cast< size_t >( x ) ] * e0 * scale;
			}
			std::vector< double > measured( static_cast< size_t >( L.N ), -1.0 );
			for( int x = c0; x < c1; ++x )
			{
				const double left  = x > 0 ? e[ static_cast< size_t >( x - 1 ) ] : 0.0;
				const double right = x + 1 < L.N ? e[ static_cast< size_t >( x + 1 ) ] : 0.0;
				const double Dp    = statedResponse( e[ static_cast< size_t >( x ) ] + kSpread * ( left + right ) );
				const double Dm    = dotDensity( out, L, x, w );
				measured[ static_cast< size_t >( x ) ] = Dm;
				worstD                                 = std::max( worstD, std::fabs( Dm - Dp ) );
				if( fired[ static_cast< size_t >( x ) ] )
					palest = std::min( palest, Dm );
			}
			//The banding, out of the picture: interior fired pairs.
			for( int x = std::max( c0, 1 ); x + 2 < c1; ++x )
			{
				const size_t a = static_cast< size_t >( x );
				if( !( fired[ a - 1 ] && fired[ a ] && fired[ a + 1 ] && fired[ a + 2 ] ) )
					continue;
				if( std::fabs( measured[ a ] - measured[ a + 1 ] ) <= 1e-3 )
					continue;
				++jumps;
				bool nearBoundary = false;
				for( int b = 1; b < S; ++b )
				{
					const int boundary = static_cast< int >( static_cast< int64_t >( b ) * L.N / S );
					nearBoundary       = nearBoundary || std::fabs( ( x + 1.0 ) - boundary ) <= 1.0;
				}
				strayJumps += nearBoundary ? 0 : 1;
			}
		}
		failed += report( worstD <= kDTolerance && strayJumps == 0 && jumps > 0, quiet,
		                  "Strobe Blocks %d: every dot of 4 bands at the stated sag, worst |D - model| %.1e (tolerance %.0e); a full row prints at D = %.3f; %d density steps between fired dots, %d of them off a block boundary",
		                  S, worstD, kDTolerance, palest, jumps, strayJumps );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --slip: a forced slip, no heat carried (so a row's densities are a function
// of its own bits), against the same picture unslipped. A repeat of m at
// receipt row r puts the data row of paper row r on rows r..r+m and moves
// every later row down by m; a stall of m strikes data rows r..r+m onto row r
// (the darkest wins) and moves every later row up by m. Held dot for dot
// (kDTolerance across two renders); the rows compared must differ from their
// neighbours, or the check could not see a slip at all.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame );

Picture cardPicture( int W, int H, int64_t frame )
{
	const std::vector< unsigned char > bytes = buildCard( W, H, frame );
	Picture p( bytes.size() );
	for( size_t i = 0; i < bytes.size(); ++i )
		p[ i ] = bytes[ i ] / 255.0f;
	return p;
}

int runSlip( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== slip at %dx%d: a forced slip repeats or overprints exactly the stated rows\n", W, H );
	int failed = 0;
	Knobs k;
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	int c0 = 0;
	while( c0 < L.N && !L.dotVisible( c0 ) )
		++c0;
	int c1 = c0;
	while( c1 < L.N && L.dotVisible( c1 ) )
		++c1;
	int rows = 0;
	while( L.rowVisible( rows ) )
		++rows;
	if( rows < 40 )
		return tooSmall( "--slip", W, H );

	const Picture pic = cardPicture( W, H, 0 );
	std::vector< float > plain;
	if( !renderStatic( k, perturb, W, H, pic, plain ) )
		return failed + report( false, quiet, "render failed" );
	auto rowOf = [ & ]( const std::vector< float >& out, int w ) {
		std::vector< double > d;
		for( int c = c0; c < c1; ++c )
			d.push_back( dotDensity( out, L, c, w ) );
		return d;
	};
	auto same = [ & ]( const std::vector< double >& a, const std::vector< double >& b ) {
		for( size_t i = 0; i < a.size(); ++i )
			if( std::fabs( a[ i ] - b[ i ] ) > kDTolerance )
				return false;
		return true;
	};

	for( int kind : { model::kRepeat, model::kStall } )
		for( int m : { 1, 3 } )
		{
			const int wSlip = rows / 3;
			std::vector< float > slipped;
			if( !renderStatic( k, perturb, W, H, pic, slipped,
			                   [ & ]( Receipt& p ) { p.ForceSlipForTest( L.dataRowOf( wSlip ), kind, m ); } ) )
				return failed + report( false, quiet, "render failed" );

			int wrong = 0, compared = 0, distinct = 0;
			for( int w = 0; w < rows; ++w )
			{
				const std::vector< double > got = rowOf( slipped, w );
				std::vector< double > want;
				if( w < wSlip )
					want = rowOf( plain, w );
				else if( kind == model::kRepeat )
				{
					const int src = w <= wSlip + m ? wSlip : w - m;
					want          = rowOf( plain, src );
				}
				else if( w == wSlip )
				{
					if( wSlip + m >= rows )
						continue;
					want = rowOf( plain, wSlip );
					for( int j = 1; j <= m; ++j )
					{
						const std::vector< double > more = rowOf( plain, wSlip + j );
						for( size_t i = 0; i < want.size(); ++i )
							want[ i ] = std::max( want[ i ], more[ i ] );
					}
				}
				else
				{
					if( w + m >= rows )
						continue;
					want = rowOf( plain, w + m );
				}
				++compared;
				wrong += same( got, want ) ? 0 : 1;
			}
			//The check can see a slip: the unslipped rows it moves differ.
			for( int j = 0; j <= m + 1; ++j )
				distinct += same( rowOf( plain, wSlip + j ), rowOf( plain, wSlip + j + 1 ) ) ? 0 : 1;
			failed += report( wrong == 0 && distinct == m + 2, quiet,
			                  "%s of %d at window row %d: %d rows compared with the unslipped print, %d wrong; the %d rows it moves are all distinct (%d of %d)",
			                  kind == model::kRepeat ? "a repeat" : "a stall ", m, wSlip, compared, wrong, m + 2, distinct, m + 2 );
		}
	return failed;
}

//---------------------------------------------------------------------------
// --grid: every dot lands on its cell. The source is a checker of source-
// aligned squares, so every dot is exactly black or white and nothing
// bleeds (no heat carried; an unfired dot beside a fired one is at q =
// kSpread e0, under the threshold). Every output pixel is classified from
// the stated layout -- dot ( floor( ( X - x0 ) / p ), floor( Y / p ) ) -- and
// must be ink, paper or off the paper exactly as the checker says; every
// cell's pixels must be identical, channel for channel; and the paper's
// first and last columns must be x0 and x0 + N p - 1 where they are in view.
//---------------------------------------------------------------------------
int runGrid( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== grid at %dx%d: every dot lands on its whole-pixel cell of the head's grid\n", W, H );
	int failed = 0;
	Knobs k;
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	const int step = L.alignStep();

	Picture pic = flat( W, H, 1.0 );
	auto black  = [ & ]( int c, int i ) { return i >= 0 && i < L.imageRows && ( ( c / step ) + ( i / step ) ) % 2 == 0 && ( ( i / step ) % 5 ) != 4; };
	for( int i = 0; i < L.imageRows; i += step )
		for( int c = 0; c < L.N; c += step )
			if( black( c, i ) )
				paintDots( pic, L, c, c + step, i, std::min( i + step, ( L.imageRows / step ) * step ), 0.0 );

	std::vector< float > out;
	if( !renderStatic( k, perturb, W, H, pic, out ) )
		return failed + report( false, quiet, "render failed" );

	long wrong = 0, inkPixels = 0, paperPixels = 0, offPixels = 0, nonUniform = 0;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const int c = floorDiv( x - L.x0, L.p );
			const int w = y / L.p;
			const int i = L.imageRowOf( w );
			const float a = at( out, W, y, x, 3 );
			const float r = at( out, W, y, x, 0 );
			int want = 0;//0 off, 1 paper, 2 ink
			if( c >= 0 && c < L.N )
				want = ( i >= 0 && i < ( L.imageRows / step ) * step && black( c, i ) ) ? 2 : 1;
			int got = a < 0.5f ? 0 : ( r < 0.5f * kPaperRed ? 2 : 1 );
			wrong += got == want ? 0 : 1;
			inkPixels += got == 2;
			paperPixels += got == 1;
			offPixels += got == 0;
			//Every pixel of a cell is its cell's first pixel, exactly.
			const int cx = L.x0 + c * L.p, cy = w * L.p;
			const int fx = std::max( 0, cx ), fy = cy;
			for( int ch = 0; ch < 4; ++ch )
				if( at( out, W, y, x, ch ) != at( out, W, fy, fx, ch ) )
				{
					++nonUniform;
					break;
				}
		}
	//The paper's own extent, from alpha, along a middle row.
	int first = -1, last = -1;
	const int midY = ( L.Rw / 2 ) * L.p;
	for( int x = 0; x < W; ++x )
		if( at( out, W, midY, x, 3 ) >= 0.5f )
		{
			if( first < 0 )
				first = x;
			last = x;
		}
	const int wantFirst = std::max( 0, L.x0 ), wantLast = std::min( W - 1, L.x0 + L.N * L.p - 1 );
	failed += report( wrong == 0 && inkPixels > 0 && paperPixels > 0, quiet,
	                  "pitch %d px, head at x = %d: %ld pixels classified (%ld ink, %ld paper, %ld off), %ld wrong", L.p, L.x0,
	                  static_cast< long >( W ) * H, inkPixels, paperPixels, offPixels, wrong );
	failed += report( nonUniform == 0, quiet, "every %dx%d cell uniform in all four channels: %ld pixels differ from their cell%s", L.p, L.p, nonUniform,
	                  L.p == 1 ? " (at pitch 1 a cell is one pixel, so this half is trivially true here)" : "" );
	failed += report( first == wantFirst && last == wantLast, quiet, "the paper spans columns %d..%d (stated %d..%d)", first, last, wantFirst, wantLast );
	return failed;
}

//---------------------------------------------------------------------------
// --printing: Printing's rows come from elapsed time. At rate r rows a
// second and a frame clocked at t_k = k / F, frame k has printed
// P( k ) = floor( r t_k ) rows (the first frame primes the clock). A frame
// k0 of a black source prints rows [ P( k0 - 1 ), P( k0 ) ) black; at frame
// K the slot is the bottom window row, so they sit at window rows
// [ P( k0 - 1 ) - P( K ) + R_w, P( k0 ) - P( K ) + R_w ), and the receipt's
// top is at R_w - P( K ). The same seconds at 60, 50 and 30 frames a second
// must give the same rows at the same times. Then a Tear Length of 30 mm
// (240 rows): the receipt tears at 240 and the paper above the new one is
// gone. Held exactly, in whole rows, in a column whose tear notch is 0.
//---------------------------------------------------------------------------
int runPrinting( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== printing at %dx%d: the receipt scrolls in elapsed time, and tears at the length\n", W, H );
	int failed = 0;
	Knobs k;
	k.mode = model::kPrinting;
	k.fit  = model::kTile;//every data row prints the frame: no blank lead or tail
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	int column = L.N / 2 + 4 - ( L.N / 2 ) % 8;//notch 0: column % 8 == 4
	if( !L.dotVisible( column ) || model::TearNotch( column ) != 0 )
		return tooSmall( "--printing", W, H );
	const double r = statedRowsPerSecond( k.speed );

	auto P = [ & ]( long frame, double fps ) {
		const double rows = r * ( static_cast< double >( frame ) / fps );
		return static_cast< long >( std::floor( rows ) );
	};

	for( double fps : { 60.0, 50.0, 30.0 } )
	{
		const long k0 = std::lround( 0.25 * fps ), K = std::lround( 0.45 * fps );
		if( P( K, fps ) >= L.Rw )
			return tooSmall( "--printing", W, H );
		Session s;
		s.fps = fps;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) )
			return failed + report( false, quiet, "InitGL failed" );
		const Picture white = flat( W, H, 1.0 ), black = flat( W, H, 0.0 );
		for( long f = 0; f <= K; ++f )
			if( !s.render( f, f == k0 ? black : white ) )
				return failed + report( false, quiet, "render failed" );
		const std::vector< float > out = s.readBackFloat();
		const long printed             = s.plugin.StatsForTest().printed;
		s.end();

		const long total = P( K, fps );
		//floor( r t ) is only a statement where r t is not within a rounding
		//of an integer (the plugin sums r dt frame by frame; the harness
		//multiplies once). Asserted, not assumed.
		double nearest = 1.0;
		for( long f : { k0 - 1, k0, K } )
		{
			const double x = r * ( static_cast< double >( f ) / fps );
			nearest        = std::min( nearest, std::fabs( x - std::round( x ) ) );
		}
		if( nearest < 1e-9 )
			return failed + report( false, quiet, "%2.0f fps: r t lands within 1e-9 of a whole row; pick another speed", fps );
		const int bandFrom = static_cast< int >( P( k0 - 1, fps ) - total + L.Rw ), bandTo = static_cast< int >( P( k0, fps ) - total + L.Rw );
		const int topWant  = static_cast< int >( L.Rw - total );
		int wrong = 0, topGot = -1;
		for( int w = 0; w < L.Rw; ++w )
		{
			if( !L.rowVisible( w ) )
				continue;
			const double D = dotDensity( out, L, column, w );
			if( D > -1.5 && D < -0.5 )
			{
				if( w >= topWant )
					++wrong;//should be paper
				continue;
			}
			if( topGot < 0 )
				topGot = w;
			const bool want = w >= bandFrom && w < bandTo;
			wrong += isInk( D ) == want ? 0 : 1;
		}
		//A near-integer r t would make floor() a coin toss in either
		//double; none of these is within 1e-9 of one.
		failed += report( wrong == 0 && topGot == topWant && printed == total, quiet,
		                  "%2.0f fps: %ld rows after %.2f s (plugin %ld); frame %ld's rows at window rows %d..%d, receipt top at row %d (measured %d); %d rows wrong",
		                  fps, total, K / fps, printed, k0, bandFrom, bandTo - 1, topWant, topGot, wrong );
	}

	//The tear.
	{
		Knobs t    = k;
		t.tearLength = 0.0f;
		t.speed      = 0.5f;
		const int T  = statedTearRows( t.tearLength );
		const double rt = statedRowsPerSecond( t.speed );
		const double fps = 60.0;
		long K           = 0;
		while( std::floor( rt * K / fps ) < T + 40 )
			++K;
		const long total = static_cast< long >( std::floor( rt * K / fps ) );
		const long since = total - T;//rows on the new receipt
		if( since >= L.Rw )
			return tooSmall( "--printing (tear)", W, H );
		Session s;
		s.fps = fps;
		apply( s.plugin, t );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) )
			return failed + report( false, quiet, "InitGL failed" );
		const Picture card = cardPicture( W, H, 0 );
		for( long f = 0; f <= K; ++f )
			if( !s.render( f, card ) )
				return failed + report( false, quiet, "render failed" );
		const std::vector< float > out = s.readBackFloat();
		const Receipt::Stats stats     = s.plugin.StatsForTest();
		s.end();
		int topGot = -1;
		for( int w = 0; w < L.Rw && topGot < 0; ++w )
			if( L.rowVisible( w ) && dotDensity( out, L, column, w ) > -0.5 )
				topGot = w;
		const int topWant = static_cast< int >( L.Rw - since );
		failed += report( topGot == topWant && stats.receiptStart == T, quiet,
		                  "Tear Length %d rows: after %ld rows the receipt tore at row %lld (stated %d) and the new one's top is at window row %d (stated %d)",
		                  T, total, static_cast< long long >( stats.receiptStart ), T, topGot, topWant );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --resize: Printing on the card at W x H, then the SAME instance handed a
// different output size (no DeInitGL) and a white frame. Every paper row
// and dot visible in both frames must carry the same density (kDTolerance):
// the paper is held at head resolution, not at the output's.
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== resize at %dx%d: a change of output size mid-print keeps the printed paper\n", W, H );
	int failed = 0;
	Knobs k;
	k.mode      = model::kPrinting;
	k.fit       = model::kTile;
	k.heatCarry = 0.5f;
	k.history   = 0.5f;
	k.speed     = 0.6f;
	const int W2 = W * 3 / 4 + ( W * 3 / 4 ) % 2, H2 = H * 3 / 4 + ( H * 3 / 4 ) % 2;
	const Layout A( W, H, kDots80, statedTearRows( k.tearLength ) ), B( W2, H2, kDots80, statedTearRows( k.tearLength ) );

	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( W, H ) )
		return failed + report( false, quiet, "InitGL failed" );
	const long K = 50;
	for( long f = 0; f < K; ++f )
		if( !s.render( f, cardPicture( W, H, f ) ) )
			return failed + report( false, quiet, "render failed" );
	const std::vector< float > before = s.readBackFloat();
	const long printedA               = s.plugin.StatsForTest().printed;

	s.resize( W2, H2 );
	if( !s.render( K, flat( W2, H2, 1.0 ) ) )
		return failed + report( false, quiet, "render failed" );
	const std::vector< float > after = s.readBackFloat();
	const long printedB              = s.plugin.StatsForTest().printed;
	s.end();

	long compared = 0, wrong = 0, ink = 0;
	for( long j = printedA - A.Rw; j < printedA; ++j )
	{
		const int wA = static_cast< int >( j - printedA + A.Rw ), wB = static_cast< int >( j - printedB + B.Rw );
		if( j < 0 || !A.rowVisible( wA ) || !B.rowVisible( wB ) )
			continue;
		for( int c = 0; c < kDots80; ++c )
		{
			if( !A.dotVisible( c ) || !B.dotVisible( c ) || model::TearNotch( c ) > 0 )
				continue;
			const double dA = dotDensity( before, A, c, wA ), dB = dotDensity( after, B, c, wB );
			if( dA < -0.5 )
				continue;//not paper before (above the receipt's top)
			++compared;
			ink += isInk( dA ) ? 1 : 0;
			wrong += std::fabs( dA - dB ) <= kDTolerance ? 0 : 1;
		}
	}
	failed += report( wrong == 0 && compared > 1000 && ink > 100, quiet,
	                  "%dx%d -> %dx%d after %ld rows (%ld more printed in the resized frame): %ld dots in view in both compared (%ld ink), %ld changed",
	                  W, H, W2, H2, printedA, printedB - printedA, compared, ink, wrong );
	return failed;
}

//---------------------------------------------------------------------------
// --alpha: the output alpha, decided. The paper is a sheet: alpha 1 however
// transparent the clip. Off the paper is transparent black (0, 0, 0, 0), so
// the layer below shows round the receipt. Mix fades both toward the source,
// alpha included: mix( src, receipt, Mix ). A clear source prints as blank
// paper: the tone is luma over paper white by the source's alpha.
//---------------------------------------------------------------------------
int runAlpha( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "== alpha at %dx%d: opaque paper, transparent off it, Mix fades both; a clear clip prints blank\n", W, H );
	int failed = 0;
	Knobs k;
	k.mode = model::kPrinting;//few rows printed: most of the window is off the paper
	const Layout L( W, H, kDots80, statedTearRows( k.tearLength ) );
	const Picture src = flat( W, H, 0.5, 0.5 );

	for( float mix : { 1.0f, 0.25f, 0.0f } )
	{
		k.mix = mix;
		Session s;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) )
			return failed + report( false, quiet, "InitGL failed" );
		for( long f = 0; f < 8; ++f )
			if( !s.render( f, src ) )
				return failed + report( false, quiet, "render failed" );
		const std::vector< float > out = s.readBackFloat();
		const long printed             = s.plugin.StatsForTest().printed;
		s.end();
		long paper = 0, off = 0, wrong = 0;
		//2 ulp of the mix's two products and a sum, at 1.
		const double tol = 3.0 * 5.96e-8;
		for( int y = 0; y < H; ++y )
			for( int x = 0; x < W; ++x )
			{
				const int c = floorDiv( x - L.x0, L.p ), w = y / L.p;
				const bool onPaper = c >= 0 && c < L.N && w >= L.Rw - printed + model::TearNotch( c ) && w < L.Rw;
				const double a     = at( out, W, y, x, 3 );
				const double want  = onPaper ? mix * 1.0 + ( 1.0 - mix ) * 0.5 : ( 1.0 - mix ) * 0.5;
				wrong += std::fabs( a - want ) <= tol ? 0 : 1;
				if( mix == 0.0f )
					for( int ch = 0; ch < 3; ++ch )
						wrong += at( out, W, y, x, ch ) == 0.5f ? 0 : 1;
				( onPaper ? paper : off ) += 1;
			}
		failed += report( wrong == 0 && paper > 0 && off > 0, quiet,
		                  "Mix %.2f: alpha %.3f on %ld paper pixels and %.3f on %ld off it, from a half-transparent clip; %ld wrong%s", mix,
		                  mix + ( 1.0 - mix ) * 0.5, paper, ( 1.0 - mix ) * 0.5, off, wrong, mix == 0.0f ? " (and every colour the source's)" : "" );
	}

	//A fully transparent black clip is nothing: blank paper, not a black
	//receipt.
	{
		Knobs st;
		std::vector< float > out;
		if( !renderStatic( st, perturb, W, H, flat( W, H, 0.0, 0.0 ), out ) )
			return failed + report( false, quiet, "render failed" );
		long dots = 0, inked = 0;
		for( int w = 0; w < L.Rw; ++w )
			for( int c = 0; c < L.N; ++c )
			{
				const double D = dotDensity( out, L, c, w );
				if( D < -0.5 )
					continue;
				++dots;
				inked += D > kDTolerance ? 1 : 0;
			}
		failed += report( dots > 0 && inked == 0, quiet, "a transparent black clip prints blank paper: %ld dots, %ld inked", dots, inked );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --negative: every model check can fail.
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "== negative controls at %dx%d: each perturbation makes its check FAIL\n", W, H );
	struct Control
	{
		const char* what;
		int perturb;
		int ( *check )( int, int, int, bool );
		const char* checkName;
	};
	const Control controls[] = {
		{ "no heat carried from row to row (Heat Carry 0)", model::kPerturbNoCarry, runHistory, "--history" },
		{ "no supply budget", model::kPerturbNoBudget, runBudget, "--budget" },
		{ "error diffusion that throws its error away", model::kPerturbNoDiffusion, runDither, "--dither" },
		{ "a slip one row shorter than it says", model::kPerturbSlipShort, runSlip, "--slip" },
		{ "a dot pitch of W / N pixels", model::kPerturbFloatPitch, runGrid, "--grid" },
		{ "Printing advancing a 60th of a second a frame", model::kPerturbClockPerFrame, runPrinting, "--printing" },
		{ "a resize that clears the paper", model::kPerturbResizeClears, runResize, "--resize" },
	};
	int failed = 0;
	for( const Control& c : controls )
	{
		const int before      = g_checks;
		const int failsBefore = g_failures;
		const int caught      = c.check( W, H, c.perturb, true );
		//The quiet run's own bookkeeping is undone: only the verdict counts.
		g_checks   = before;
		g_failures = failsBefore;
		failed += report( caught > 0, false, "%s: %s fails (%d of its assertions)", c.what, c.checkName, caught );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --laws: every control law against its statement; the model's promises. No
// GL.
//---------------------------------------------------------------------------
int runLaws()
{
	std::printf( "== laws: every control law against its statement (no GL)\n" );
	namespace ctl = receiptfx::controls;
	int failed    = 0;
	double worst  = 0.0;
	bool tearOk   = true;
	for( int i = 0; i <= 20; ++i )
	{
		const float v = static_cast< float >( i ) / 20.0f;
		auto rel      = [ & ]( double a, double b ) { return std::fabs( a - b ) / std::max( 1e-12, std::fabs( b ) ); };
		worst         = std::max( worst, rel( ctl::Energy( v ), statedEnergy( v ) ) );
		worst         = std::max( worst, rel( ctl::Carry( v ), statedCarry( v ) ) );
		worst         = std::max( worst, rel( ctl::History( v ), unit( v ) ) );
		worst         = std::max( worst, rel( ctl::SpeedMmPerSecond( v ), statedSpeed( v ) ) );
		worst         = std::max( worst, rel( ctl::RowsPerSecond( v ), statedRowsPerSecond( v ) ) );
		worst         = std::max( worst, rel( ctl::SlipRate( v ), statedSlipRate( v ) ) );
		worst         = std::max( worst, rel( ctl::TearMm( v ), statedTearMm( v ) ) );
		worst         = std::max( worst, rel( ctl::Age( v ), unit( v ) ) );
		tearOk        = tearOk && ctl::TearRows( v ) == statedTearRows( v );
	}
	failed += report( worst <= 1e-12 && tearOk, false, "9 laws at 21 points: worst relative difference %.1e; Tear Length in rows agrees at all 21", worst );
	failed += report( ctl::Energy( 0.5f ) == model::kSaturation, false, "Density 0.5 is exactly the paper's saturation energy: a lone fresh dot just saturates" );
	failed += report( ctl::StrobeBlocks( 0.4f ) == 1 && ctl::StrobeBlocks( 3.6f ) == 4 && ctl::StrobeBlocks( 99.0f ) == 8, false, "Strobe Blocks rounds and clamps to 1..8" );
	failed += report( ctl::OptionIndex( 5.4f, 6 ) == 5 && ctl::OptionIndex( -1.0f, 3 ) == 0 && ctl::OptionIndex( 1.5f, 2 ) == 1, false, "options map by index" );

	//The model's constants against their statements here.
	failed += report( model::kSpread == kSpread && model::kThreshold == kQ0 && model::kSaturation == kQ1 && model::kBudgetDots == kBudget && model::kMaxOD == kODMax
	                      && model::kWidthDots[ model::k80mm ] == kDots80 && model::kWidthDots[ model::k58mm ] == 384 && model::kDotsPerMm == 8.0,
	                  false, "the head's constants are the ones stated: 8 dots/mm, 384 and 576 dots, spread 0.22, threshold 0.35, saturation 1, budget 96, OD 1.3" );
	failed += report( static_cast< double >( model::kTintRGB[ model::kWhite ][ 0 ] ) == static_cast< double >( static_cast< float >( kPaperRed ) ) && model::kInkFresh[ 0 ] == 1.0f,
	                  false, "the white stock's red is %.3f and the fresh dye's red density is 1 x OD", kPaperRed );

	//The Bayer table against the recursion.
	bool bayer = true;
	std::set< int > values;
	for( int r = 0; r < 8; ++r )
		for( int c = 0; c < 8; ++c )
		{
			bayer = bayer && model::kBayer8[ r ][ c ] == statedBayer( r, c );
			values.insert( model::kBayer8[ r ][ c ] );
		}
	failed += report( bayer && values.size() == 64, false, "the 8x8 Bayer table is the recursive construction, a permutation of 0..63" );

	//The layout laws.
	bool layout = true;
	for( int W : { 320, 384, 575, 576, 1152, 1280, 1920, 3840 } )
		for( int N : { 384, 576 } )
		{
			const int p = std::max( 1, W / N );
			layout      = layout && model::Pitch( W, N ) == p && model::StripX0( W, N, p ) == floorDiv( W - N * p, 2 );
		}
	for( int H : { 1, 179, 180, 181, 720, 2160 } )
		for( int p : { 1, 2, 3, 6 } )
			layout = layout && model::WindowRows( H, p ) == ( H + p - 1 ) / p;
	failed += report( layout, false, "pitch = max( 1, floor( W / N ) ), x0 = floor( ( W - N p ) / 2 ), rows = ceil( H / p ), at 16 widths and 24 heights" );

	//The tear notch.
	bool notch = true;
	for( int c = -16; c < 32; ++c )
	{
		const int r = ( ( c % 8 ) + 8 ) % 8;
		notch       = notch && model::TearNotch( c ) == std::abs( r - 4 ) / 2;
	}
	failed += report( notch, false, "the tear notch is | ( c mod 8 ) - 4 | / 2, 0 to 2 rows" );

	//Slips: the rate, the split and the lengths, over two million rows. A
	//binomial count is within 6 sigma of its mean with probability 1 - 2e-9.
	{
		const double rate = statedSlipRate( 1.0f );
		const int64_t rows = 2000000;
		long events = 0, stalls = 0;
		long length[ model::kMaxSlipRows + 1 ] = {};
		for( int64_t j = 0; j < rows; ++j )
		{
			const model::Slip s = model::SlipAt( j, rate );
			if( s.kind == model::kNoSlip )
				continue;
			++events;
			stalls += s.kind == model::kStall ? 1 : 0;
			if( s.rows >= 1 && s.rows <= model::kMaxSlipRows )
				++length[ s.rows ];
		}
		const double mean = rate * rows, sigma = std::sqrt( rows * rate * ( 1.0 - rate ) );
		bool lengths      = true;
		for( int m = 1; m <= model::kMaxSlipRows; ++m )
		{
			const double pm = 1.0 / model::kMaxSlipRows;
			lengths         = lengths && std::fabs( length[ m ] - events * pm ) <= 6.0 * std::sqrt( events * pm * ( 1 - pm ) );
		}
		failed += report( std::fabs( events - mean ) <= 6.0 * sigma && std::fabs( stalls - events / 2.0 ) <= 6.0 * std::sqrt( events * 0.25 ) && lengths
		                      && model::SlipAt( 12345, 0.0 ).kind == model::kNoSlip,
		                  false, "Slip 1: %ld events in %lld rows (stated %.0f +- %.0f), %ld stalls, lengths 1..4 at %ld %ld %ld %ld; Slip 0 never slips",
		                  events, static_cast< long long >( rows ), mean, 6.0 * sigma, stalls, length[ 1 ], length[ 2 ], length[ 3 ], length[ 4 ] );
	}
	return failed;
}

//---------------------------------------------------------------------------
// --names: what a host will show, and what oxbow will read.
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "== names: nothing the host silently truncates (no GL)\n" );
	int failed = 0;
	Receipt plugin;
	std::set< std::string > seen;
	int longest = 0;
	std::string longestName;
	bool unique = true;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( !seen.insert( p.name ).second )
		{
			unique = false;
			std::printf( "        duplicate: %s\n", p.name.c_str() );
		}
		if( static_cast< int >( p.name.size() ) > longest )
		{
			longest     = static_cast< int >( p.name.size() );
			longestName = p.name;
		}
	}
	failed += report( longest <= 16, false, "longest parameter name is %d characters (%s); the limit is 16", longest, longestName.c_str() );
	failed += report( unique, false, "every parameter name is unique (%zu of them)", seen.size() );
	failed += report( std::strlen( "SW Receipt" ) <= 16, false, "the plugin name 'SW Receipt' fits the 16-character field" );
	failed += report( std::strlen( "RC01" ) == 4, false, "the id 'RC01' is four characters" );
	return failed;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench, the slip check and a
// default --pipe: a light background, a photograph-like gradient panel, a big
// solid, a row of smaller solids, thin rules, text-like marks and a moving
// bar -- greys for the dither, solids for the budget and the heat, lines for
// the history control.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const double barX = std::fmod( 40.0 + 200.0 * t, static_cast< double >( width ) );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double v = 0.86;
			if( fx > 0.04 && fx < 0.46 && fy > 0.06 && fy < 0.52 )
			{
				const double dx = ( fx - 0.25 ) / 0.21, dy = ( fy - 0.29 ) / 0.23;
				v = std::clamp( 0.1 + 0.8 * std::sqrt( dx * dx + dy * dy ), 0.0, 1.0 );
			}
			if( fx > 0.55 && fx < 0.95 && fy > 0.08 && fy < 0.42 )
				v = 0.04;
			for( int i = 0; i < 5; ++i )
			{
				const double s  = 0.09 - 0.016 * i;
				const double x0 = 0.06 + 0.19 * i;
				if( fx > x0 && fx < x0 + s * height / width && fy > 0.58 && fy < 0.58 + s )
					v = 0.0;
			}
			if( fy > 0.72 && fy < 0.735 )
				v = 0.0;
			if( std::fabs( fx - 0.5 ) < 0.004 && fy > 0.45 && fy < 0.95 )
				v = 0.0;
			if( fy > 0.78 && fy < 0.94 && std::fmod( fx * 40.0, 1.0 ) < 0.35 && std::fmod( fy * 60.0, 1.0 ) < 0.6 && ( ( x / 9 + y / 7 ) % 3 ) != 0 )
				v = 0.15;
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 160.0 ) )
				v = 0.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< unsigned char >( std::lround( 255.0 * v ) );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench: ProcessOpenGL, glFinish both sides, and the plugin's own stage
// times. The host hands over a texture it already has, so the cards are
// uploaded once and cycled by handle.
//---------------------------------------------------------------------------
struct BenchResult
{
	double ms = -1.0, sampleRead = 0.0, engine = 0.0, upload = 0.0;
};

BenchResult benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	BenchResult result;
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return result;

	std::vector< GLuint > loop;
	for( int i = 0; i < 4; ++i )
	{
		const std::vector< unsigned char > card = flipRows( buildCard( width, height, i * 7 ), width, height );
		loop.push_back( makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, card.data() ) );
	}
	long frame = 0;
	auto one   = [ & ]() {
		session.inputStruct.Handle = loop[ static_cast< size_t >( frame ) % loop.size() ];
		session.renderAt( frame++ );
	};
	for( int i = 0; i < 10; ++i )
		one();
	glFinish();

	//Best of three: the GPU and the CPU are shared with other builds.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		double sr = 0.0, en = 0.0, up = 0.0;
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
		{
			one();
			const Receipt::Stats& st = session.plugin.StatsForTest();
			sr += st.sampleReadMs;
			en += st.engineMs;
			up += st.uploadMs;
		}
		glFinish();
		const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames;
		if( ms < best )
		{
			best              = ms;
			result.sampleRead = sr / frames;
			result.engine     = en / frames;
			result.upload     = up / frames;
		}
	}
	result.ms = best;
	for( GLuint t : loop )
		glDeleteTextures( 1, &t );
	session.end();
	return result;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	struct Case
	{
		const char* name;
		std::vector< std::string > extra;
	};
	const Case cases[] = {
		{ "Static, Floyd-Steinberg (the default)", {} },
		{ "Static, Bayer", { "Dither=0" } },
		{ "Static, Rotate", { "Fit=1" } },
		{ "Printing", { "Mode=1" } },
	};
	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides.\n", frames );
	std::printf( "Stages are the plugin's own steady_clock times: the sample pass and its read-back (the read waits for\n"
	             "the pass), the CPU engine, and the upload of the printed rows.\n\n" );
	std::printf( "%-38s %-10s %9s %7s  %12s %8s %8s\n", "case", "raster", "ms/frame", "%60fps", "sample+read", "engine", "upload" );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			std::vector< std::string > s = settings;
			s.insert( s.end(), c.extra.begin(), c.extra.end() );
			const BenchResult r = benchAt( s, size.width, size.height, frames );
			std::printf( "%-38s %-10s %9.2f %6.1f%%  %12.2f %8.2f %8.2f\n", c.name, size.name, r.ms, r.ms / 16.667 * 100.0, r.sampleRead, r.engine, r.upload );
		}
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = receiptfx::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex },
		{ "sample.frag", sh::kSample },
		{ "display.frag", sh::kDisplay },
	};
	static_assert( sizeof( files ) / sizeof( files[ 0 ] ) == sh::kFragmentCount + 1, "the dump lists every shader" );
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Static and Printing to ramp through. An
// event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
// --cues: the cue sheet's kinds, with no GL and no plugin render. This
// plugin declares no boolean, and its only events are the About buttons,
// which open a web browser -- so the step rules for those two kinds are
// held here, on valueAt itself; verify.sh holds an option, an integer and a
// slider through real --pipe runs.
//---------------------------------------------------------------------------
int runCues()
{
	std::printf( "== cues: sliders ramp; options, booleans and integers step; events fire on their frame (no GL)\n" );
	int failed = 0;
	const Track t = { { 0, 0.0f }, { 4, 1.0f }, { 8, 0.0f } };
	failed += report( valueAt( t, 2, FF_TYPE_STANDARD ) == 0.5f && valueAt( t, 6, FF_TYPE_STANDARD ) == 0.5f, false, "a slider ramps: 0 -> 1 over frames 0..4 is 0.5 at 2, and back down by 6" );
	bool steps = true;
	for( unsigned int type : { FF_TYPE_OPTION, FF_TYPE_BOOLEAN, FF_TYPE_INTEGER } )
		for( int f = 0; f <= 10; ++f )
		{
			const float want = f < 4 ? 0.0f : ( f < 8 ? 1.0f : 0.0f );
			steps            = steps && valueAt( t, f, type ) == want;
		}
	failed += report( steps, false, "an option, a boolean and an integer hold the last cue at or before each of frames 0..10: no value between" );
	const Track press = { { 3, 1.0f }, { 7, 1.0f } };
	bool fires        = true;
	for( int f = 0; f <= 10; ++f )
		fires = fires && valueAt( press, f, FF_TYPE_EVENT ) == ( f == 3 || f == 7 ? 1.0f : 0.0f );
	failed += report( fires, false, "an event is 1 on its cue frames (3 and 7) and 0 on every other" );
	return failed;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rctest -- render and measure the Receipt thermal printer\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/receipt.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             the synthetic clock's frame rate (default 60): frame n is at n / fps seconds\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for Strobe Blocks). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --dither            the mean dot density on flat greys is the tone, within each dither's bound\n"
		"  --history           a vertical line thickens as the heat carry predicts; not under history control\n"
		"  --budget            a block over the supply's budget prints paler; the banding is on block boundaries\n"
		"  --slip              a forced slip repeats or overprints exactly the stated rows\n"
		"  --grid              every dot lands on its whole-pixel cell\n"
		"  --printing          Printing scrolls in elapsed time at any frame rate, and tears at the length\n"
		"  --resize            a change of output size mid-print keeps the printed paper\n"
		"  --alpha             opaque paper, transparent off it, Mix fades both; a clear clip prints blank\n"
		"  --negative          every model check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed printer (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --laws              every control law against its statement; the model's promises\n"
		"  --names             nothing the host will silently truncate; SW Receipt / RC01\n"
		"  --cues              the cue sheet's kinds: sliders ramp, options/booleans/integers step, events fire\n"
		"  --offline           all three; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, by stage\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value' (sliders ramp; options,\n"
		"                      booleans and integers step; events fire on their frame)\n"
		"  --help\n"
		"\n"
		"  RCTEST_RENDERER=software   Apple's software renderer, what a GPU-less CI runner gets\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/receipt.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--dither", "--history", "--budget", "--slip", "--grid", "--printing", "--resize", "--alpha", "--negative" };
	const std::set< std::string > offline  = { "--laws", "--names", "--cues" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--laws", "--names", "--cues" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Receipt plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--laws" )
				runLaws();
			else if( check == "--names" )
				runNames();
			else if( check == "--cues" )
				runCues();
			else
			{
				needGL = true;
				continue;
			}
			offlineRan = true;
			std::printf( "\n" );
		}
		if( offlineRan && !needGL )
			std::printf( "   OFFLINE: --dither, --history, --budget, --slip, --grid, --printing, --resize,\n"
			             "   --alpha and their negative controls were NOT run. Nothing here drew a pixel\n"
			             "   through a GL driver or ran the print engine; the shaders were not exercised,\n"
			             "   only (in CI) compiled by glslc.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--dither" )
						runDither( width, height, perturb );
					else if( check == "--history" )
						runHistory( width, height, perturb );
					else if( check == "--budget" )
						runBudget( width, height, perturb );
					else if( check == "--slip" )
						runSlip( width, height, perturb );
					else if( check == "--grid" )
						runGrid( width, height, perturb );
					else if( check == "--printing" )
						runPrinting( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--alpha" )
						runAlpha( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool rendered = index != failRender && session.render( index, frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
