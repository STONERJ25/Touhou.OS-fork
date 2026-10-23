/* ping: ICMP echo through an ICMP datagram socket (the kernel stamps the identifier and the checksum). */
#include "netutil.h"

static volatile int g_stop;
static void on_int(int sig) { (void)sig; g_stop = 1; }

static const char *unreach_text(int code) {
    switch (code) {
    case 0: return "Destination Net Unreachable";
    case 1: return "Destination Host Unreachable";
    case 2: return "Destination Protocol Unreachable";
    case 3: return "Destination Port Unreachable";
    case 4: return "Fragmentation needed and DF set";
    default: return "Destination Unreachable";
    }
}

APPLET(ping) {
    int count = -1, interval_ms = 1000, timeout_ms = 2000, size = 56, ttl = 0, quiet = 0;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "c:i:W:w:s:t:qn")) != -1) {
        switch (opt) {
        case 'c': count = atoi(optarg); break;
        case 'i': interval_ms = (int)(atof(optarg) * 1000); break;
        case 'W': case 'w': timeout_ms = (int)(atof(optarg) * 1000); break;
        case 's': size = atoi(optarg); break;
        case 't': ttl = atoi(optarg); break;
        case 'q': quiet = 1; break;
        case 'n': break;
        default: kb_error("usage: ping [-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] [-q] HOST"); return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: ping [-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] [-q] HOST"); return 2; }
    if (size < 8 || size > 1400) { kb_error("packet size must be 8 to 1400"); return 2; }
    if (interval_ms < 200) interval_ms = 200;
    if (timeout_ms < 100) timeout_ms = 100;
    const char *host = argv[optind];
    uint32_t ip;
    if (!nu_resolve(host, &ip)) return 2;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) { kb_error("cannot open an ICMP socket: %s", strerror(errno)); return 2; }
    if (ttl > 0) setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(ip);
    signal(SIGINT, on_int);

    printf("PING %s (%s) %d(%d) bytes of data.\n", host, nu_ip(ip), size, size + 28);
    unsigned char pkt[8 + 1400], rep[2048];
    int sent = 0, received = 0, errors = 0;
    long rtt_min = 1L << 30, rtt_max = 0, rtt_sum = 0;
    long t_start = nu_now_ms();
    for (int seq = 1; !g_stop && (count < 0 || seq <= count); seq++) {
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 8;
        pkt[6] = (unsigned char)(seq >> 8);
        pkt[7] = (unsigned char)seq;
        long t0 = nu_now_ms();
        memcpy(pkt + 8, &t0, sizeof t0);
        for (int i = (int)sizeof t0; i < size; i++) pkt[8 + i] = (unsigned char)(0x10 + i);
        if (sendto(fd, pkt, (size_t)size + 8, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            printf("ping: sendto: %s\n", strerror(errno));
            errors++;
        } else {
            sent++;
        }
        int answered = 0;
        for (;;) {
            long left = timeout_ms - (nu_now_ms() - t0);
            if (left <= 0 || g_stop) break;
            struct gpollfd p = { fd, POLLIN, 0 };
            int r = poll(&p, 1, (int)left);
            if (r <= 0) break;
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, rep, sizeof rep, 0, (struct sockaddr *)&from, &fl);
            if (n < 28) continue;
            unsigned ihl = (rep[0] & 15) * 4u;
            if (ihl + 8 > (unsigned)n) continue;
            unsigned char *ic = rep + ihl;
            uint32_t src = ntohl(from.sin_addr.s_addr);
            if (ic[0] == 0) {
                int rseq = (ic[6] << 8) | ic[7];
                if (rseq != seq) continue;                      /* a late answer to an earlier request */
                long dt = nu_now_ms() - t0;
                received++;
                rtt_sum += dt;
                if (dt < rtt_min) rtt_min = dt;
                if (dt > rtt_max) rtt_max = dt;
                if (!quiet) printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%ld ms\n", (int)(n - ihl), nu_ip(src), rseq, rep[8], dt);
                answered = 1;
                break;
            }
            if ((ic[0] == 3 || ic[0] == 11) && (unsigned)n >= ihl + 8 + 20 + 8) {
                unsigned char *inner = ic + 8;
                unsigned iihl = (inner[0] & 15) * 4u;
                if (ihl + 8 + iihl + 8 > (unsigned)n) continue;
                int rseq = (inner[iihl + 6] << 8) | inner[iihl + 7];
                if (rseq != seq) continue;
                printf("From %s icmp_seq=%d %s\n", nu_ip(src), rseq, ic[0] == 11 ? "Time to live exceeded" : unreach_text(ic[1]));
                errors++;
                answered = 1;
                break;
            }
        }
        if (!answered && !g_stop && !quiet) printf("Request timeout for icmp_seq %d\n", seq);
        if (count >= 0 && seq >= count) break;
        long wait = interval_ms - (nu_now_ms() - t0);
        while (wait > 0 && !g_stop) { usleep((useconds_t)(wait > 100 ? 100 : wait) * 1000); wait = interval_ms - (nu_now_ms() - t0); }
    }
    long total = nu_now_ms() - t_start;
    printf("\n--- %s ping statistics ---\n", host);
    int loss = sent ? (int)(100L * (sent - received) / sent) : 0;
    printf("%d packets transmitted, %d received,%s %d%% packet loss, time %ldms\n", sent, received, errors ? " +errors," : "", loss, total);
    if (received) printf("rtt min/avg/max = %ld/%ld/%ld ms\n", rtt_min, rtt_sum / received, rtt_max);
    close(fd);
    return received > 0 ? 0 : 1;
}
