/*
 * The clock that certificate dates are checked against.
 *
 * A PC's clock can be wrong (the Dell's said 2025 for a while), and a client cannot tell from the certificates. Three states, all
 * visible in TlsInfo.clock and https-get -v:
 *
 *   TLS_CLOCK_SET     a program said what time it is (tls_set_time, or a time source that calls itself trusted, e.g. after NTP)
 *   TLS_CLOCK_SYSTEM  the system clock, not earlier than the moment this library was compiled: plausible, not confirmed
 *   TLS_CLOCK_BEHIND  the system clock is earlier than the build: certainly wrong. The build time is used instead, because the
 *                     real time is at least that: a certificate that expired before the build is still refused, one issued
 *                     after it is not, and one that expired between the build and today cannot be caught (the docs say so).
 */
#include "conn.h"

static int64_t g_pinned;
static int64_t (*g_source)(void *ctx, int *trusted);
static void *g_source_ctx;

void tls_set_time(int64_t unix_seconds) { g_pinned = unix_seconds; }

void tls_set_time_source(int64_t (*fn)(void *ctx, int *trusted), void *ctx) {
    g_source = fn;
    g_source_ctx = ctx;
}

/* __DATE__ is "Nov  2 2026"; the day before is used, so that a time zone ahead of UTC on the build machine cannot make it late */
int64_t tls_build_time(void) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    unsigned m = 1;
    for (unsigned i = 0; i < 12; i++)
        if (memcmp(d, months + 3 * i, 3) == 0) m = i + 1;
    unsigned day = (unsigned)((d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0'));
    int64_t year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    return asn1_days_from_civil(year, m, day) * 86400 - 86400;
}

int tls_clock(int64_t *now) {
    int64_t t;
    if (g_pinned) {
        if (now) *now = g_pinned;
        return TLS_CLOCK_SET;
    }
    if (g_source) {
        int trusted = 0;
        t = g_source(g_source_ctx, &trusted);
        if (trusted) {
            if (now) *now = t;
            return TLS_CLOCK_SET;
        }
    } else {
        t = tls_os_time();
    }
    int64_t floor = tls_build_time();
    if (t < floor) {
        if (now) *now = floor;
        return TLS_CLOCK_BEHIND;
    }
    if (now) *now = t;
    return TLS_CLOCK_SYSTEM;
}

int clock_now(int64_t *now) { return tls_clock(now); }

const char *tls_clock_name(int state) {
    switch (state) {
    case TLS_CLOCK_SET: return "set by the program";
    case TLS_CLOCK_SYSTEM: return "system clock (not confirmed)";
    case TLS_CLOCK_BEHIND: return "system clock is behind (earlier than this program's build): not trusted";
    default: return "unknown";
    }
}
