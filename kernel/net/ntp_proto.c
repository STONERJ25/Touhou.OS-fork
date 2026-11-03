/*
 * Network time, the half that needs no machine (see kernel/include/ntp.h).
 *
 * Written from RFC 4330 (SNTPv4: the 48-byte packet, the offset and delay formulas), RFC 1035 (the DNS message: header, question,
 * resource records, name compression) and the POSIX description of TZ. Nothing here reads a clock, a file or a socket: the client
 * (ntp.c) hands in the times and the bytes, and tests/host/test_ntp.py does the same from Python.
 */
#include "ntp.h"

/* ------------------------------------------------------------------ timestamps */

#define NTP_UNIX_DELTA   2208988800LL      /* seconds from 1900-01-01 to 1970-01-01 */
#define NTP_ERA1_DELTA   2085978496LL      /* 2^32 - NTP_UNIX_DELTA: a timestamp whose top bit is clear is in the era that starts in 2036 */

static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32  get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

void ntp_unix_ms_to_ts(s64 unix_ms, u8 out[8]) {
    s64 sec = unix_ms / 1000, ms = unix_ms % 1000;
    if (ms < 0) { ms += 1000; sec--; }
    put32(out, (u32)(u64)(sec + NTP_UNIX_DELTA));                   /* only the low 32 bits go on the wire: the era is not sent */
    put32(out + 4, (u32)((((u64)ms) << 32) / 1000));
}

/* A timestamp has no era. Seconds with the top bit set are 1968..2036 (the era every server has been in), a clear top bit means the
 * counter has wrapped: 2036..2104. So a server's clock is read correctly from 1968 to 2104 with no help from the local one.
 * The fraction is rounded to the nearest millisecond, not truncated: a truncation would make every server look half a millisecond
 * behind (and could carry 1000 into the millisecond part, which the sum below takes in its stride). */
s64 ntp_ts_to_unix_ms(const u8 ts[8]) {
    u32 sec = get32(ts), frac = get32(ts + 4);
    s64 unix_sec = (sec & 0x80000000u) ? (s64)sec - NTP_UNIX_DELTA : (s64)sec + NTP_ERA1_DELTA;
    return unix_sec * 1000 + (s64)((((u64)frac * 1000) + 0x80000000u) >> 32);
}

/* ------------------------------------------------------------------ the packet */

void ntp_make_request(u8 pkt[NTP_PKT_LEN], s64 now_ms, u16 nonce) {
    for (int i = 0; i < NTP_PKT_LEN; i++) pkt[i] = 0;
    pkt[0] = (0 << 6) | (4 << 3) | 3;                               /* leap indicator 0, version 4, mode 3 (client) */
    ntp_unix_ms_to_ts(now_ms, pkt + 40);
    /* RFC 4330 section 3: the low-order bits of the transmit timestamp are random, so that a reply only a listener could have
     * seen cannot be forged by someone who merely knows the time. 16 bits of the fraction are 15 microseconds of error. */
    pkt[46] = (u8)(nonce >> 8);
    pkt[47] = (u8)nonce;
}

static bool ts_is_zero(const u8 *p) {
    for (int i = 0; i < 8; i++) if (p[i]) return false;
    return true;
}

