/*
 * sntp and ntpdate: ask an NTP server (RFC 4330, simple mode) for the time, show the offset, and with -s (always, for ntpdate)
 * step the system clock through SYS_settime. The system clock is UTC; the time zone (tz=) only affects how it is shown. With
 * -w the clock chip is written too, in local time if the machine was booted with rtc=local.
 */
#include "netutil.h"

#define NTP_UNIX_DELTA 2208988800ull

static void put_ntp(unsigned char *p, long long ms) {               /* unix milliseconds -> 64-bit NTP timestamp */
    unsigned long long sec = (unsigned long long)(ms / 1000) + NTP_UNIX_DELTA;
    unsigned long long frac = ((unsigned long long)(ms % 1000) << 32) / 1000;
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(sec >> (24 - 8 * i));
    for (int i = 0; i < 4; i++) p[4 + i] = (unsigned char)(frac >> (24 - 8 * i));
}

static long long get_ntp(const unsigned char *p) {                  /* NTP timestamp -> unix milliseconds */
    unsigned long long sec = 0, frac = 0;
    for (int i = 0; i < 4; i++) sec = sec << 8 | p[i];
    for (int i = 0; i < 4; i++) frac = frac << 8 | p[4 + i];
    return (long long)(sec - NTP_UNIX_DELTA) * 1000 + (long long)((frac * 1000) >> 32);
}

static int rtc_is_local(void) {
    FILE *f = fopen("/proc/cmdline", "r");
    char line[512] = "";
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    return strstr(line, "rtc=local") != NULL;
}

static int sntp_main(int argc, char **argv, int set_default) {
    int set = set_default, write_rtc = 0, timeout_s = 5, query_only = 0, opt;
    unsigned port = 123;
    optind = 1;
    while ((opt = getopt(argc, argv, "swqp:t:")) != -1) {
        switch (opt) {
        case 's': set = 1; break;
        case 'w': write_rtc = 1; set = 1; break;
        case 'q': query_only = 1; break;
        case 'p': port = (unsigned)atoi(optarg); break;
        case 't': timeout_s = atoi(optarg); break;
        default: kb_error("usage: %s [-s] [-w] [-q] [-p PORT] [-t SECONDS] SERVER", kb_name); return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: %s [-s] [-w] [-q] [-p PORT] [-t SECONDS] SERVER", kb_name); return 2; }
    if (query_only) set = write_rtc = 0;
    const char *server = argv[optind];
    uint32_t ip;
    if (!nu_resolve(server, &ip)) return 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { kb_error("socket: %s", strerror(errno)); return 1; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { kb_error("%s: %s", server, strerror(errno)); return 1; }

    unsigned char req[48], rep[128];
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(req, 0, sizeof req);
        req[0] = 0x23;                                                /* leap 0, version 4, mode 3 (client) */
        long long t1 = gap_time_ms();
        put_ntp(req + 40, t1);                                        /* the server echoes it back as the origin timestamp */
        if (send(fd, req, sizeof req, 0) < 0) { kb_error("send: %s", strerror(errno)); return 1; }
        long start = nu_now_ms();
        for (;;) {
            long left = timeout_s * 1000L - (nu_now_ms() - start);
            if (left <= 0) break;
            struct gpollfd p = { fd, POLLIN, 0 };
            int r = poll(&p, 1, (int)left);
            if (r <= 0) break;
            ssize_t n = recv(fd, rep, sizeof rep, 0);
            long long t4 = gap_time_ms();
            if (n < 48) { if (n < 0 && errno == ECONNREFUSED) { kb_error("%s: connection refused (is an NTP server running there?)", server); return 1; } continue; }
            int mode = rep[0] & 7, li = rep[0] >> 6, stratum = rep[1];
            if ((mode != 4 && mode != 5) || memcmp(rep + 24, req + 40, 8)) continue;       /* not an answer to our request */
            if (li == 3 || stratum == 0 || stratum > 15) { kb_error("%s: the server's clock is not synchronised (stratum %d)", server, stratum); return 1; }
            long long t2 = get_ntp(rep + 32), t3 = get_ntp(rep + 40);
            long long offset = ((t2 - t1) + (t3 - t4)) / 2, delay = (t4 - t1) - (t3 - t2);
            printf("server %s, stratum %d, offset %+lld.%03lld, delay %lld.%03lld\n", nu_ip(ip), stratum, offset / 1000, (offset < 0 ? -offset : offset) % 1000,
                   delay / 1000, (delay < 0 ? -delay : delay) % 1000);
            if (!set) return 0;
            long long now = gap_time_ms() + offset;
            long rtc_value = 0;
            if (write_rtc) {
                time_t secs = (time_t)(now / 1000);
                rtc_value = (long)secs;
                if (rtc_is_local()) { struct tm tm; localtime_r(&secs, &tm); rtc_value += tm.tm_gmtoff; }
            }
            if (gap_settime((long)now, rtc_value) < 0) { kb_error("cannot set the clock: %s (only root may)", strerror(errno)); return 1; }
            time_t secs = (time_t)(now / 1000);
            char shown[64];
            struct tm tm;
            localtime_r(&secs, &tm);
            strftime(shown, sizeof shown, "%Y-%m-%d %H:%M:%S %Z", &tm);
            printf("the clock now reads %s%s\n", shown, write_rtc ? " (clock chip written too)" : "");
            return 0;
        }
    }
    kb_error("no answer from %s", server);
    return 1;
}

APPLET(sntp) { return sntp_main(argc, argv, 0); }
APPLET(ntpdate) { return sntp_main(argc, argv, 1); }
