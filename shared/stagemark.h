/*
 * Boot progress marks: squares drawn straight into the framebuffer, in the top-left corner of the screen. A PC has no serial port,
 * and until the kernel's own console starts there is no text on screen, so a machine that stops early shows how far it got by
 * the squares. The kernel's console clears them when it starts.
 *
 * Row 0, one white square per stage:
 *   1 loader: files read, display chosen      2 loader: boot services are gone       3 loader: page tables built, about to jump
 *   4 kernel: entered (command line parsed)   5 kernel: CPU set up                  6 kernel: descriptor tables loaded
 *   7 kernel: framebuffer taken over          8 kernel: console started (the console clears the rest, so only this one is left)
 *
 * The loader's exit from the firmware is the step most likely to misbehave on a real machine, so it has rows of its own:
 *   row 1: one square per attempt, drawn before the memory map is asked for
 *   row 2: the low byte of the answer to GetMemoryMap when it was not a success (bits as squares, white = 1, grey = 0, high bit left)
 *   row 3: one square per attempt, drawn right before ExitBootServices is called
 *   row 4: the low byte of the answer to ExitBootServices (EFI_INVALID_PARAMETER is 00000010, EFI_SUCCESS all grey)
 *   row 5: first square: every attempt failed
 *
 * "stagehold=N" on the command line stops the machine right after square N (a way to test the marks, and to bisect a hang).
 * Shared by the loader (the framebuffer at its physical address) and the kernel (through the direct map); plain integer types only.
 */
#ifndef SHARED_STAGEMARK_H
#define SHARED_STAGEMARK_H

#include <stdint.h>

/* One 14x14 square at (row, col) of the mark grid, in the given 0x00RRGGBB colour (white and grey are the same in BGRX and RGBX). */
static inline void mark_square(volatile uint8_t *fb, uint32_t pitch, uint32_t width, uint32_t height, int row, int col, uint32_t colour) {
    if (!fb || row < 0 || col < 0 || row > 8 || col > 40) return;
    uint32_t x0 = 6 + (uint32_t)col * 20, y0 = 6 + (uint32_t)row * 18;
    if (x0 + 14 > width || y0 + 14 > height || pitch < (x0 + 14) * 4) return;
    for (uint32_t y = y0; y < y0 + 14; y++) {
        volatile uint32_t *line = (volatile uint32_t *)(fb + (uint64_t)y * pitch) + x0;
        for (uint32_t x = 0; x < 14; x++) line[x] = colour;
    }
}

static inline void stage_mark(volatile uint8_t *fb, uint32_t pitch, uint32_t width, uint32_t height, int n) {
    if (n >= 1 && n <= 24) mark_square(fb, pitch, width, height, 0, n - 1, 0x00FFFFFFu);
}

/* The low byte of a value as eight squares, most significant bit first: white for 1, dark grey for 0. */
static inline void mark_byte(volatile uint8_t *fb, uint32_t pitch, uint32_t width, uint32_t height, int row, uint64_t value) {
    for (int b = 0; b < 8; b++) mark_square(fb, pitch, width, height, row, b, ((value >> (7 - b)) & 1) ? 0x00FFFFFFu : 0x00303030u);
}

#endif
