/* Procedural yin-yang orb (integer-only, 4x4 supersampled), shared by the kernel boot screen and userland. */
#ifndef SHARED_YINYANG_H
#define SHARED_YINYANG_H

#include <stdint.h>

/* Renders a size x size orb into dst (0x00RRGGBB, pitch in pixels). Pixels outside the orb get `bg`.
 * `left` colours the left half's body, `right` the right half's; the dots take the opposite colours. */
void yinyang_render(uint32_t *dst, int size, int pitch, uint32_t left, uint32_t right, uint32_t bg);

#endif
