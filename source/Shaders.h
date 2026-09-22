#pragma once

/**
	The GLSL, as string literals.

	`#version 410 core` because that is macOS's ceiling and the rest of the
	fleet already sits there.

	## One fragment shader, one full-screen triangle

	There is no geometry: the vertex shader emits one triangle covering the
	viewport from `gl_VertexID`, so the only things bound at draw time are a VAO
	and the font texture. All four instruments branch on `uType` inside `main`.

	## It computes no layout

	Every rectangle, radius and angle arrives in **pixels**, already decided by
	`Needle.cpp`. This shader turns those into coverage and nothing else. See
	`Render.h` for why that is the arrangement -- briefly, it is what lets a
	pixel check probe coordinates the plugin itself produced, and what keeps the
	layout identical on a rasteriser this was never run on.

	## Coverage, and why it is linear

	`cov( d ) = clamp( 0.5 - d, 0, 1 )` over a signed distance in pixels: a
	one-pixel linear ramp across every edge. Not `smoothstep`, which would give
	an edge a soft shoulder two pixels wide and put every probe within two
	pixels of an edge at the mercy of the GPU's `smoothstep`. With this, a point
	two pixels inside a feature has coverage of exactly 1.0 on any hardware,
	which is the property `ndtest --pixels` rests on.

	## Reserved words

	`patch`, `sample`, `input`, `output`, `filter`, `common`, `active`, `half`,
	`layout` and `flat` are GLSL keywords. `patch` cost graticule a build; the
	local for a fractional part here is `fp` rather than `fract` for the same
	family of reason.

	## Randomness

	Wear is an integer PCG hash, never `fract( sin( x ) )`: the latter depends
	on the driver's `sin`, so two GPUs disagree about which specks are where and
	no offline mirror can agree with either.

	## Two adjacent literals

	MSVC caps one string literal at about 16 KB (C2026), so the fragment shader
	is two raw strings side by side. `tools/verify.sh` joins adjacent literals
	before handing the result to `glslc`; if it ever stops doing so the shader
	check reports a syntax error in the middle of a function.
*/
namespace needle
{
extern const char* const kVertexShader;
extern const char* const kFragmentShader;

} // namespace needle
