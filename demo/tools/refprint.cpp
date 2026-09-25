// The reference side of demo/tools/check_port.mjs.
//
// Built by check_port.mjs with -ffp-contract=off (as receipt_core is) against
// the plugin's own source/Printer.cpp and source/Controls.cpp, UNCHANGED, and
// against text CUT OUT of source/Receipt.h and source/Receipt.cpp by
// check_port.mjs into cut_class.inc and cut_defs.inc: the Stats struct, the
// ParamID enum and every private member of Receipt; floorDiv and floorMod;
// Receipt::dataTones, printPaperRow and resetPrinting; and the whole of
// Receipt::ProcessOpenGL. Nothing in those is retyped here.
//
// What IS written here: stand-ins for GL, FFGL and ffglex, so that
// ProcessOpenGL runs with no context. glReadPixels copies in the tones the
// script names (the sample pass is the GPU's and is not in this check);
// glTexSubImage2D records which paper rows were uploaded; the display shader's
// Set records every uniform. nowSeconds() returns the script's clock in
// seconds, which is what SetClockScaleForTest( 1 ) makes the plugin's own
// voting return.
//
//   refprint frames SCRIPT OUTDIR   run a frame script (below)
//   refprint rows TONES N ROWS DITHER BLOCKS DENSITY CARRY HISTORY OUT
//                                   Printer alone: bits, densities and heat per row
//   refprint laws                   the control laws, the hash, SlipAt
//
// A frame script, one command per line:
//   set INDEX VALUE                 params[ INDEX ] = VALUE (a float)
//   frame NOW SRCW SRCH OUTW OUTH TONES   one ProcessOpenGL
//   dump NAME                       write the paper (window or ring) to OUTDIR/NAME
// Every frame prints one line of what it did (runFrames).

#include "Controls.h"
#include "Model.h"
#include "Printer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

//---------------------------------------------------------------------------
// GL and FFGL, stood in for.
//---------------------------------------------------------------------------
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef uint32_t FFUInt32;
typedef uint32_t FFResult;
constexpr FFResult FF_SUCCESS = 0;
constexpr FFResult FF_FAIL    = 1;
enum : GLenum
{
	GL_VIEWPORT = 1,
	GL_FRAMEBUFFER,
	GL_TEXTURE0,
	GL_TEXTURE1,
	GL_TEXTURE_2D,
	GL_RED,
	GL_FLOAT,
	GL_R32F,
};

struct FFGLTextureStruct
{
	FFUInt32 Width, Height;
	FFUInt32 HardwareWidth, HardwareHeight;
	GLuint Handle;
};
struct ProcessOpenGLStruct
{
	FFUInt32 numInputTextures;
	FFGLTextureStruct** inputTextures;
	GLuint HostFBO;
};

namespace stub
{
int viewportW = 0, viewportH = 0;
std::vector< float > tones;   ///< what the next glReadPixels returns
std::string tonesName;
bool readMismatch = false;
GLuint boundTexture = 0;
std::vector< std::pair< int, int > > uploads;///< ( yoffset, rows ) into any texture
GLuint nextTexture = 1;
} // namespace stub

inline void glGetIntegerv( GLenum, GLint* out )
{
	out[ 0 ] = 0;
	out[ 1 ] = 0;
	out[ 2 ] = stub::viewportW;
	out[ 3 ] = stub::viewportH;
}
inline void glBindFramebuffer( GLenum, GLuint ) {}
inline void glViewport( GLint, GLint, GLsizei, GLsizei ) {}
inline void glActiveTexture( GLenum ) {}
inline void glBindTexture( GLenum, GLuint t )
{
	stub::boundTexture = t;
}
inline void glDeleteTextures( GLsizei, const GLuint* ) {}
inline void glUniform2i( GLint, GLint, GLint ) {}
inline void glReadPixels( GLint, GLint, GLsizei w, GLsizei h, GLenum, GLenum, void* out )
{
	const size_t n = static_cast< size_t >( w ) * static_cast< size_t >( h );
	if( n != stub::tones.size() )
	{
		stub::readMismatch = true;
		return;
	}
	std::copy( stub::tones.begin(), stub::tones.end(), static_cast< float* >( out ) );
}
inline void glTexSubImage2D( GLenum, GLint, GLint, GLint y, GLsizei, GLsizei h, GLenum, GLenum, const void* )
{
	stub::uploads.emplace_back( y, h );
}

