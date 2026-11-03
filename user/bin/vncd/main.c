/*
 * vncd: Hatate's remote window on the desktop, a VNC (RFB 3.8) server. See docs/VNC.md and vncd(8).
 *
 *   vncd [--port N] [--listen-all] [--allow-resize] [--verbose]     the server (root; init starts it when "vnc" is on the kernel command line)
 *   vncd -passwd                                                       set the VNC password (read from the terminal or standard input)
 *   vncd -nopasswd                                                     remove it (127.0.0.1 is then served without a password)
 *
 * By default it listens on 127.0.0.1:5900 only: the way in from another computer is an ssh tunnel
 * (ssh -L 5900:127.0.0.1:5900 marisa@gensokyo, then a viewer on localhost:5900). --listen-all (vnc.listen=all) serves every
 * interface, with a warning in the log, and only with a password. Kernel command line words: vnc.port=N, vnc.listen=all,
 * vnc.resize, vnc.throttle=N, vnc.peraddr=N, vnc.maxclients=N, vnc.stall=SECONDS, vnc.verbose (the last of a name wins; options
 * given to vncd itself win over all of them).
 *
 * One process serves every viewer (they share the picture), with poll(); nothing a viewer does can block the others, and the
 * window server is never waited for (source.c).
 */
#include "vncd.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifndef __linux__
#include <gensokyo.h>
#endif

Options g_opt = {
    .port = VNC_DEFAULT_PORT, .max_clients = 8, .per_address = 4, .throttle_fails = 3, .fail_delay_ms = 1000,
    .handshake_ms = 30000, .stall_ms = 60000,
    .passwd_path = "/etc/vnc.passwd", .keep_path = "/HumanVillage/.system/etc/vnc.passwd",
    .fairy_socket = "/tmp/fairy/fairyd.sock", .vinput_path = "/dev/vinput",
};

static volatile int g_stop;
static void on_term(int sig) { (void)sig; g_stop = 1; }

#ifndef __linux__
#include <sys/wait.h>
/* init starts vncd once (the "vnc" word) and does not watch it, so vncd watches itself: the parent keeps a server process running,
 * starts it again a second after it ends, and gives up after five ends within twenty seconds of their start (a broken server must
 * not become a busy loop). SIGTERM to the parent ends both. The host build has no supervisor: the tests want to see a crash. */
static volatile int g_child;
static void forward_term(int sig) { if (g_child > 0) kill(g_child, sig); g_stop = 1; }

static void supervise(void) {
    int quick = 0;
    signal(SIGTERM, forward_term);
    signal(SIGINT, forward_term);
    for (;;) {
        long started = mono_ms();
        pid_t pid = fork();
        if (pid == 0) { signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); return; }    /* the child serves */
        if (pid < 0) { sleep(1); continue; }
        g_child = pid;
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && !g_stop) { }
        g_child = 0;
        if (g_stop) exit(0);
        quick = mono_ms() - started < 20000 ? quick + 1 : 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) exit(0);
        if (quick >= 5) { vlog("the server keeps ending at once (status %d): Hatate gives up until the next restart", st); exit(1); }
        vlog("the server ended (%s %d): starting it again", WIFSIGNALED(st) ? "signal" : "status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
        sleep(1);
    }
}
#endif

static void usage(void) {
    fprintf(stderr, "usage: vncd [--port N] [--listen-all] [--allow-resize] [--verbose]\n"
                    "       vncd -passwd      set the VNC password (1 to 8 characters)\n"
                    "       vncd -nopasswd    remove it\n");
}

static int number(const char *s, int lo, int hi, const char *what) {
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) { fprintf(stderr, "vncd: %s must be a number from %d to %d\n", what, lo, hi); exit(2); }
    return (int)v;
}

static void cmdline_options(void) {
    char v[32];
    if (cmdline_value("vnc.port=", v, sizeof v)) g_opt.port = number(v, 1, 65535, "vnc.port");
    if (cmdline_value("vnc.listen=", v, sizeof v) && !strcmp(v, "all")) g_opt.listen_all = true;
    if (cmdline_word("vnc.resize")) g_opt.allow_resize = true;
    if (cmdline_value("vnc.throttle=", v, sizeof v)) g_opt.throttle_fails = number(v, 0, 1000, "vnc.throttle");
    if (cmdline_value("vnc.peraddr=", v, sizeof v)) g_opt.per_address = number(v, 0, 1000, "vnc.peraddr");
    if (cmdline_value("vnc.maxclients=", v, sizeof v)) g_opt.max_clients = number(v, 1, 64, "vnc.maxclients");
    if (cmdline_value("vnc.stall=", v, sizeof v)) g_opt.stall_ms = number(v, 1, 3600, "vnc.stall") * 1000;     /* seconds a viewer may not read */
    if (cmdline_word("vnc.verbose")) g_opt.verbose = true;
}

/* ------------------------------------------------------------------------------------------------ the password commands */

