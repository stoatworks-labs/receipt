#include "Shaders.h"

namespace receiptfx::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// sample: the head's image. Fragment ( c, i ) is dot column c of image row
// i (row 0 printed first). Its footprint in the source, in units of 1/N of a
// source pixel, is [ c U, ( c + 1 ) U ) across and [ i U, ( i + 1 ) U ) down
// for Letterbox (U = the source's width), and for Rotate (U = its height) the
// source's x runs down the paper and its top is the paper's right-hand edge:
// [ i U, ( i + 1 ) U ) along x, [ ( N - 1 - c ) U, ( N - c ) U ) down from
// the top. Every overlap is an integer, the sums of integers times 0 or 1
// are exact in float below 2^24, so a dot wholly inside black source pixels
// is exactly 0 and one wholly inside white is exactly 1.
//---------------------------------------------------------------------------
const char* const kSample = R"(#version 410 core

uniform sampler2D InputTexture;
uniform int SrcW;
uniform int SrcH;
uniform int Dots;
uniform int Unit;
uniform int Rotate;

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

//The source's tone at pixel ( x, y from the top ): luma, over paper white by
//the source's alpha; off the source is paper.
float toneAt( int x, int yTop )
{
	if( x < 0 || yTop < 0 || x >= SrcW || yTop >= SrcH )
		return 1.0;
	vec4 s = texelFetch( InputTexture, ivec2( x, SrcH - 1 - yTop ), 0 );
	return dot( s.rgb, kLuma ) * s.a + ( 1.0 - s.a );
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int u0, v0;
	if( Rotate == 0 )
	{
		u0 = p.x * Unit;
		v0 = p.y * Unit;
	}
	else
	{
		u0 = p.y * Unit;
		v0 = ( Dots - 1 - p.x ) * Unit;
	}
	int u1  = u0 + Unit;
	int v1  = v0 + Unit;
	int kx0 = u0 / Dots;
	int kx1 = ( u1 - 1 ) / Dots;
	int ky0 = v0 / Dots;
	int ky1 = ( v1 - 1 ) / Dots;

	float sum = 0.0;
	for( int ky = ky0; ky <= ky1; ++ky )
	{
		int oy    = min( ( ky + 1 ) * Dots, v1 ) - max( ky * Dots, v0 );
		float row = 0.0;
		for( int kx = kx0; kx <= kx1; ++kx )
		{
			int ox = min( ( kx + 1 ) * Dots, u1 ) - max( kx * Dots, u0 );
			row += float( ox ) * toneAt( kx, ky );
		}
		sum += float( oy ) * row;
	}
	fragColor = vec4( sum / ( float( Unit ) * float( Unit ) ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// display: output pixel ( X, Y from the top ) shows dot c = floor( ( X - X0 )
// / Pitch ) of window row w = floor( Y / Pitch ), both in integers. A window
// row maps to a row of the paper texture through RowBase (a ring in
// Printing). Rows outside the receipt, and the notches of a torn end, are
// not paper: transparent black. The paper is the stock attenuated by the
// dye's optical density, 10^-OD, with Age fading the print away from the
// tear edge and developing the background near it.
//---------------------------------------------------------------------------
const char* const kDisplay = R"(#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D PaperTexture;
uniform ivec2 Origin;
uniform int OutH;
uniform int Dots;
uniform int Pitch;
uniform int X0;
uniform int FloatPitch;
uniform float PitchF;
uniform int TexRows;
uniform int RowBase;
uniform int ReceiptTop;
uniform int ReceiptBottom;
uniform int NotchTop;
uniform int NotchBottom;
uniform vec3 Paper;
uniform vec3 Ink;
uniform float FadeAmount;
uniform float FadeRows;
uniform float HeldOD;
uniform float HeldRows;
uniform float MixAmount;

out vec4 fragColor;

const float kLog2Ten = 3.32192809488736;

//Floor division and modulo, for operands of either sign.
int idiv( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}

int imod( int a, int b )
{
	return a - b * idiv( a, b );
}

//Model.h's TearNotch: a sawtooth of period 8 dots, 0..2 rows deep.
int notch( int c )
{
	int r = imod( c, 8 );
	int d = r < 4 ? 4 - r : r - 4;
	return d / 2;
}

void main()
{
	ivec2 pix = ivec2( gl_FragCoord.xy ) - Origin;
	vec4 src  = texelFetch( InputTexture, pix, 0 );
	int yTop  = OutH - 1 - pix.y;

	int c, w;
	if( FloatPitch != 0 )
	{
		c = int( floor( ( float( pix.x ) - float( X0 ) ) / PitchF ) );
		w = int( floor( float( yTop ) / PitchF ) );
	}
	else
	{
		c = idiv( pix.x - X0, Pitch );
		w = idiv( yTop, Pitch );
	}

	vec4 receipt = vec4( 0.0 );
	bool onPaper = c >= 0 && c < Dots;
	if( onPaper )
	{
		int top    = ReceiptTop + ( NotchTop != 0 ? notch( c ) : 0 );
		int bottom = ReceiptBottom - ( NotchBottom != 0 ? notch( c ) : 0 );
		onPaper    = w >= top && w < bottom;
	}
	if( onPaper )
	{
		float D     = texelFetch( PaperTexture, ivec2( c, imod( RowBase + w, TexRows ) ), 0 ).r;
		float dist  = float( ReceiptBottom - 1 - w );
		float faded = D * ( 1.0 - FadeAmount * ( 1.0 - exp( -dist / FadeRows ) ) );
		vec3 od     = Ink * faded + vec3( HeldOD * exp( -dist / HeldRows ) );
		receipt     = vec4( Paper * exp2( -od * kLog2Ten ), 1.0 );
	}

	//The paper is a sheet: opaque whatever the clip's alpha. Off the paper is
	//transparent black, so the layer below shows round the receipt.
	fragColor = MixAmount >= 1.0 ? receipt : mix( src, receipt, MixAmount );
}
)";

} // namespace receiptfx::shaders