namespace ffglex
{
struct FFGLShader
{
	std::map< std::string, std::vector< double > > set;
	GLuint GetGLID() const
	{
		return 1;
	}
	GLint FindUniform( const char* ) const
	{
		return 0;
	}
	void Set( const char* name, int v )
	{
		set[ name ] = { static_cast< double >( v ) };
	}
	void Set( const char* name, float v )
	{
		set[ name ] = { static_cast< double >( v ) };
	}
	void Set( const char* name, float a, float b, float c )
	{
		set[ name ] = { a, b, c };
	}
};
struct FFGLScreenQuad
{
	void Draw() {}
};
struct ScopedShaderBinding
{
	explicit ScopedShaderBinding( GLuint ) {}
};
} // namespace ffglex

namespace receiptfx
{
struct PassBuffer
{
	enum class Sampling
	{
		Nearest,
		Linear
	};
	bool Ensure( GLsizei, GLsizei, GLint, Sampling )
	{
		return true;
	}
	GLuint GetGLID() const
	{
		return 2;
	}
	void ResizeViewPort() {}
};
namespace diag
{
inline void error( const std::string& message )
{
	std::fprintf( stderr, "diag: %s\n", message.c_str() );
}
} // namespace diag
} // namespace receiptfx

namespace stoatworks::about
{
constexpr unsigned kParamCount = 5;
}

namespace
{
GLuint makeFloatTexture( int, int, const float* )
{
	return stub::nextTexture++;
}
struct ClientTransfer
{
};
double msSince( std::chrono::steady_clock::time_point )
{
	return 0.0;
}
} // namespace

using namespace ffglex;
using namespace receiptfx;

//---------------------------------------------------------------------------
// Receipt, from its own declarations.
//---------------------------------------------------------------------------
class Receipt
{
public:
#include "cut_class.inc"

public:
	double scriptNow = 0.0;
	double nowSeconds()
	{
		return scriptNow;
	}
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL );

	/// What a frame left behind.
	const ffglex::FFGLShader& Display() const
	{
		return displayShader;
	}
	const ffglex::FFGLShader& Sample() const
	{
		return sampleShader;
	}
	const std::vector< float >& Window() const
	{
		return window;
	}
	const std::vector< float >& Ring() const
	{
		return ring;
	}
	int64_t UploadedTo() const
	{
		return uploadedTo;
	}
	void SetParam( int i, float v )
	{
		params[ i ] = v;
	}
};

#include "cut_defs.inc"

