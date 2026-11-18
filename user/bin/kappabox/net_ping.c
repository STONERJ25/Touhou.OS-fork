/* ping and ping6: ICMP echo through an ICMP datagram socket (the kernel stamps the identifier and the checksum), for IPv4 (ICMP) and IPv6 (ICMPv6: the socket gets the ICMPv6 message
 * without its IP header). "ping HOST" uses IPv4, unless the name only resolves to IPv6 or is an IPv6 address; -4 and -6 force a family; ping6 is "ping -6". */
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

static const char *unreach6_text(int code) {
    switch (code) {
    case 0: return "Destination unreachable: No route";
    case 1: return "Destination unreachable: Administratively prohibited";
    case 2: return "Destination unreachable: Beyond scope of source address";
    case 3: return "Destination unreachable: Address unreachable";
    case 4: return "Destination unreachable: Port unreachable";
    default: return "Destination unreachable";
    }
}

static int ping_main(int argc, char **argv, int family) {
    int count = -1, interval_ms = 1000, timeout_ms = 2000, size = 56, ttl = 0, quiet = 0;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "c:i:W:w:s:t:qn46")) != -1) {
        switch (opt) {
        case 'c': count = atoi(optarg); break;
        case 'i': interval_ms = (int)(atof(optarg) * 1000); break;
        case 'W': case 'w': timeout_ms = (int)(atof(optarg) * 1000); break;
        case 's': size = atoi(optarg); break;
        case 't': ttl = atoi(optarg); break;
        case 'q': quiet = 1; break;
        case 'n': break;
        case '4': family = AF_INET; break;
        case '6': family = AF_INET6; break;
        default: kb_error("usage: ping [-4|-6] [-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] [-q] HOST"); return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: ping [-4|-6] [-c COUNT] [-i SECONDS] [-W SECONDS] [-s BYTES] [-t TTL] [-q] HOST"); return 2; }
    if (interval_ms < 200) interval_ms = 200;
    if (timeout_ms < 100) timeout_ms = 100;
    const char *host = argv[optind];
    struct sockaddr_storage dst;
    socklen_t dstlen;
    if (!nu_resolve_sa(host, family, &dst, &dstlen)) return 2;
    int v6 = dst.ss_family == AF_INET6;
    if (size < 8 || size > (v6 ? 65000 : 1400)) { kb_error(v6 ? "packet size must be 8 to 65000" : "packet size must be 8 to 1400"); return 2; }
    int fd = v6 ? socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6) : socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) { kb_error("cannot open an ICMP socket: %s", strerror(errno)); return 2; }
    if (ttl > 0) {
        if (v6) setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof ttl);
        else setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
    }
    signal(SIGINT, on_int);

    char addrtext[80];
    nu_sa_str((struct sockaddr *)&dst, addrtext, sizeof addrtext);
    if (v6) printf("PING %s(%s) %d data bytes\n", host, addrtext, size);
    else printf("PING %s (%s) %d(%d) bytes of data.\n", host, addrtext, size, size + 28);
    static unsigned char pkt[8 + 65000], rep[65600];
    int sent = 0, received = 0, errors = 0;
    long rtt_min = 1L << 30, rtt_max = 0, rtt_sum = 0;
    long t_start = nu_now_ms();
    for (int seq = 1; !g_stop && (count < 0 || seq <= count); seq++) {
        memset(pkt, 0, 8);
        pkt[0] = v6 ? 128 : 8;
        pkt[6] = (unsigned char)(seq >> 8);
        pkt[7] = (unsigned char)seq;
        long t0 = nu_now_ms();
        memcpy(pkt + 8, &t0, sizeof t0);
        for (int i = (int)sizeof t0; i < size; i++) pkt[8 + i] = (unsigned char)(0x10 + i);
        if (sendto(fd, pkt, (size_t)size + 8, 0, (struct sockaddr *)&dst, dstlen) < 0) {
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
            struct sockaddr_storage from;
            socklen_t fl = sizeof from;
            ssize_t n = recvfrom(fd, rep, sizeof rep, 0, (struct sockaddr *)&from, &fl);
            char fromtext[80];
            if (v6) {
                if (n < 8) continue;
                unsigned char *ic = rep;                                  /* the ICMPv6 message itself */
                nu_sa_str((struct sockaddr *)&from, fromtext, sizeof fromtext);
                if (ic[0] == 129) {
                    int rseq = (ic[6] << 8) | ic[7];
                    if (rseq != seq) continue;                            /* a late answer to an earlier request */
                    long dt = nu_now_ms() - t0;
                    received++;
                    rtt_sum += dt;
                    if (dt < rtt_min) rtt_min = dt;
                    if (dt > rtt_max) rtt_max = dt;
                    if (!quiet) printf("%d bytes from %s: icmp_seq=%d time=%ld ms\n", (int)n, fromtext, rseq, dt);
                    answered = 1;
                    break;
                }
                if (ic[0] >= 1 && ic[0] <= 4 && (unsigned)n >= 8 + 40 + 8) {          /* an error that quotes our request: its IPv6 header, then the echo */
                    unsigned char *inner = ic + 8;
                    unsigned off = 40;
                    if (inner[6] == 44) off += 8;                         /* a fragment header in between (the request was fragmented) */
                    if (8 + off + 8 > (unsigned)n) continue;
                    int rseq = (inner[off + 6] << 8) | inner[off + 7];
                    if (rseq != seq) continue;
                    printf("From %s icmp_seq=%d %s\n", fromtext, rseq,
                           ic[0] == 1 ? unreach6_text(ic[1]) : ic[0] == 2 ? "Packet too big" : ic[0] == 3 ? "Time exceeded: Hop limit" : "Parameter problem");
                    errors++;
                    answered = 1;
                    break;
                }
                continue;
            }
            if (n < 28) continue;
            unsigned ihl = (rep[0] & 15) * 4u;
            if (ihl + 8 > (unsigned)n) continue;
            unsigned char *ic = rep + ihl;
            nu_sa_str((struct sockaddr *)&from, fromtext, sizeof fromtext);
            if (ic[0] == 0) {
                int rseq = (ic[6] << 8) | ic[7];
                if (rseq != seq) continue;                      /* a late answer to an earlier request */
                long dt = nu_now_ms() - t0;
                received++;
                rtt_sum += dt;
                if (dt < rtt_min) rtt_min = dt;
                if (dt > rtt_max) rtt_max = dt;
                if (!quiet) printf("%d bytes from %s: icmp_seq=%d ttl=%d time=%ld ms\n", (int)(n - ihl), fromtext, rseq, rep[8], dt);
                answered = 1;
                break;
            }
            if ((ic[0] == 3 || ic[0] == 11) && (unsigned)n >= ihl + 8 + 20 + 8) {
                unsigned char *inner = ic + 8;
                unsigned iihl = (inner[0] & 15) * 4u;
                if (ihl + 8 + iihl + 8 > (unsigned)n) continue;
                int rseq = (inner[iihl + 6] << 8) | inner[iihl + 7];
                if (rseq != seq) continue;
                printf("From %s icmp_seq=%d %s\n", fromtext, rseq, ic[0] == 11 ? "Time to live exceeded" : unreach_text(ic[1]));
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

APPLET(ping) { return ping_main(argc, argv, AF_UNSPEC); }
APPLET(ping6) { return ping_main(argc, argv, AF_INET6); }