static bool read_line_quiet(const char *prompt, char *out, size_t cap) {
    bool tty = isatty(0);
    struct termios old, quiet;
    if (tty) {
        fputs(prompt, stderr);
        fflush(stderr);
        if (tcgetattr(0, &old) == 0) { quiet = old; quiet.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSAFLUSH, &quiet); }
        else tty = false;
    }
    size_t n = 0;
    bool ok = false;
    for (;;) {
        char ch;
        ssize_t r = read(0, &ch, 1);
        if (r <= 0) { ok = n > 0; break; }
        if (ch == '\n' || ch == '\r') { ok = true; break; }
        if (n + 1 < cap) out[n++] = ch;
        else n = cap;                                           /* far too long: remembered as such */
    }
    if (tty) { tcsetattr(0, TCSAFLUSH, &old); fputc('\n', stderr); }
    if (n >= cap) { out[0] = 0; return false; }
    out[n] = 0;
    return ok;
}

static int cmd_passwd(void) {
    char a[64], b[64], why[160];
    if (!read_line_quiet("New VNC password (1 to 8 characters): ", a, sizeof a)) { fprintf(stderr, "vncd: no password given\n"); return 1; }
    if (isatty(0)) {
        if (!read_line_quiet("The same once more: ", b, sizeof b) || strcmp(a, b)) { fprintf(stderr, "vncd: the two did not match; nothing changed\n"); return 1; }
    }
    int rc = auth_set_password(a, why, sizeof why);
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    if (rc < 0) { fprintf(stderr, "vncd: %s\n", why); return 1; }
    if (rc > 0) fprintf(stderr, "vncd: the password is set in %s, but %s\n", g_opt.passwd_path, why);
    else fprintf(stderr, "vncd: the VNC password is set (%s%s%s); it counts from the next connection\n", g_opt.passwd_path,
                 g_opt.keep_path ? ", kept in " : "", g_opt.keep_path ? g_opt.keep_path : "");
    return 0;
}

