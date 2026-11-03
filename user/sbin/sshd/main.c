/*
 * sshd: the SSH server. Started by init; listens on port 22 (or -p PORT) and serves each connection in a process of its own.
 *
 *   sshd [-p PORT]
 *
 * Lines go to the kernel log under the name "sshd" (dmesg, and TOUHOU.LOG on the volume): the host key's fingerprint, who connected, who got in
 * (with the fingerprint of the key), who was turned away and what each session ran. Nothing secret is ever logged.
 */
#include "sshd.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <gensokyo.h>

#define MAX_CONNECTIONS 16

bool g_log_commands;

void slog(const char *fmt, ...) {
    char line[196];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    gap_log(line);
}

static void addr_text(const struct sockaddr_in *sa, char *out, size_t n, unsigned *port) {
    if (!inet_ntop(AF_INET, &sa->sin_addr, out, (socklen_t)n)) snprintf(out, n, "?");
    *port = ntohs(sa->sin_port);
}

static Conn g_conn;
static unsigned g_keepalive_ms = 60000;
static int g_throttle_fails = THROTTLE_FAILS, g_per_address = MAX_PER_ADDRESS;      /* sshd.throttle=N and sshd.peraddr=N on the command line; 0 switches the limit off */

/* The value of the word NAME (which ends in '=') on the command line, or NULL when the word is not there. */
static const char *option(const char *cl, const char *name) {
    for (const char *p = cl; (p = strstr(p, name)) != NULL; p++)
        if (p == cl || p[-1] == ' ') return p + strlen(name);
    return NULL;
}

static bool value_is(const char *v, const char *s) {
    size_t n = strlen(s);
    return !strncmp(v, s, n) && (v[n] == 0 || v[n] == ' ' || v[n] == '\n');
}

/* sshd.keepalive=SECONDS on the kernel command line (0: no probes), sshd.throttle=N (connections without a login within a minute before an address is not served; 0: never),
 * sshd.peraddr=N (simultaneous connections per address; 0: no limit), sshd.logcmds (log the whole command of an exec request): for the tests and for anyone who wants them otherwise.
 * Port forwarding (forward.c): sshd.forward=off|local|any (local: this machine only), sshd.remoteforward=off|local (ssh -R; off unless said, local: listen on the
 * loopback only), sshd.forwardmax=N channels per connection (1 to 32; 0 is the most there is, 32), sshd.forwardtotal=N for the whole server (0: no such cap). */
static void read_cmdline_options(void) {
    char cl[512] = "";
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    size_t n = fread(cl, 1, sizeof cl - 1, f);
    fclose(f);
    cl[n] = 0;
    const char *p = strstr(cl, "sshd.keepalive=");
    if (p && (p == cl || p[-1] == ' ')) g_keepalive_ms = (unsigned)atoi(p + 15) * 1000u;
    p = strstr(cl, "sshd.throttle=");
    if (p && (p == cl || p[-1] == ' ')) g_throttle_fails = atoi(p + 14);
    p = strstr(cl, "sshd.peraddr=");
    if (p && (p == cl || p[-1] == ' ')) g_per_address = atoi(p + 13);
    p = strstr(cl, "sshd.logcmds");
    g_log_commands = p && (p == cl || p[-1] == ' ') && (p[12] == 0 || p[12] == ' ' || p[12] == '\n');
    if ((p = option(cl, "sshd.forward="))) {
        if (value_is(p, "local")) g_fwd_policy = FWD_LOCAL;
        else if (value_is(p, "any")) g_fwd_policy = FWD_ANY;
        else {                                                         /* "off", and anything unknown: a typo must not give more reach than was meant */
            if (!value_is(p, "off")) slog("sshd.forward= takes off, local or any: port forwarding is off");
            g_fwd_policy = FWD_OFF;
        }
    }
    if ((p = option(cl, "sshd.remoteforward="))) {
        if (value_is(p, "local")) g_rfwd_policy = RFWD_LOCAL;
        else {
            if (!value_is(p, "off")) slog("sshd.remoteforward= takes off or local: remote forwarding is off");
            g_rfwd_policy = RFWD_OFF;
        }
    }
    if ((p = option(cl, "sshd.forwardmax="))) {
        int v = atoi(p);
        g_fwd_max = v <= 0 || v > FWD_MAX_HARD ? FWD_MAX_HARD : v;
    }
    if ((p = option(cl, "sshd.forwardtotal="))) {
        int v = atoi(p);
        g_fwd_total = v <= 0 ? 0 : v > FWD_TOTAL_MAX ? FWD_TOTAL_MAX : v;
    }
}

/* The child's exit status tells the parent how the connection went: 0 a login that ended in the ordinary way, SERVE_NO_LOGIN anything that never got in
 * (the parent counts those per address and stops serving an address that only fails). */
#define SERVE_NO_LOGIN 3

