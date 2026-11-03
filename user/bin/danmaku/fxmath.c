/* Fixed-point trigonometry: a sine table, an arctangent table and an integer distance. No floating point in the simulation. */
#include "dm.h"
#include <math.h>

static int32_t g_sin[ANG];
static uint16_t g_atan[257];

void fxm_init(void) {
    for (int i = 0; i < ANG; i++) {
        double v = sin((double)i * 6.283185307179586 / (double)ANG) * 65536.0;
        g_sin[i] = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
    }
    for (int i = 0; i <= 256; i++) g_atan[i] = (uint16_t)(atan2((double)i, 256.0) * (double)ANG / 6.283185307179586 + 0.5);
}

fx fx_sin(int a) { return g_sin[a & (ANG - 1)]; }
fx fx_cos(int a) { return g_sin[(a + ANG / 4) & (ANG - 1)]; }

int fx_atan2(fx dy, fx dx) {
    if (dx == 0 && dy == 0) return 0;
    int64_t adx = dx < 0 ? -(int64_t)dx : dx, ady = dy < 0 ? -(int64_t)dy : dy;
    int a;
    if (adx >= ady) a = g_atan[(int)(ady * 256 / adx)];
    else a = 256 - g_atan[(int)(adx * 256 / ady)];
    if (dx < 0 && dy >= 0) a = 512 - a;
    else if (dx < 0 && dy < 0) a = 512 + a;
    else if (dx >= 0 && dy < 0) a = 1024 - a;
    return a & (ANG - 1);
}

int fx_dist(fx dx, fx dy) {
    int x = dx >> 12, y = dy >> 12;                         /* 1/16 pixel, so the squares fit in 32 bits */
    return kg_isqrt(x * x + y * y) >> 4;
}