//---------------------------------------------------------------------------
// The harness.
//---------------------------------------------------------------------------
namespace
{
/// Two 32-bit hashes of a float array: FNV-1a over its bytes, and a
/// multiply-add over its words. check_port.mjs computes the same two.
std::string paperHash( const std::vector< float >& v )
{
	const unsigned char* p = reinterpret_cast< const unsigned char* >( v.data() );
	uint32_t a             = 2166136261u;
	for( size_t i = 0; i < v.size() * sizeof( float ); ++i )
	{
		a ^= p[ i ];
		a *= 16777619u;
	}
	uint32_t b = 0x9747B28Cu;
	for( size_t i = 0; i < v.size(); ++i )
	{
		uint32_t w;
		std::memcpy( &w, &v[ i ], sizeof w );
		b = b * 31u + w;
	}
	char buf[ 24 ];
	std::snprintf( buf, sizeof buf, "%08x%08x", a, b );
	return buf;
}

bool readFloats( const std::string& path, std::vector< float >& out )
{
	std::ifstream in( path, std::ios::binary | std::ios::ate );
	if( !in )
		return false;
	const std::streamsize bytes = in.tellg();
	in.seekg( 0 );
	out.resize( static_cast< size_t >( bytes ) / sizeof( float ) );
	in.read( reinterpret_cast< char* >( out.data() ), bytes );
	return static_cast< bool >( in );
}

void writeFloats( const std::string& path, const std::vector< float >& v )
{
	std::ofstream out( path, std::ios::binary );
	out.write( reinterpret_cast< const char* >( v.data() ), static_cast< std::streamsize >( v.size() * sizeof( float ) ) );
}

std::string num( double v )
{
	char buf[ 40 ];
	std::snprintf( buf, sizeof buf, "%.17g", v );
	return buf;
}

std::string uniforms( const ffglex::FFGLShader& s )
{
	std::string out;
	for( const auto& [ name, values ] : s.set )
	{
		if( name == "InputTexture" || name == "PaperTexture" )
			continue;
		out += name + "=";
		for( size_t i = 0; i < values.size(); ++i )
			out += ( i ? "," : "" ) + num( values[ i ] );
		out += " ";
	}
	return out;
}

int runFrames( const std::string& script, const std::string& outdir )
{
	std::ifstream in( script );
	if( !in )
	{
		std::fprintf( stderr, "no script %s\n", script.c_str() );
		return 2;
	}
	Receipt plugin;
	FFGLTextureStruct picture{};
	FFGLTextureStruct* inputs[ 1 ] = { &picture };
	ProcessOpenGLStruct gl{};
	gl.numInputTextures = 1;
	gl.inputTextures    = inputs;

	std::string line;
	int frame = 0;
	while( std::getline( in, line ) )
	{
		std::istringstream words( line );
		std::string verb;
		words >> verb;
		if( verb == "set" )
		{
			int index = 0;
			float value = 0.0f;
			words >> index >> value;
			plugin.SetParam( index, value );
		}
		else if( verb == "frame" )
		{
			double now = 0.0;
			int srcW = 0, srcH = 0, outW = 0, outH = 0;
			std::string tones;
			words >> now >> srcW >> srcH >> outW >> outH >> tones;
			if( tones != stub::tonesName )
			{
				if( !readFloats( tones, stub::tones ) )
				{
					std::fprintf( stderr, "no tones %s\n", tones.c_str() );
					return 2;
				}
				stub::tonesName = tones;
			}
			stub::readMismatch = false;
			stub::uploads.clear();
			stub::viewportW = outW;
			stub::viewportH = outH;
			picture.Width   = static_cast< FFUInt32 >( srcW );
			picture.Height  = static_cast< FFUInt32 >( srcH );
			picture.Handle  = 9;
			plugin.scriptNow = now;
			const FFResult r = plugin.ProcessOpenGL( &gl );
			const auto& st   = plugin.StatsForTest();
			const bool printing = plugin.Display().set.at( "TexRows" )[ 0 ] == static_cast< double >( model::kPaperRows );
			const std::vector< float >& paper = printing ? plugin.Ring() : plugin.Window();
			std::string ups;
			for( const auto& [ y, h ] : stub::uploads )
				ups += std::to_string( y ) + "+" + std::to_string( h ) + ";";
			std::printf( "frame %d result=%u read=%s printed=%lld start=%lld rows=%d dots=%d pitch=%d x0=%d window=%d image=%d uploads=%s paper=%zu:%s | %s| %s\n",
			             frame, r, stub::readMismatch ? "MISMATCH" : "ok", static_cast< long long >( st.printed ), static_cast< long long >( st.receiptStart ),
			             st.rowsThisFrame, st.dots, st.pitch, st.x0, st.windowRows, st.imageRows, ups.c_str(), paper.size(),
			             paperHash( paper ).c_str(),
			             uniforms( plugin.Sample() ).c_str(), uniforms( plugin.Display() ).c_str() );
			++frame;
		}
		else if( verb == "dump" )
		{
			std::string name;
			words >> name;
			const bool printing = plugin.Display().set.at( "TexRows" )[ 0 ] == static_cast< double >( model::kPaperRows );
			writeFloats( outdir + "/" + name, printing ? plugin.Ring() : plugin.Window() );
		}
	}
	return 0;
}

int runRows( int argc, char** argv )
{
	if( argc < 11 )
		return 2;
	std::vector< float > tones;
	if( !readFloats( argv[ 2 ], tones ) )
		return 2;
	Printer::Settings s;
	s.dots    = std::atoi( argv[ 3 ] );
	const int rows = std::atoi( argv[ 4 ] );
	s.dither  = std::atoi( argv[ 5 ] );
	s.blocks  = std::atoi( argv[ 6 ] );
	s.energy  = std::strtod( argv[ 7 ], nullptr );
	s.carry   = std::strtod( argv[ 8 ], nullptr );
	s.history = std::strtod( argv[ 9 ], nullptr );
	if( tones.size() != static_cast< size_t >( s.dots ) * rows )
		return 2;
	Printer printer;
	printer.Configure( s );
	std::vector< uint8_t > bits( static_cast< size_t >( s.dots ) );
	std::vector< float > density( static_cast< size_t >( s.dots ) );
	std::ofstream out( argv[ 10 ], std::ios::binary );
	for( int r = 0; r < rows; ++r )
	{
		printer.Dither( tones.data() + static_cast< size_t >( r ) * s.dots, r, bits.data() );
		printer.Strobe( bits.data(), density.data() );
		out.write( reinterpret_cast< const char* >( bits.data() ), static_cast< std::streamsize >( bits.size() ) );
		out.write( reinterpret_cast< const char* >( density.data() ), static_cast< std::streamsize >( density.size() * sizeof( float ) ) );
		for( int x = 0; x < s.dots; ++x )
		{
			const double heat = printer.HeatForTest( x );
			out.write( reinterpret_cast< const char* >( &heat ), sizeof heat );
		}
	}
	return 0;
}

int runLaws()
{
	std::vector< float > values;
	for( int i = 0; i <= 1000; ++i )
		values.push_back( static_cast< float >( i ) / 1000.0f );
	for( float v : { -0.25f, 1.5f, 0.4999f, 0.5001f, 1.4999f, 1.5001f, 2.5f, 7.49f, 7.51f, 8.6f, 0.33333334f } )
		values.push_back( v );
	for( float v : values )
	{
		std::printf( "law %s %d %d %d %s %s %s %s %s %s %s %s %d %s\n", num( v ).c_str(), controls::OptionIndex( v, 2 ), controls::OptionIndex( v, 6 ),
		             controls::StrobeBlocks( v ), num( controls::Energy( v ) ).c_str(), num( controls::Carry( v ) ).c_str(), num( controls::History( v ) ).c_str(),
		             num( controls::SpeedMmPerSecond( v ) ).c_str(), num( controls::RowsPerSecond( v ) ).c_str(), num( controls::SlipRate( v ) ).c_str(),
		             num( controls::TearMm( v ) ).c_str(), num( controls::Age( v ) ).c_str(), controls::TearRows( v ), "" );
	}
	for( uint32_t v : { 0u, 1u, 2u, 12345u, 0x51F15EEDu, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu } )
		std::printf( "hash %u %u\n", v, model::HashInt( v ) );
	for( double rate : { controls::SlipRate( 1.0f ), controls::SlipRate( 0.15f ), 0.5 } )
	{
		int events = 0;
		uint64_t h = 14695981039346656037ull;
		for( int64_t row : { int64_t( 0 ), int64_t( 4294967296 ), int64_t( 4294967296 ) * 3 + 17 } )
			for( int64_t k = 0; k < 200000; ++k )
			{
				const model::Slip s = model::SlipAt( row + k, rate );
				if( s.kind == model::kNoSlip )
					continue;
				++events;
				const int64_t rec[ 3 ] = { row + k, s.kind, s.rows };
				for( int64_t word : rec )
					for( int b = 0; b < 8; ++b )
					{
						h ^= static_cast< unsigned char >( static_cast< uint64_t >( word ) >> ( 8 * b ) );
						h *= 1099511628211ull;
					}
			}
		std::printf( "slips %s %d %016llx\n", num( rate ).c_str(), events, static_cast< unsigned long long >( h ) );
	}
	return 0;
}
} // namespace

int main( int argc, char** argv )
{
	const std::string mode = argc > 1 ? argv[ 1 ] : "";
	if( mode == "frames" && argc >= 4 )
		return runFrames( argv[ 2 ], argv[ 3 ] );
	if( mode == "rows" )
		return runRows( argc, argv );
	if( mode == "laws" )
		return runLaws();
	std::fprintf( stderr, "usage: refprint frames SCRIPT OUTDIR | rows ... | laws\n" );
	return 2;
}