int serve(int fd) {
    Conn *c = &g_conn;
    memset(c, 0, sizeof *c);
    c->fd = fd;
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) addr_text(&sa, c->peer, sizeof c->peer, &c->peer_port);
    else snprintf(c->peer, sizeof c->peer, "unknown");
    sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) addr_text(&sa, c->local, sizeof c->local, &c->local_port);
    else snprintf(c->local, sizeof c->local, "unknown");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);          /* keystrokes should not wait for company */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);                 /* a peer that stops reading must not park send() in the kernel: write_full() waits with a deadline */
    c->keepalive_ms = g_keepalive_ms;

    Account who;
    int status = SERVE_NO_LOGIN;
    if (transport_start(c) == 0 && auth_run(c, &who) == 0) {
        session_run(c, &who);
        status = 0;
    }
    close(fd);
    slog("%s: connection closed", c->peer);
    return status;
}

int main(int argc, char **argv) {
    gap_setname("sshd");
    unsigned port = 22;
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') port = (unsigned)atoi(optarg);
        else { fprintf(stderr, "usage: sshd [-p PORT]\n"); return 2; }
    }
    if (!port || port > 65535) { fprintf(stderr, "sshd: bad port\n"); return 2; }
    if (getuid() != 0) { fprintf(stderr, "sshd: only root can run the server\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);
    chdir("/");
    read_cmdline_options();

    host_key_init();
    fwd_setup();

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { slog("no socket: %s", strerror(errno)); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 16) < 0) {
        slog("cannot listen on port %u: %s", port, strerror(errno));
        return 1;
    }
    slog("listening on port %u", port);

    /* Who is being served, and who has only failed lately. Addresses are IPv4, as the listening socket is. */
    struct { pid_t pid; uint32_t ip; } kids[MAX_CONNECTIONS];
    struct { uint32_t ip; int fails; long first; long blocked_until; bool told; } bad[32];
    memset(kids, 0, sizeof kids);
    memset(bad, 0, sizeof bad);
    int children = 0;
    for (;;) {
        struct pollfd p = { .fd = ls, .events = POLLIN };
        int pr = poll(&p, 1, 1000);
        int st;
        pid_t done;
        while ((done = waitpid(-1, &st, WNOHANG)) > 0) {
            if (children > 0) children--;
            fwd_reaped(done);                                                /* the forwards it held count no more, even if it died holding them */
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (kids[i].pid != done) continue;
                kids[i].pid = 0;
                if (WIFEXITED(st) && WEXITSTATUS(st) == SERVE_NO_LOGIN) {      /* never got in: remember the address */
                    long now = mono_ms();
                    int slot = -1;
                    for (int k = 0; k < 32; k++) if (bad[k].ip == kids[i].ip) { slot = k; break; }
                    if (slot < 0) {
                        for (int k = 0; k < 32 && slot < 0; k++) if (!bad[k].ip || (bad[k].blocked_until < now && now - bad[k].first > THROTTLE_WINDOW_MS)) slot = k;
                        if (slot < 0) slot = 0;
                        memset(&bad[slot], 0, sizeof bad[slot]);
                        bad[slot].ip = kids[i].ip;
                    }
                    if (now - bad[slot].first > THROTTLE_WINDOW_MS) { bad[slot].fails = 0; bad[slot].first = now; }
                    if (++bad[slot].fails >= g_throttle_fails && g_throttle_fails > 0) { bad[slot].blocked_until = now + THROTTLE_BLOCK_MS; bad[slot].told = false; }
                } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {            /* a login: the address is clean again */
                    for (int k = 0; k < 32; k++) if (bad[k].ip == kids[i].ip) memset(&bad[k], 0, sizeof bad[k]);
                }
                break;
            }
        }
        if (pr <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        memset(&from, 0, sizeof from);
        int c = accept(ls, (struct sockaddr *)&from, &fl);
        if (c < 0) {
            if (errno != EINTR && errno != EAGAIN) usleep(100000);          /* out of descriptors or the like: do not spin */
            continue;
        }
        uint32_t ip = from.sin_family == AF_INET ? from.sin_addr.s_addr : 0;
        long now = mono_ms();
        int blocked = -1, same = 0;
        for (int k = 0; k < 32; k++) if (ip && bad[k].ip == ip && bad[k].blocked_until > now) blocked = k;
        for (int i = 0; i < MAX_CONNECTIONS; i++) if (kids[i].pid && kids[i].ip == ip) same++;
        if (blocked >= 0) {                                                  /* said once per block, not once per attempt */
            if (!bad[blocked].told) {
                char who[32];
                unsigned port;
                addr_text(&from, who, sizeof who, &port);
                slog("%s: %d connections in a minute without a login: not served for the next %d seconds", who, g_throttle_fails, THROTTLE_BLOCK_MS / 1000);
                bad[blocked].told = true;
            }
            close(c);
            continue;
        }
        if (g_per_address > 0 && same >= g_per_address) {
            close(c);                                                        /* (silently: the log is not for this) */
            continue;
        }
        if (children >= MAX_CONNECTIONS) {
            slog("too many connections (%d): one was turned away", children);
            close(c);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(ls);
            _exit(serve(c));
        }
        close(c);
        if (pid > 0) {
            children++;
            for (int i = 0; i < MAX_CONNECTIONS; i++) if (!kids[i].pid) { kids[i].pid = pid; kids[i].ip = ip; break; }
        }
    }
}
