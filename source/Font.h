#pragma once

#include <cstdint>
#include <vector>

/**
    A 5x7 bitmap font, carried unchanged from graticule.

    Every piece of text a meter carries is drawn from it by the fragment shader:
    the VU scale's numbers, the "VU" legend, the PPM's marks, the bargraph's
    step labels. A bitmap font is the right tool here for the same reason it was
    there -- a glyph is an exact arrangement of whole pixels at an integer
    scale, with no rasteriser, no hinting and no anti-aliasing between the table
    and the screen, so what `ndtest --pixels` asserts about a label is what a
    wall shows.

    The glyphs are written as pictures in Font.cpp so they can be read, and
    `ndtest --font` prints them back so they can be checked by eye. Printable
    ASCII only (32..126), one row per glyph line, top row first, and column 0 is
    the leftmost pixel.
*/
namespace needle::font
{
constexpr int kFirst  = 32;///< first code covered
constexpr int kCount  = 96;///< 32..127
constexpr int kWidth  = 5;
constexpr int kHeight = 7;
/// Horizontal advance: the glyph plus one blank column.
constexpr int kAdvance = kWidth + 1;

/// The glyph for `code` as seven rows of five characters, '#' for a lit pixel.
/// Anything outside the covered range is a blank glyph.
const char* const* Glyph( int code );

/// Is pixel (x, y) of glyph `code` lit?
bool Bit( int code, int x, int y );

/// The whole table as an 8-bit single-channel image, kWidth*128 wide and
/// kHeight tall, indexed by ASCII code directly: glyph for code c starts at
/// column c*kWidth. Codes below kFirst are blank so the shader never has to
/// subtract anything.
std::vector< uint8_t > Texture();

constexpr int kTextureWidth  = kWidth * 128;
constexpr int kTextureHeight = kHeight;

} // namespace needle::font
