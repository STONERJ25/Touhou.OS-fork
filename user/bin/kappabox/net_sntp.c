/*
 * sntp and ntpdate: ask an NTP server (RFC 4330, simple mode) for the time, show the offset, and with -s (always, for ntpdate)
 * step the system clock through SYS_settime. The system clock is UTC; the time zone (tz=) only affects how it is shown. With
 * -w the clock chip is written too, in local time if the machine was booted with rtc=local.
 *
 * The kernel asks for the time by itself after boot (kernel/net/ntp.c, docs/NTP.md); sntp -S shows what it did (/proc/ntp) and
 * sntp -n makes it ask again now (root).
 * A name with AAAA and A records is asked over both families in turn (IPv6 first when this machine has a global address); a literal may be
 * written "fec0::2" or "[fec0::2]".
 */
#include "netutil.h"
#include <netconn.h>

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

/* /proc/ntp is "key: value" lines. Prints the whole file, or says why there is none. */
static int ntp_show_status(void) {
    FILE *f = fopen("/proc/ntp", "r");
    if (!f) { kb_error("/proc/ntp: %s (this kernel has no network time client)", strerror(errno)); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
    return 0;
}

static long ntp_cycles(void) {                                      /* rounds the kernel's client has finished, -1 if unknown */
    FILE *f = fopen("/proc/ntp", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "cycles:", 7)) { v = atol(line + 7); break; }
    if (f) fclose(f);
    return v;
}

/* sntp -n: tell the kernel's client to ask now, wait for the round to end, show the result. */
static int ntp_sync_now(void) {
    long before = ntp_cycles();
    if (before < 0) { kb_error("/proc/ntp: the kernel's network time client is off or missing (ntp=off?)"); return 1; }
    int fd = open("/proc/ntp", O_WRONLY);
    if (fd < 0 || write(fd, "sync\n", 5) != 5) { kb_error("cannot ask the kernel to sync: %s (only root may)", strerror(errno)); if (fd >= 0) close(fd); return 1; }
    close(fd);
    long start = nu_now_ms();
    while (nu_now_ms() - start < 90000) {                           /* a round with dead servers takes about as long as that */
        if (ntp_cycles() > before) break;
        usleep(200000);
    }
    return ntp_show_status();
}

static int sntp_main(int argc, char **argv, int set_default) {
    int set = set_default, write_rtc = 0, timeout_s = 5, query_only = 0, opt;
    unsigned port = 123;
    optind = 1;
    while ((opt = getopt(argc, argv, "swqSnp:t:")) != -1) {
        switch (opt) {
        case 's': set = 1; break;
        case 'w': write_rtc = 1; set = 1; break;
        case 'q': query_only = 1; break;
        case 'S': return ntp_show_status();
        case 'n': return ntp_sync_now();
        case 'p': port = (unsigned)atoi(optarg); break;
        case 't': timeout_s = atoi(optarg); break;
        default: kb_error("usage: %s [-s] [-w] [-q] [-p PORT] [-t SECONDS] SERVER | -S | -n", kb_name); return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: %s [-s] [-w] [-q] [-p PORT] [-t SECONDS] SERVER | -S | -n", kb_name); return 2; }
    if (query_only) set = write_rtc = 0;
    const char *server = argv[optind];
    /* every address of the name (IPv6 and IPv4, the families this machine has), interleaved by family: the three attempts go to the
     * first, second and third of them in turn, so a family that is broken here costs one attempt, not all three */
    char name[256];
    snprintf(name, sizeof name, "%s", server);
    size_t nl = strlen(name);
    if (nl >= 2 && name[0] == '[' && name[nl - 1] == ']') { name[nl - 1] = 0; memmove(name, name + 1, nl - 1); }
    char service[8];
    snprintf(service, sizeof service, "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int rc = getaddrinfo(name, service, &hints, &res);
    if (rc == EAI_NONAME || rc == EAI_FAIL) { hints.ai_flags = 0; if (getaddrinfo(name, service, &hints, &res) == 0) rc = 0; }
    if (rc != 0 || !res) { kb_error("%s: %s", server, gai_strerror(rc)); return 1; }
    const struct addrinfo *addrs[NETCONN_MAX];
    int naddrs = 0;
    for (const struct addrinfo *ai = res; ai && naddrs < NETCONN_MAX; ai = ai->ai_next) addrs[naddrs++] = ai;
    netconn_interleave(addrs, naddrs);
    int fd = -1;
    char shown_ip[64] = "";

    unsigned char req[48], rep[128];
    for (int attempt = 0; attempt < 3; attempt++) {
        const struct addrinfo *ai = addrs[attempt % naddrs];
        if (fd >= 0) close(fd);
        fd = socket(ai->ai_family, SOCK_DGRAM, 0);
        if (fd < 0) { kb_error("socket: %s", strerror(errno)); freeaddrinfo(res); return 1; }
        if (ai->ai_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, shown_ip, sizeof shown_ip);
        else inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, shown_ip, sizeof shown_ip);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) { if (attempt + 1 < 3 && naddrs > 1) continue; kb_error("%s: %s", server, strerror(errno)); freeaddrinfo(res); return 1; }
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
            if (n < 48) {
                if (n < 0 && errno == ECONNREFUSED) {
                    if (naddrs > 1 && attempt + 1 < 3) break;                 /* the next address may have one */
                    kb_error("%s: connection refused (is an NTP server running there?)", server);
                    freeaddrinfo(res);
                    return 1;
                }
                continue;
            }
            int mode = rep[0] & 7, li = rep[0] >> 6, stratum = rep[1];
            if ((mode != 4 && mode != 5) || memcmp(rep + 24, req + 40, 8)) continue;       /* not an answer to our request */
            if (li == 3 || stratum == 0 || stratum > 15) { kb_error("%s: the server's clock is not synchronised (stratum %d)", server, stratum); return 1; }
            long long t2 = get_ntp(rep + 32), t3 = get_ntp(rep + 40);
            long long offset = ((t2 - t1) + (t3 - t4)) / 2, delay = (t4 - t1) - (t3 - t2);
            printf("server %s, stratum %d, offset %+lld.%03lld, delay %lld.%03lld\n", shown_ip, stratum, offset / 1000, (offset < 0 ? -offset : offset) % 1000,
                   delay / 1000, (delay < 0 ? -delay : delay) % 1000);
            freeaddrinfo(res);
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
    freeaddrinfo(res);
    kb_error("no answer from %s", server);
    return 1;
}

APPLET(sntp) { return sntp_main(argc, argv, 0); }
APPLET(ntpdate) { return sntp_main(argc, argv, 1); }