int ntp_parse_reply(const u8 *p, size_t len, const u8 sent[8], NtpReply *r) {
    r->li = r->version = r->stratum = 0;
    r->t2_ms = r->t3_ms = r->root_ms = 0;
    r->kiss[0] = 0;
    if (len < NTP_PKT_LEN) return NTP_E_SHORT;
    r->li = p[0] >> 6;
    r->version = (p[0] >> 3) & 7;
    r->stratum = p[1];
    if ((p[0] & 7) != 4) return NTP_E_MODE;                         /* a server's reply: not a client's request, not a broadcast, not a control message */
    if (r->version < 3 || r->version > 4) return NTP_E_VERSION;
    for (int i = 0; i < 8; i++)
        if (p[24 + i] != sent[i]) return NTP_E_ORIGIN;              /* what the server copied back is what proves it answers THIS request */
    if (r->stratum == 0) {                                          /* kiss-o'-death: the reference identifier holds four ASCII letters */
        int n = 0;
        for (int i = 0; i < 4; i++) if (p[12 + i] >= 0x20 && p[12 + i] < 0x7F) r->kiss[n++] = (char)p[12 + i];
        r->kiss[n] = 0;
        return NTP_E_KISS;
    }
    if (r->stratum >= 16) return NTP_E_STRATUM;
    if (r->li == 3) return NTP_E_ALARM;
    if (ts_is_zero(p + 32) || ts_is_zero(p + 40)) return NTP_E_TIMES;
    r->t2_ms = ntp_ts_to_unix_ms(p + 32);
    r->t3_ms = ntp_ts_to_unix_ms(p + 40);
    if (r->t3_ms < r->t2_ms || r->t3_ms / 1000 < NTP_MIN_UNIX_S) return NTP_E_TIMES;
    /* root delay is a signed 16.16 number of seconds, root dispersion an unsigned one */
    s64 delay_ms = ((s64)(s32)get32(p + 4) * 1000) >> 16, disp_ms = (s64)(((u64)get32(p + 8) * 1000) >> 16);
    r->root_ms = (delay_ms > 0 ? delay_ms / 2 : 0) + disp_ms;
    if (r->root_ms > NTP_ROOT_MAX_MS) return NTP_E_ROOT;
    return NTP_OK;
}

const char *ntp_strerror(int code) {
    switch (code) {
    case NTP_OK:        return "ok";
    case NTP_E_SHORT:   return "an answer shorter than an NTP packet";
    case NTP_E_MODE:    return "not a server's reply";
    case NTP_E_VERSION: return "an NTP version that is not 3 or 4";
    case NTP_E_ORIGIN:  return "an answer to another request";
    case NTP_E_KISS:    return "it asks us to stay away (kiss-o'-death)";
    case NTP_E_STRATUM: return "its clock is not synchronised (stratum 16)";
    case NTP_E_ALARM:   return "its clock is not synchronised (leap indicator 3)";
    case NTP_E_TIMES:   return "impossible timestamps";
    case NTP_E_ROOT:    return "too far from a real clock (root distance)";
    default:            return "unknown";
    }
}

void ntp_offset_delay(s64 t1, s64 t2, s64 t3, s64 t4, s64 *offset, s64 *delay) {
    *offset = ((t2 - t1) + (t3 - t4)) / 2;                          /* how far the local clock is BEHIND the server's */
    *delay = (t4 - t1) - (t3 - t2);                                 /* the time on the wire: the round trip less the server's own think time */
}

/* A negative delay by a few milliseconds is rounding (every time here is a whole millisecond); more means the clock moved under us. */
bool ntp_delay_ok(s64 delay_ms) { return delay_ms >= -10 && delay_ms <= NTP_MAX_DELAY_MS; }

bool ntp_agree(s64 a, s64 b) {
    s64 d = a - b;
    return (d < 0 ? -d : d) <= NTP_AGREE_MS;
}

/* ------------------------------------------------------------------ "host:port,host:port" */

bool ntp_parse_ip4(const char *s, u32 *ip) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        u32 part = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { part = part * 10 + (u32)(*s++ - '0'); if (++digits > 3 || part > 255) return false; }
        v = v << 8 | part;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *ip = v;
    return true;
}

bool ntp_next_spec(const char **list, NtpSpec *out) {
    const char *s = *list;
    for (;;) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) { *list = s; return false; }
        const char *end = s, *colon = NULL;
        while (*end && *end != ',') { if (*end == ':') colon = end; end++; }
        const char *stop = end;                                     /* (the entry ends before the spaces that come after it) */
        while (stop > s && stop[-1] == ' ') stop--;
        if (colon && colon >= stop) colon = NULL;
        size_t hl = (size_t)((colon ? colon : stop) - s);
        u32 port = NTP_PORT;
        bool ok = hl > 0 && hl < sizeof out->host;
        if (ok && colon) {
            port = 0;
            ok = colon + 1 < stop;
            for (const char *q = colon + 1; ok && q < stop; q++) {
                if (*q < '0' || *q > '9') ok = false;
                else { port = port * 10 + (u32)(*q - '0'); if (port > 65535) ok = false; }
            }
            if (port == 0) ok = false;
        }
        *list = end;
        if (ok) {
            for (size_t i = 0; i < hl; i++) out->host[i] = s[i];
            out->host[hl] = 0;
            out->port = (u16)port;
            return true;
        }
        s = end;                                                    /* an entry that makes no sense is skipped, not fatal */
    }
}

