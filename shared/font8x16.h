/* 8x16 bitmap font shared by the kernel console and the userland graphics library. */
#ifndef SHARED_FONT8X16_H
#define SHARED_FONT8X16_H

#define FONT_W 8
#define FONT_H 16

/* Returns 16 bytes, one per scanline, MSB = leftmost pixel. Unknown code points give a hollow box. */
const unsigned char *font8x16_glyph(unsigned cp);

#endif
