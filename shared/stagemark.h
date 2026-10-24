/*
 * Boot progress marks: one white square per stage, side by side in the top-left corner of the screen, drawn straight into the
 * framebuffer. A PC has no serial port, and until the kernel's own console starts there is no text on screen, so a machine that stops
 * early shows how far it got by the number of squares. The kernel's console clears them when it starts.
 *
 *   1 loader: files read, display chosen      2 loader: boot services are gone       3 loader: page tables built, about to jump
 *   4 kernel: entered (command line parsed)   5 kernel: CPU set up                  6 kernel: descriptor tables loaded
 *   (then the console starts and the squares disappear under the banner)
 *
 * "stagehold=N" on the command line stops the machine right after square N (a way to test the marks, and to bisect a hang).
 * Shared by the loader (the framebuffer at its physical address) and the kernel (through the direct map); plain integer types only.
 */
#ifndef SHARED_STAGEMARK_H
#define SHARED_STAGEMARK_H

#include <stdint.h>

static inline void stage_mark(volatile uint8_t *fb, uint32_t pitch, uint32_t width, uint32_t height, int n) {
    if (!fb || n < 1 || n > 24) return;
    uint32_t x0 = 6 + (uint32_t)(n - 1) * 20;
    if (x0 + 14 > width || 22 > height || pitch < (x0 + 14) * 4) return;
    for (uint32_t y = 6; y < 20; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(fb + (uint64_t)y * pitch) + x0;
        for (uint32_t x = 0; x < 14; x++) row[x] = 0x00FFFFFFu;        /* white is white in BGRX and RGBX alike */
    }
}

#endif