/* ------------------------------------------------------------------ DNS */

size_t ntp_dns_query(u8 *buf, size_t cap, u16 id, const char *name) {
    size_t n = 0;
    while (name[n]) n++;
    if (n == 0 || n > 253 || cap < 12 + n + 2 + 4) return 0;
    for (int i = 0; i < 12; i++) buf[i] = 0;
    buf[0] = (u8)(id >> 8); buf[1] = (u8)id;
    buf[2] = 0x01;                                                  /* recursion desired */
    buf[5] = 1;                                                     /* one question */
    size_t pos = 12;
    const char *s = name;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') {
            char c = *dot;
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return 0;
            dot++;
        }
        size_t l = (size_t)(dot - s);
        if (l == 0 || l > 63) return 0;                             /* an empty label ("a..b", ".a") cannot be asked */
        buf[pos++] = (u8)l;
        for (size_t i = 0; i < l; i++) buf[pos++] = (u8)s[i];
        s = dot;
        if (*s == '.') s++;                                         /* (a trailing dot ends the name) */
    }
    buf[pos++] = 0;
    buf[pos++] = 0; buf[pos++] = 1;                                 /* type A */
    buf[pos++] = 0; buf[pos++] = 1;                                 /* class IN */
    return pos;
}

/* Steps over a name, which is a run of labels ending in a zero byte or in a two-byte pointer. Where a pointer leads does not matter here. */
static bool skip_name(const u8 *m, size_t len, size_t *pos) {
    size_t p = *pos;
    for (int guard = 0; guard < 130; guard++) {
        if (p >= len) return false;
        u8 b = m[p];
        if (b == 0) { *pos = p + 1; return true; }
        if ((b & 0xC0) == 0xC0) { if (p + 1 >= len) return false; *pos = p + 2; return true; }
        if (b & 0xC0) return false;                                 /* the label types 01 and 10 do not exist */
        p += 1 + (size_t)b;
    }
    return false;
}

int ntp_dns_answers(const u8 *m, size_t len, u16 id, u32 *ips, int max) {
    if (len < 12) return -1;
    u16 rid = (u16)(m[0] << 8 | m[1]), flags = (u16)(m[2] << 8 | m[3]);
    u16 qd = (u16)(m[4] << 8 | m[5]), an = (u16)(m[6] << 8 | m[7]);
    if (rid != id || !(flags & 0x8000)) return -1;
    if (flags & 0x000F) return -2;                                  /* NXDOMAIN, SERVFAIL, REFUSED ... */
    size_t pos = 12;
    for (u16 i = 0; i < qd; i++) {
        if (!skip_name(m, len, &pos) || pos + 4 > len) return 0;
        pos += 4;
    }
    int n = 0;
    for (u16 i = 0; i < an; i++) {
        if (!skip_name(m, len, &pos) || pos + 10 > len) break;
        u16 type = (u16)(m[pos] << 8 | m[pos + 1]), cls = (u16)(m[pos + 2] << 8 | m[pos + 3]), rdlen = (u16)(m[pos + 8] << 8 | m[pos + 9]);
        pos += 10;
        if (pos + rdlen > len) break;
        if (type == 1 && cls == 1 && rdlen == 4 && n < max) ips[n++] = get32(m + pos);      /* (the CNAMEs in front of the A records need no following: the server did that) */
        pos += rdlen;
    }
    return n;
}

/* ------------------------------------------------------------------ time zones */

static s64 days_from_civil(s64 y, unsigned m, unsigned d) {         /* days since 1970-01-01 of a calendar date (Hinnant's algorithm) */
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

static s64 year_of_days(s64 days) {
    s64 z = days + 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    return (s64)yoe + era * 400 + (mp >= 10);
}

static bool is_leap(s64 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static bool parse_zone_name(const char **p) {                       /* three or more letters, or <anything> */
    const char *s = *p;
    int n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') { s++; n++; }
        if (*s != '>') return false;
        s++;
    } else {
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) { s++; n++; }
    }
    if (n < 3) return false;
    *p = s;
    return true;
}

