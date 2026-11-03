/*
 * Network time for the kernel (docs/NTP.md).
 *
 * kernel/net/ntp.c is the client: a kernel thread that waits for an address, asks the servers, steps the clock and says so in the
 * log and in /proc/ntp. kernel/net/ntp_proto.c is everything in it that needs no machine: the packet (RFC 4330, SNTPv4), the wire
 * format of the little DNS client (RFC 1035), "host:port,host:port" lists, and the POSIX time zone rules that ntp.rtc=1 needs on
 * a PC whose clock chip keeps local time. That half includes nothing but types.h, so tests/host/test_ntp.py compiles it with gcc
 * and checks it against Python.
 */
#ifndef K_NTP_H
#define K_NTP_H

#include "types.h"

#define NTP_PORT          123
#define NTP_PKT_LEN       48
#define NTP_MIN_UNIX_S    1577836800LL     /* 2020-01-01: a server whose clock says earlier is not telling the time */
#define NTP_MAX_DELAY_MS  3000             /* a round trip longer than this is not believed */
#define NTP_AGREE_MS      1000             /* two servers may differ by this much, or neither is believed */
#define NTP_ROOT_MAX_MS   3000             /* a server further than this (root distance) from a real clock is not believed either */

/* ---- the packet ---- */

typedef struct {
    int  li, version, stratum;
    s64  t2_ms, t3_ms;                     /* the server's receive and transmit time, Unix milliseconds */
    s64  root_ms;                          /* root distance: half the root delay plus the root dispersion */
    char kiss[5];                          /* stratum 0: the kiss code ("RATE", "DENY"), else "" */
} NtpReply;

enum {
    NTP_OK = 0,
    NTP_E_SHORT,                           /* fewer than 48 bytes */
    NTP_E_MODE,                            /* not a server's reply */
    NTP_E_VERSION,                         /* not version 3 or 4 */
    NTP_E_ORIGIN,                          /* does not answer this request (a late reply to an earlier one, or a stranger's) */
    NTP_E_KISS,                            /* stratum 0: the server tells us to go away (kiss-o'-death) */
    NTP_E_STRATUM,                         /* stratum 16 or more: not synchronised */
    NTP_E_ALARM,                           /* leap indicator 3: the server's clock is not synchronised */
    NTP_E_TIMES,                           /* zero, backwards or impossible timestamps */
    NTP_E_ROOT,                            /* too far from a real clock */
};

void        ntp_unix_ms_to_ts(s64 unix_ms, u8 out[8]);        /* a 64-bit NTP timestamp (seconds since 1900, 32-bit fraction) */
s64         ntp_ts_to_unix_ms(const u8 ts[8]);                /* ... and back; a set top bit means the era before 2036, else the era after */
void        ntp_make_request(u8 pkt[NTP_PKT_LEN], s64 now_ms, u16 nonce);   /* mode 3, version 4; the nonce fills the low bits of the transmit time */
int         ntp_parse_reply(const u8 *pkt, size_t len, const u8 sent[8], NtpReply *out);   /* sent: the request's transmit timestamp (pkt + 40) */
const char *ntp_strerror(int code);
void        ntp_offset_delay(s64 t1, s64 t2, s64 t3, s64 t4, s64 *offset, s64 *delay);     /* RFC 4330 section 5; all Unix milliseconds */
bool        ntp_delay_ok(s64 delay_ms);
bool        ntp_agree(s64 offset_a, s64 offset_b);

/* ---- "host:port,host:port" ---- */

typedef struct { char host[64]; u16 port; } NtpSpec;
bool ntp_next_spec(const char **list, NtpSpec *out);          /* takes the next usable entry off the list; false at the end */
bool ntp_parse_ip4(const char *s, u32 *ip);                   /* "10.0.2.2" -> host order */

/* ---- DNS: one A query, and the addresses in the answer ---- */

#define NTP_DNS_MAX 512
size_t ntp_dns_query(u8 *buf, size_t cap, u16 id, const char *name);      /* the query message, or 0 for a name that cannot be asked */
int    ntp_dns_answers(const u8 *msg, size_t len, u16 id, u32 *ips, int max);   /* addresses found (0 = none); -1 not an answer to id; -2 the server said no */

/* ---- time zones (POSIX: STDoffset[DST[offset][,Mm.w.d[/time],Mm.w.d[/time]]]) ---- */

typedef struct { int month, week, wday, secs; } NtpRule;
typedef struct { s32 std_off, dst_off; bool has_dst; NtpRule start, end; } NtpZone;   /* offsets in seconds EAST of UTC */
bool ntp_zone_parse(const char *tz, NtpZone *z);              /* false: not understood, z is UTC */
s32  ntp_zone_offset(const NtpZone *z, s64 utc_sec);          /* the offset in force at that instant */

/* ---- the lease (kernel/net/dhcp.c) ---- */

size_t dhcp_ntp(u32 *servers, size_t max);                    /* the NTP servers (option 42) of the current lease, host order */

#endif
