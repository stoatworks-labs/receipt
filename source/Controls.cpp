#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace receiptfx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int StrobeBlocks( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), model::kStrobeBlocksMin, model::kStrobeBlocksMax );
}

double Energy( float value )
{
	return 0.5 + unit( value );
}

double Carry( float value )
{
	return 0.9 * unit( value );
}

double History( float value )
{
	return unit( value );
}

double SpeedMmPerSecond( float value )
{
	const double v = unit( value );
	return 2.0 + 248.0 * v * v;
}

double RowsPerSecond( float value )
{
	return model::kDotsPerMm * SpeedMmPerSecond( value );
}

double SlipRate( float value )
{
	const double v = unit( value );
	return 0.02 * v * v;
}

double TearMm( float value )
{
	const double v = unit( value );
	return 30.0 + 570.0 * v * v;
}

int TearRows( float value )
{
	return std::max( 1, static_cast< int >( std::lround( model::kDotsPerMm * TearMm( value ) ) ) );
}

double Age( float value )
{
	return unit( value );
}

} // namespace receiptfx::controls