static bool parse_hms(const char **p, s32 *out) {                   /* [+-]hh[:mm[:ss]] in seconds */
    const char *s = *p;
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return false;
    s32 v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        s32 n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); if (n > 1000) return false; }
        v[i] = n;
        if (*s != ':' || i == 2) break;
        s++;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *p = s;
    return true;
}

static bool parse_int(const char **p, int *out) {
    const char *s = *p;
    if (*s < '0' || *s > '9') return false;
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); if (n > 1000) return false; }
    *out = n;
    *p = s;
    return true;
}

static bool parse_rule(const char **p, NtpRule *r) {                /* Mm.w.d[/time]; the Julian forms are not supported (the C library does not either) */
    const char *s = *p;
    r->secs = 7200;
    if (*s++ != 'M') return false;
    if (!parse_int(&s, &r->month) || *s++ != '.') return false;
    if (!parse_int(&s, &r->week) || *s++ != '.') return false;
    if (!parse_int(&s, &r->wday)) return false;
    if (*s == '/') {
        s++;
        s32 t;
        if (!parse_hms(&s, &t)) return false;
        r->secs = t;
    }
    *p = s;
    return r->month >= 1 && r->month <= 12 && r->week >= 1 && r->week <= 5 && r->wday >= 0 && r->wday <= 6;
}

bool ntp_zone_parse(const char *tz, NtpZone *z) {
    z->std_off = z->dst_off = 0;
    z->has_dst = false;
    z->start = (NtpRule){ 0, 0, 0, 0 };
    z->end = z->start;
    if (!tz) return false;
    const char *s = tz;
    s32 off;
    if (!parse_zone_name(&s) || !parse_hms(&s, &off)) return false;
    z->std_off = -off;                                              /* POSIX counts hours WEST of Greenwich */
    if (*s && *s != ',') {
        if (!parse_zone_name(&s)) { z->std_off = 0; return false; }
        z->has_dst = true;
        z->dst_off = z->std_off + 3600;
        if (*s && *s != ',' && parse_hms(&s, &off)) z->dst_off = -off;
        z->start = (NtpRule){ 3, 2, 0, 7200 };                      /* no rule given: the United States' (second Sunday of March, first of November) */
        z->end = (NtpRule){ 11, 1, 0, 7200 };
        if (*s == ',') {
            s++;
            if (!parse_rule(&s, &z->start) || *s++ != ',' || !parse_rule(&s, &z->end) || *s) {
                z->std_off = z->dst_off = 0;
                z->has_dst = false;
                return false;
            }
        } else if (*s) {
            z->std_off = z->dst_off = 0;
            z->has_dst = false;
            return false;
        }
    } else if (*s) {
        z->std_off = 0;
        return false;
    }
    return true;
}

/* The UTC instant at which `r` takes effect in `year`, given the offset in force just before it. */
static s64 rule_instant(s64 year, const NtpRule *r, s32 offset_before) {
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    s64 first = days_from_civil(year, (unsigned)r->month, 1);
    int first_wday = (int)((first + 4) % 7);                        /* 1970-01-01 was a Thursday */
    if (first_wday < 0) first_wday += 7;
    int day = 1 + (r->wday - first_wday + 7) % 7 + (r->week - 1) * 7;
    int dim = mdays[r->month - 1] + (r->month == 2 && is_leap(year));
    while (day > dim) day -= 7;                                     /* week 5 means "the last one in the month" */
    return (first + day - 1) * 86400 + r->secs - offset_before;
}

s32 ntp_zone_offset(const NtpZone *z, s64 utc_sec) {
    if (!z->has_dst) return z->std_off;
    s64 local = utc_sec + z->std_off, days = local / 86400;
    if (local % 86400 < 0) days--;
    s64 y = year_of_days(days);
    s64 s = rule_instant(y, &z->start, z->std_off), e = rule_instant(y, &z->end, z->dst_off);
    bool in_dst = s < e ? (utc_sec >= s && utc_sec < e) : !(utc_sec >= e && utc_sec < s);        /* southern hemisphere: the summer wraps the new year */
    return in_dst ? z->dst_off : z->std_off;
}
