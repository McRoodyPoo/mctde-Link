// DSFont.h -- exact Dark Souls bitmap font renderer for the net overlay.
//
// Loads the launcher-generated glyph cache (mctde_dsfont.bin: the game's own DSFont24 UI font,
// English + JP/KR/TC merged for Steam-name coverage, 10,730 glyphs) and blits scaled, outlined,
// alpha-composited text into a 32-bit BGRA (0xAARRGGBB) buffer. No GDI. Used when [Overlay]
// UseDsFont=1; the legacy GDI/Tahoma path stays as fallback.
#pragma once
#include <windows.h>
#include <cstdint>

// Load the blob once. Safe to call repeatedly (no-op once loaded). Returns Ready().
bool DSFont_Load(const wchar_t* path);
bool DSFont_Ready();

// Native design cell height (px) the blob was authored at (maxH, ~29). Scale = pxHeight / this.
int  DSFont_CellHeight();

// Force the exact render scale (>0), bypassing pixel-height rounding, so the overlay can match the
// game's UI scale precisely. Pass 0 to go back to pxHeight-derived scaling.
void DSFont_SetScale(float s);

// true = bilinear (matches the game's filtered UI font), false = point/nearest (hard pixels).
void DSFont_SetSmooth(bool s);

// The height the font actually renders at for a requested pixel height (snapped to an integer scale
// so it stays crisp). Size overlay layout to this in Classic Font mode.
int  DSFont_SnapHeight(int pxHeight);

// Advance width (px) of a UTF-8 / wide run at the given target pixel height.
int  DSFont_MeasureUTF8(const char* s, int len, int pxHeight);
int  DSFont_MeasureW(const wchar_t* s, int len, int pxHeight);

// Blit into a top-down BGRA buffer (stride in pixels). (x,y) is the top-left of the text cell.
// Draws an optional outline (outlineRadius px in outlineColor) then the fill. Alpha-composites
// (src-over) so partial coverage blends with whatever is already in the buffer.
void DSFont_DrawUTF8(uint32_t* bits, int W, int H, int stride,
                     int x, int y, const char* s, int len,
                     COLORREF fill, int pxHeight,
                     COLORREF outline, int outlineRadius);
void DSFont_DrawW(uint32_t* bits, int W, int H, int stride,
                  int x, int y, const wchar_t* s, int len,
                  COLORREF fill, int pxHeight,
                  COLORREF outline, int outlineRadius);
