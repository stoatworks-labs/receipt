#include "Printer.h"

#include "Model.h"

#include <algorithm>

namespace receiptfx
{

void Printer::Configure( const Settings& s )
{
	const bool newHead = s.dots != settings.dots || theta.size() != static_cast< size_t >( s.dots );
	settings           = s;
	if( !newHead )
		return;
	theta.assign( static_cast< size_t >( s.dots ), 0.0 );
	energy.assign( static_cast< size_t >( s.dots ), 0.0 );
	for( auto& row : error )
		row.assign( static_cast< size_t >( s.dots + 2 * kPad ), 0.0 );
	errorRow = 0;
}

void Printer::ResetHeat()
{
	std::fill( theta.begin(), theta.end(), 0.0 );
}

void Printer::ResetDither()
{
	for( auto& row : error )
		std::fill( row.begin(), row.end(), 0.0 );
	errorRow = 0;
}

void Printer::Dither( const float* tones, int64_t dataRow, uint8_t* bits )
{
	const int N = settings.dots;

	if( settings.dither == model::kBayer )
	{
		const int phase = static_cast< int >( ( ( dataRow % 8 ) + 8 ) % 8 );
		for( int x = 0; x < N; ++x )
		{
			const double dark      = 1.0 - static_cast< double >( tones[ x ] );
			const double threshold = ( model::kBayer8[ phase ][ x & 7 ] + 0.5 ) / 64.0;
			bits[ x ]              = dark > threshold ? 1 : 0;
		}
		return;
	}

	double* here  = error[ errorRow ].data() + kPad;
	double* next  = error[ ( errorRow + 1 ) % 3 ].data() + kPad;
	double* after = error[ ( errorRow + 2 ) % 3 ].data() + kPad;
	const bool diffuse = ( settings.perturb & model::kPerturbNoDiffusion ) == 0;

	for( int x = 0; x < N; ++x )
	{
		const double v = ( 1.0 - static_cast< double >( tones[ x ] ) ) + here[ x ];
		const uint8_t b = v >= 0.5 ? 1 : 0;
		bits[ x ]       = b;
		if( !diffuse )
			continue;
		const double e = v - b;
		if( settings.dither == model::kFloydSteinberg )
		{
			here[ x + 1 ] += e * ( 7.0 / 16.0 );
			next[ x - 1 ] += e * ( 3.0 / 16.0 );
			next[ x ] += e * ( 5.0 / 16.0 );
			next[ x + 1 ] += e * ( 1.0 / 16.0 );
		}
		else
		{
			const double eighth = e * 0.125;
			here[ x + 1 ] += eighth;
			here[ x + 2 ] += eighth;
			next[ x - 1 ] += eighth;
			next[ x ] += eighth;
			next[ x + 1 ] += eighth;
			after[ x ] += eighth;
		}
	}

	//This row's buffer is spent: it becomes the one after next.
	std::fill( error[ errorRow ].begin(), error[ errorRow ].end(), 0.0 );
	errorRow = ( errorRow + 1 ) % 3;
}

void Printer::Strobe( const uint8_t* bits, float* density )
{
	const int N      = settings.dots;
	const double c   = ( settings.perturb & model::kPerturbNoCarry ) ? 0.0 : settings.carry;
	const double e0  = settings.energy;
	const double h   = settings.history;
	const int blocks = std::clamp( settings.blocks, 1, N );

	//History control: a firing heater's strobe is shortened by the heat the
	//firmware predicts is still in it.
	for( int x = 0; x < N; ++x )
		energy[ x ] = bits[ x ] ? std::max( 0.0, e0 - h * c * theta[ x ] ) : 0.0;

	//The budget: each block is strobed on its own, and sags with its count.
	if( ( settings.perturb & model::kPerturbNoBudget ) == 0 )
		for( int k = 0; k < blocks; ++k )
		{
			const int x0 = static_cast< int >( static_cast< int64_t >( k ) * N / blocks );
			const int x1 = static_cast< int >( static_cast< int64_t >( k + 1 ) * N / blocks );
			int firing   = 0;
			for( int x = x0; x < x1; ++x )
				firing += bits[ x ];
			if( firing <= model::kBudgetDots )
				continue;
			const double scale = model::kBudgetDots / firing;
			for( int x = x0; x < x1; ++x )
				energy[ x ] *= scale;
		}

	//The heat, then the paper under each heater.
	for( int x = 0; x < N; ++x )
		theta[ x ] = c * theta[ x ] + energy[ x ];

	const double span = model::kSaturation - model::kThreshold;
	for( int x = 0; x < N; ++x )
	{
		const double left  = x > 0 ? theta[ x - 1 ] : 0.0;
		const double right = x + 1 < N ? theta[ x + 1 ] : 0.0;
		const double q     = theta[ x ] + model::kSpread * ( left + right );
		const double t     = std::clamp( ( q - model::kThreshold ) / span, 0.0, 1.0 );
		density[ x ]       = static_cast< float >( t * t * ( 3.0 - 2.0 * t ) );
	}
}

} // namespace receiptfx