static int cmd_nopasswd(void) {
    int rc = 0;
    if (unlink(g_opt.passwd_path) < 0 && errno != ENOENT) { fprintf(stderr, "vncd: cannot remove %s: %s\n", g_opt.passwd_path, strerror(errno)); rc = 1; }
    if (g_opt.keep_path && unlink(g_opt.keep_path) < 0 && errno != ENOENT && errno != ENOTDIR) { fprintf(stderr, "vncd: cannot remove %s: %s\n", g_opt.keep_path, strerror(errno)); rc = 1; }
    sync();
    if (!rc) fprintf(stderr, "vncd: the VNC password is gone: only 127.0.0.1 is served, without a password\n");
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the server */

static int listen_on(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { vlog("no socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_opt.port);
    sa.sin_addr.s_addr = htonl(g_opt.listen_all ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 16) < 0) {
        vlog("cannot listen on port %d: %s", g_opt.port, strerror(errno));
        close(ls);
        return -1;
    }
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
    return ls;
}

static void accept_viewers(int ls) {
    for (int i = 0; i < 16; i++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        memset(&from, 0, sizeof from);
        int fd = accept(ls, (struct sockaddr *)&from, &fl);
        if (fd < 0) return;
        char peer[48];
        uint32_t ip = from.sin_family == AF_INET ? from.sin_addr.s_addr : 0;
        if (!inet_ntop(AF_INET, &from.sin_addr, peer, sizeof peer)) snprintf(peer, sizeof peer, "?");
        int total = clients_count(), same = 0, authed = 0;
        for (Client *c = clients_first(); c; c = client_next_of(c)) { if (client_ip(c) == ip) same++; if (client_authenticated(c)) authed++; }
        if (total >= 2 * g_opt.max_clients + 8) { close(fd); continue; }    /* a flood: not even a word */
        Client *c = client_new(fd, ip, peer);
        if (!c) { close(fd); continue; }
        long now = mono_ms();
        if (auth_blocked(ip, now)) client_set_refusal(c, "too many wrong passwords from this address: try again in a minute");
        else if (authed >= g_opt.max_clients || total >= g_opt.max_clients * 2) client_set_refusal(c, "too many viewers are connected already");
        else if (g_opt.per_address > 0 && same >= g_opt.per_address) client_set_refusal(c, "too many viewers from this address");
        if (g_opt.verbose) vlog("%s: connected", peer);
    }
}

int main(int argc, char **argv) {
    enum { RUN, PASSWD, NOPASSWD } mode = RUN;
    cmdline_options();                                          /* the kernel command line first: options given to vncd itself win */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-passwd") || !strcmp(a, "--passwd")) mode = PASSWD;
        else if (!strcmp(a, "-nopasswd") || !strcmp(a, "--nopasswd")) mode = NOPASSWD;
        else if (!strcmp(a, "--listen-all")) g_opt.listen_all = true;
        else if (!strcmp(a, "--allow-resize")) g_opt.allow_resize = true;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) g_opt.verbose = true;
        else if (!strcmp(a, "--port") && next) { g_opt.port = number(next, 1, 65535, "the port"); i++; }
        /* for the host tests (tests/host/test_vnc.py) and for trying things by hand */
        else if (!strcmp(a, "--fairy") && next) { g_opt.fairy_socket = next; i++; }
        else if (!strcmp(a, "--vinput") && next) { g_opt.vinput_path = next; i++; }
        else if (!strcmp(a, "--passwd-file") && next) { g_opt.passwd_path = next; i++; }
        else if (!strcmp(a, "--keep-file") && next) { g_opt.keep_path = strcmp(next, "none") ? next : NULL; i++; }
        else if (!strcmp(a, "--fail-delay") && next) { g_opt.fail_delay_ms = number(next, 0, 60000, "--fail-delay"); i++; }
        else if (!strcmp(a, "--handshake") && next) { g_opt.handshake_ms = number(next, 100, 600000, "--handshake"); i++; }
        else if (!strcmp(a, "--stall") && next) { g_opt.stall_ms = number(next, 100, 3600000, "--stall"); i++; }
        else if (!strcmp(a, "--throttle") && next) { g_opt.throttle_fails = number(next, 0, 1000, "--throttle"); i++; }
        else if (!strcmp(a, "--peraddr") && next) { g_opt.per_address = number(next, 0, 1000, "--peraddr"); i++; }
        else if (!strcmp(a, "--maxclients") && next) { g_opt.max_clients = number(next, 1, 64, "--maxclients"); i++; }
        else { usage(); return 2; }
    }
#ifndef __linux__
    gap_setname("vncd");                                        /* the name on the log lines */
    if (geteuid() != 0) { fprintf(stderr, "vncd: only root can run the VNC server or change its password\n"); return 1; }
#endif
    if (mode == PASSWD) return cmd_passwd();
    if (mode == NOPASSWD) return cmd_nopasswd();

    signal(SIGPIPE, SIG_IGN);
#ifndef __linux__
    supervise();
#endif
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    snprintf(g_desktop_name, sizeof g_desktop_name, "%s (TouhouOS)", host_name());
    source_init();
    vinput_open();

    int ls = listen_on();
    if (ls < 0) return 1;
    char pw[16];
    int have_pw = auth_read_password(pw, sizeof pw);
    memset(pw, 0, sizeof pw);
    if (g_opt.listen_all) {
        vlog("WARNING: listening on every interface (port %d): anyone on the network can try passwords; VNC's DES login is weak and the pictures travel unencrypted", g_opt.port);
        if (have_pw != 1) vlog("no usable VNC password: every viewer is turned away until root runs vncd -passwd");
    } else {
        vlog("Hatate keeps watch on 127.0.0.1:%d (VNC; from elsewhere: ssh -L %d:127.0.0.1:%d, then a viewer on localhost:%d)%s", g_opt.port, g_opt.port,
             g_opt.port, g_opt.port, have_pw == 1 ? "" : have_pw == 0 ? "; no password is set, so a local viewer needs none" : "");
    }

    while (!g_stop) {
        long now = mono_ms();
        source_poll(now);
        for (Client *c = clients_first(); c; c = client_next_of(c)) { client_tick(c, now); client_try_update(c); }
        for (Client *c = clients_first(); c;) {
            Client *next = client_next_of(c);
            if (client_dead(c)) client_free(c);
            c = next;
        }
        int nc = clients_count();
        struct pollfd *pf = calloc((size_t)nc + 2, sizeof *pf);
        Client **who = calloc((size_t)nc + 2, sizeof *who);
        if (!pf || !who) { free(pf); free(who); vlog("out of memory"); usleep(100000); continue; }
        int n = 0;
        pf[n].fd = ls; pf[n].events = POLLIN; n++;
        int sfd = source_fd();
        int src_i = -1;
        if (sfd >= 0) { src_i = n; pf[n].fd = sfd; pf[n].events = POLLIN; n++; }
        for (Client *c = clients_first(); c; c = client_next_of(c)) {
            who[n] = c;
            pf[n].fd = client_fd(c);
            pf[n].events = (short)(POLLIN | (client_wants_write(c) ? POLLOUT : 0));
            n++;
        }
        long deadline = now + 1000;
        long d = source_next_deadline();
        if (d && d < deadline) deadline = d;
        for (Client *c = clients_first(); c; c = client_next_of(c)) { d = client_next_deadline(c); if (d && d < deadline) deadline = d; }
        int timeout = (int)(deadline - now);
        if (timeout < 0) timeout = 0;
        int pr = poll(pf, (nfds_t)n, timeout);
        if (pr > 0) {
            if (pf[0].revents & POLLIN) accept_viewers(ls);
            if (src_i >= 0 && (pf[src_i].revents & (POLLIN | POLLHUP | POLLERR))) source_on_readable();
            for (int i = 0; i < n; i++) {
                Client *c = who[i];
                if (!c) continue;
                if (pf[i].revents & POLLOUT) client_on_writable(c);
                if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) client_on_readable(c);
            }
        }
        free(pf);
        free(who);
        vinput_flush();
    }
    vlog("Hatate puts the camera away (signal)");
    for (Client *c = clients_first(); c;) { Client *next = client_next_of(c); client_free(c); c = next; }
    close(ls);
    return 0;
}
