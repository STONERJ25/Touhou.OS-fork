/* nc: connect to a TCP or UDP port (or listen on one) and shuttle standard input and output through it. */
#include "netutil.h"

static int relay(int fd, int is_udp, int wait_s) {
    char buf[4096];
    int in_open = 1;
    long idle_since = 0;
    for (;;) {
        struct gpollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        int timeout = -1;
        if (!in_open && wait_s > 0) timeout = wait_s * 1000;
        int r = poll(p, in_open ? 2 : 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; return 1; }
        if (r == 0) {
            if (!in_open) return 0;                      /* stdin is done and the peer has said nothing for -w seconds */
            continue;
        }
        (void)idle_since;
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n <= 0) return n < 0 && errno != ECONNRESET ? 1 : 0;
            for (ssize_t off = 0; off < n;) {
                ssize_t w = write(1, buf + off, (size_t)(n - off));
                if (w <= 0) return 1;
                off += w;
            }
        }
        if (in_open && (p[1].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                in_open = 0;
                if (!is_udp) shutdown(fd, SHUT_WR);
                else if (wait_s <= 0) return 0;
            } else {
                for (ssize_t off = 0; off < n;) {
                    ssize_t w = send(fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
                    if (w <= 0) return 1;
                    off += w;
                }
            }
        }
    }
}

APPLET(nc) {
    int udp = 0, listen_mode = 0, zero = 0, verbose = 0, wait_s = 0, opt;
    unsigned port = 0;
    optind = 1;
    while ((opt = getopt(argc, argv, "ulp:zvw:n46")) != -1) {
        switch (opt) {
        case 'u': udp = 1; break;
        case 'l': listen_mode = 1; break;
        case 'p': port = (unsigned)atoi(optarg); break;
        case 'z': zero = 1; break;
        case 'v': verbose = 1; break;
        case 'w': wait_s = atoi(optarg); break;
        case 'n': case '4': case '6': break;
        default: kb_error("usage: nc [-u] [-z] [-v] [-w SECONDS] HOST PORT   |   nc -l [-u] [-p] PORT"); return 2;
        }
    }
    const char *host = NULL;
    if (listen_mode) {
        if (!port) { if (optind >= argc) { kb_error("listen on which port?"); return 2; } port = (unsigned)atoi(argv[optind++]); }
        if (optind < argc) host = argv[optind];
    } else {
        if (optind + 1 >= argc) { kb_error("usage: nc [-u] [-z] [-v] [-w SECONDS] HOST PORT"); return 2; }
        host = argv[optind];
        port = (unsigned)atoi(argv[optind + 1]);
    }
    if (!port || port > 65535) { kb_error("bad port"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) { kb_error("socket: %s", strerror(errno)); return 1; }

    if (listen_mode) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        uint32_t ip = 0;
        if (host && !nu_resolve(host, &ip)) return 1;
        sa.sin_addr.s_addr = htonl(ip);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { kb_error("bind to port %u: %s", port, strerror(errno)); return 1; }
        if (verbose) fprintf(stderr, "Listening on port %u\n", port);
        if (udp) {
            char first[2048];
            struct sockaddr_in peer;
            socklen_t pl = sizeof peer;
            ssize_t n = recvfrom(fd, first, sizeof first, 0, (struct sockaddr *)&peer, &pl);
            if (n < 0) { kb_error("recvfrom: %s", strerror(errno)); return 1; }
            if (connect(fd, (struct sockaddr *)&peer, pl) < 0) { kb_error("connect: %s", strerror(errno)); return 1; }
            if (write(1, first, (size_t)n) < 0) return 1;
            return relay(fd, 1, wait_s);
        }
        if (listen(fd, 1) < 0) { kb_error("listen: %s", strerror(errno)); return 1; }
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int c = accept(fd, (struct sockaddr *)&peer, &pl);
        if (c < 0) { kb_error("accept: %s", strerror(errno)); return 1; }
        if (verbose) fprintf(stderr, "Connection from %s:%u\n", nu_ip(ntohl(peer.sin_addr.s_addr)), ntohs(peer.sin_port));
        close(fd);
        return relay(c, 0, wait_s);
    }

    uint32_t ip;
    if (!nu_resolve(host, &ip)) return 1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(ip);
    if (wait_s > 0) {
        struct timeval tv = { wait_s, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (verbose || zero) fprintf(stderr, "nc: connect to %s port %u (%s) failed: %s\n", host, port, udp ? "udp" : "tcp", strerror(errno));
        else kb_error("%s: %s", host, strerror(errno));
        return 1;
    }
    if (verbose || zero) fprintf(stderr, "Connection to %s %u port [%s/*] succeeded!\n", host, port, udp ? "udp" : "tcp");
    if (zero) { close(fd); return 0; }
    return relay(fd, udp, wait_s);
}
