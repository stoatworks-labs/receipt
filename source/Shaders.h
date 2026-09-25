#pragma once

/**
	The GPU half. Two passes a frame and the vertex shader:

	  sample    the host's picture -> the head's image, N dots across and
	            R_img rows down, R32F tone (0 black, 1 white): the clip fit to
	            the head per Fit, each dot the EXACT area average of the
	            source pixels under it (overlaps computed in integers, in
	            units of 1/N of a source pixel), luma over paper white where
	            the source is transparent or absent. Read back once a frame
	            for the CPU engine.
	  display   the printed paper -> the host: dots a whole number of pixels
	            a side, the receipt's torn ends, the stock and the dye by
	            optical density, Age, and the mix.

	Every read is `texelFetch` at integer coordinates computed in integers,
	so nothing here depends on a texture unit's filtering or on where a
	rasteriser's interpolated uv lands. The print itself -- dither, heat,
	budget, paper response -- is the CPU's (Printer.cpp); see there for why.
*/
namespace receiptfx::shaders
{

extern const char* const kVertex;
extern const char* const kSample;
extern const char* const kDisplay;

/// How many fragment shaders there are, for the dump and the check.
constexpr int kFragmentCount = 2;

} // namespace receiptfx::shaders
