#pragma once

/**
	The VFD's fragment shader (source/VfdShaders.cpp). One raw string: it is
	well under MSVC's 16 KB literal cap, and tools/verify.sh's extractor joins
	adjacent literals anyway. The vertex shader is needle's kVertexShader.

	Same rules as Shaders.h: coverage is cov(), a one-pixel linear ramp, so a
	probe two pixels inside a segment reads exactly the segment's colour on any
	rasteriser -- except for the wires, which are sub-pixel and use the exact
	box overlap instead (wireCover). No reserved words (patch, sample, input,
	output, filter, common, active, half, layout, flat) as identifiers.
*/
namespace needle::vfd
{
extern const char* const kVfdFragmentShader;
} // namespace needle::vfd
