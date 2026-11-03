/*
 * Port forwarding (RFC 4254, section 7): "direct-tcpip" channels, which ssh -L, ssh -D (SOCKS) and ssh -W (and so ProxyJump, ssh -J) open for every
 * connection they carry: the client names a host and a port, the server connects there itself and the channel carries the bytes both ways. That is
 * how a service that listens only on this machine (a VNC server, a web page for the administrator, a debugger) is reached from outside without being
 * opened to the network: it rides inside the encrypted, authenticated connection. And, off unless asked for, the other way round (ssh -R): a
 * "tcpip-forward" request makes the server listen on its loopback, and each connection that comes in there becomes a "forwarded-tcpip" channel
 * that the client connects onward on its side.
 *
 * What may be reached is the policy of sshd.forward= on the kernel command line, checked on the address that is actually connected to (after a name
 * was looked up), so a name that leads elsewhere is refused like an address that does:
 *   local   the default: this machine only (127.0.0.0/8; 0.0.0.0, which the stack takes for "this host"; the addresses of its own interfaces)
 *   any     anywhere the machine can reach (a jump host, ssh -J, needs it)
 *   off     nothing
 * sshd.remoteforward= is the policy of ssh -R: off (the default), or local: the server listens on 127.0.0.1 only, whatever address the client asked
 * for, and never on a port below 1024. Forwarding gives no new rights (whoever logs in has a shell and could connect anywhere with nc), but it gives
 * reach: a service that trusts "connections from 127.0.0.1" trusts every key that may log in, and a -R port on the loopback is open to every resident
 * of this machine. docs/SSH.md says so.
 *
 * Caps: sshd.forwardmax=N channels per connection (16 unless said, at most FWD_MAX_HARD), sshd.forwardtotal=N for the whole server (64; 0 = only the
 * per-connection cap), counting both kinds; FWD_LISTENERS -R ports per connection. The connections are processes of their own, so the total is kept
 * in slot files in /var/run/sshd (root's, 0700): a channel holds the file it created with O_EXCL and removes it when it ends; the listening process
 * removes the files of a connection process that ended without doing so (fwd_reaped), and a full set is checked for owners that no longer exist
 * before an open is refused.
 *
 * Everything happens in the connection's one poll loop (session.c), and nothing here waits:
 *   LOOKUP    the target is a name: a child process asks the resolver (that may take seconds when a name server is silent) and writes the address
 *             into a pipe that the loop polls; at most FWD_LOOKUPS at a time, the others wait their turn
 *   CONNECT   a non-blocking connect(), with a deadline of FWD_CONNECT_MS
 *   OPENING   (ssh -R) the server's own CHANNEL_OPEN is out; the client's confirmation is awaited, the accepted socket is not read meanwhile
 *   OPEN      the confirmation has gone out (or come in) and bytes flow
 * Flow control: nothing is read from the socket beyond the peer's window, so a client that stops reading stops the target (its socket buffer
 * fills up); and the window we give the peer is opened again only for bytes the socket has taken, so at most FWD_WINDOW bytes of a channel ever
 * wait in our buffer. EOF travels both ways (shutdown(SHUT_WR) toward the target once the peer's EOF has come and everything before it is
 * delivered; CHANNEL_EOF toward the peer when the target's side ends), and the channel is closed when both directions are done, when the socket
 * fails, or when the peer closes it. A peer's CLOSE does not throw away what it sent before: OpenSSH sends DATA, EOF and CLOSE back to back once
 * the target has ended its side, and a target that reads slowly still gets the last bytes (for up to FWD_DRAIN_MS) before its socket is closed.
 *
 * The log gets one line when a channel opens or is refused and one when it closes: the resident, the target, who the client says asked for it,
 * the result and the byte counts, never the bytes. A connection that opens hundreds of channels (a browser behind ssh -D, or a flood) gets lines for
 * its first FWD_LOG_CHANNELS and ends with one line that counts the rest: the log is also written to the stick.
 */
#include "sshd.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gensokyo.h>

#define FWD_WINDOW      65536           /* the window the peer gets, and so the most that waits in a channel's buffer */
#define FWD_MAX_PACKET  32768           /* the largest data packet we take */
#define FWD_CHUNK       32768           /* the most we send in one data packet */
#define FWD_LOOKUP_MS   20000           /* a name without an answer by then is given up */
#define FWD_CONNECT_MS  10000           /* a target that has not answered the connect by then is given up */
#define FWD_DRAIN_MS    30000           /* after the peer's CLOSE, how long the bytes that came before it may take to reach the target */
#define FWD_LOOKUPS     4               /* resolver children at once, per connection */
#define FWD_LOG_CHANNELS 20            /* channels (and -R requests) per connection that get their lines in the log */
#define FWD_RUN_DIR     "/var/run/sshd"

/* reasons of a refused open (RFC 4254, section 5.1) */
enum { OPEN_PROHIBITED = 1, OPEN_CONNECT_FAILED = 2, OPEN_UNKNOWN_TYPE = 3, OPEN_RESOURCE_SHORTAGE = 4 };

int g_fwd_policy = FWD_LOCAL;
int g_rfwd_policy = RFWD_OFF;
int g_fwd_max = 16;
int g_fwd_total = 64;

typedef enum { F_FREE, F_LOOKUP, F_CONNECT, F_OPENING, F_OPEN } FState;

typedef struct {
    FState st;
    bool remote;                        /* ssh -R: the connection came in on one of our listeners, and the client connects it onward */
    uint32_t gen;                       /* new at every open: a poll result meant for the channel that had this place before is not applied */
    uint32_t peer;                      /* the peer's number for the channel */
    uint32_t peer_win, peer_max;        /* what the peer still lets us send, and its largest packet */
    uint32_t our_win, to_adjust;        /* what the peer may still send; bytes the socket took that the peer has not been given back */
    int sock;                           /* toward the target (ssh -R: the connection that came in) */
    int lk_fd;                          /* LOOKUP: the read end of the resolver child's pipe (-1 while the name waits for its turn) */
    pid_t lk_pid;
    long started, deadline;             /* mono_ms */
    uint32_t addr;                      /* the target, host byte order, once known */
    unsigned port;                      /* the target's port (ssh -R: the port of our listener) */
    bool named;                         /* the target was given as a name (the log shows the address it led to) */
    char host[256];                     /* the target as the client named it, checked to be a host name or an address (ssh -R: our listener's address) */
    char orig[64];                      /* "127.0.0.1:51234": who the client says asked for the channel (ssh -R: who connected here), made printable */
    uint8_t *buf;                       /* the peer's bytes that the socket has not taken yet (FWD_WINDOW bytes, allocated when the channel opens) */
    size_t buf_len;
    bool peer_eof;                      /* the peer sent EOF */
    bool wr_shut;                       /* nothing more goes to the target: the EOF was passed on, or the socket failed */
    bool sock_eof;                      /* nothing more is read from the target: EOF was sent to the peer (or the peer closed the channel) */
    bool close_sent;                    /* our CLOSE went out; the place is free once the peer's has come too, and what it sent before has gone on */
    bool close_got;                     /* the peer's CLOSE came; the socket stays open only while the bytes before it drain */
    int slot;                           /* the slot file held for the server-wide cap, -1 none */
    unsigned long long up, down;        /* bytes toward the target and from it, for the log */
    unsigned long long up_at_close;     /* `up` when the peer's CLOSE came */
    bool quiet;                         /* its opening was not logged (the connection's lines are used up): neither is its end */
} Fwd;

/* An ssh -R port: where we listen, and what the client asked for (it finds its forward by that name and port when we open a channel). */
typedef struct {
    int fd;                             /* -1: a free place */
    char asked[64];                     /* the address as the client sent it ("localhost", "", "127.0.0.1", ...) */
    unsigned asked_port, port;          /* the port it asked for (0: any) and the one we listen on */
} Lst;

static Fwd g_f[FWD_MAX_HARD];
static Lst g_l[FWD_LISTENERS];
static int g_nfwd;                      /* places in use */
static int g_lookups;                   /* resolver children running */
static uint32_t g_gen;
static const Account *g_acct;
static Conn *g_conn;
static int g_pfi[FWD_MAX_HARD];         /* where each channel's descriptor is in this round's poll set, -1 nowhere */
static uint32_t g_pfgen[FWD_MAX_HARD];
static int g_lpfi[FWD_LISTENERS];       /* the same for the listeners */
static uint8_t g_pkt[FWD_CHUNK + 16];
static int g_log_left = FWD_LOG_CHANNELS;
static unsigned g_quiet_opened, g_quiet_refused;
static pid_t g_zombies[FWD_MAX_HARD];   /* resolver children that answered and have not been collected yet */

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t fwd_id(const Fwd *f) { return FWD_BASE + (uint32_t)(f - g_f); }
static uint32_t min3(uint32_t a, uint32_t b, uint32_t c) { uint32_t m = a < b ? a : b; return m < c ? m : c; }

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static const char *ipstr(uint32_t a, char *buf, size_t n) {
    snprintf(buf, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return buf;
}

/* ------------------------------------------------------------------------------------------------ the log */

/* Takes one of the connection's lines; the last one says that what follows is only counted. False when they are used up. */
static bool take_line(void) {
    if (g_log_left <= 0) return false;
    if (--g_log_left == 0) slog("%s: %s: further forwards of this connection are counted, not logged one by one", g_conn->peer, g_acct->name);
    return true;
}

/* One line about a channel: "10.0.2.2: marisa: forward to localhost:5900 = 127.0.0.1 (for 127.0.0.1:51234): open", or for ssh -R "...: remote forward
 * 127.0.0.1:9000 (for 127.0.0.1:43210): open". `opening` says whether this is the first line of the channel: that one takes a line of the
 * connection's budget (and decides whether the channel's last line is logged too). */
static void flog(Fwd *f, bool opening, bool good, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void flog(Fwd *f, bool opening, bool good, const char *fmt, ...) {
    if (opening) f->quiet = g_log_left <= 0;
    if (f->quiet) {
        if (opening && good) g_quiet_opened++;
        else if (opening) g_quiet_refused++;
        return;
    }
    char what[160], addr[20] = "", target[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    char shown[48];                                           /* (the line has 196 characters: the result at its end must fit) */
    printable(shown, sizeof shown, (const uint8_t *)f->host, strlen(f->host));
    if (f->named && f->addr) { char a[20]; snprintf(addr, sizeof addr, " = %s", ipstr(f->addr, a, sizeof a)); }
    snprintf(target, sizeof target, "%s:%u%s", shown, f->port, addr);
    slog("%s: %s: %s %s (for %s): %s", g_conn->peer, g_acct->name, f->remote ? "remote forward" : "forward to", target, f->orig, what);
    if (opening) take_line();
}

/* "5242880 bytes to the target and 0 from it" (ssh -R: "to 127.0.0.1:43210", the connection that came in here); `between` goes after the first count. */
static const char *counts(const Fwd *f, const char *between, char *buf, size_t n) {
    snprintf(buf, n, "%llu bytes to %s%s and %llu from it", f->up, f->remote ? f->orig : "the target", between, f->down);
    return buf;
}

/* ------------------------------------------------------------------------------------------------ the server-wide cap */

static void slot_path(char *p, size_t n, int i) { snprintf(p, n, FWD_RUN_DIR "/fwd.%d", i); }

static void slot_remove(int i) {
    char p[64];
    slot_path(p, sizeof p, i);
    unlink(p);
}

/* The process that holds slot i, 0 when the file is empty or unreadable, -1 when there is no such file. */
static long slot_owner(int i) {
    char p[64], t[24];
    slot_path(p, sizeof p, i);
    int fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, t, sizeof t - 1);
    close(fd);
    if (n <= 0) return 0;
    t[n] = 0;
    return strtol(t, NULL, 10);
}

static int slot_create(int i) {
    char p[64];
    slot_path(p, sizeof p, i);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? 0 : -1;
    char t[24];
    int n = snprintf(t, sizeof t, "%d\n", (int)getpid());
    if (write(fd, t, (size_t)n) != n) { close(fd); unlink(p); return -1; }
    close(fd);
    return 1;
}

/* A free slot of the g_fwd_total: its number, -1 when all are taken, -2 when the directory cannot be used (then nothing is forwarded: a cap that
 * cannot be counted is not a cap). */
static int slot_take(void) {
    if (g_fwd_total == 0) return FWD_MAX_HARD;                    /* no server-wide cap: a token that is not a file */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_fwd_total; i++) {
            int r = slot_create(i);
            if (r > 0) return i;
            if (r < 0) return -2;
        }
        /* All taken. A connection process that died without giving its slots back (killed, or crashed) is normally cleaned up by the listening
         * process; this is the second line of defence, for a listener that was itself restarted in between. */
        bool freed = false;
        for (int i = 0; i < g_fwd_total; i++) {
            long pid = slot_owner(i);
            if (pid > 0 && pid != getpid() && kill((pid_t)pid, 0) < 0 && errno == ESRCH) {
                slot_remove(i);
                freed = true;
            }
        }
        if (!freed) break;
    }
    return -1;
}

/* Gives our slot back; only while the file is still ours (had another process taken it over for a stale one, removing it would let the count slip). */
static void slot_give(int i) {
    if (i < 0 || g_fwd_total <= 0 || i >= g_fwd_total) return;
    if (slot_owner(i) == (long)getpid()) slot_remove(i);
}

void fwd_setup(void) {
    if (g_fwd_total > 0) {
        mkdir("/var/run", 0755);
        mkdir(FWD_RUN_DIR, 0700);
        struct stat st;
        if (lstat(FWD_RUN_DIR, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) {
            slog("%s is not a directory of root's that only root can write: port forwarding is off", FWD_RUN_DIR);
            g_fwd_policy = FWD_OFF;
            g_rfwd_policy = RFWD_OFF;
        } else {
            for (int i = 0; i < g_fwd_total; i++) {                   /* left over from an earlier server: keep those whose owner still runs */
                long pid = slot_owner(i);
                if (pid == 0 || (pid > 0 && kill((pid_t)pid, 0) < 0 && errno == ESRCH)) slot_remove(i);
            }
        }
    }
    static const char *const how[] = { "off (sshd.forward=off)", "to this machine only (sshd.forward=local)", "to anywhere (sshd.forward=any)" };
    if (g_fwd_policy == FWD_OFF) slog("port forwarding %s", how[FWD_OFF]);
    else if (g_fwd_total > 0) slog("port forwarding %s, at most %d channels per connection and %d in all", how[g_fwd_policy], g_fwd_max, g_fwd_total);
    else slog("port forwarding %s, at most %d channels per connection", how[g_fwd_policy], g_fwd_max);
    slog("remote forwarding (ssh -R) %s", g_rfwd_policy == RFWD_LOCAL ? "on the loopback only (sshd.remoteforward=local)" : "off (sshd.remoteforward=off)");
}

void fwd_reaped(pid_t pid) {
    if (g_fwd_total <= 0 || pid <= 0) return;
    for (int i = 0; i < g_fwd_total; i++)
        if (slot_owner(i) == (long)pid) slot_remove(i);
}

/* ------------------------------------------------------------------------------------------------ the policy */

/* Is the address (host byte order) this machine? 127.0.0.0/8, 0.0.0.0 (connect() takes it for the loopback), or an address of one of the
 * interfaces as they are now (DHCP may have changed them since the connection began). */
static bool is_local(uint32_t a) {
    if ((a >> 24) == 127 || a == 0) return true;
    for (uint32_t i = 0; i < 16; i++) {
        struct gnet_if ifc;
        memset(&ifc, 0, sizeof ifc);
        ifc.index = i;
        if (gap_netctl(NETCTL_IF_GET, &ifc) < 0) break;
        if (ifc.addr && ifc.addr == a) return true;
    }
    return false;
}

static bool allowed(uint32_t a) { return g_fwd_policy == FWD_ANY || (g_fwd_policy == FWD_LOCAL && is_local(a)); }

/* A host name (RFC 1123 letters, digits, hyphens and dots; underscores too, which some local names have) or a dotted address. */
static bool host_text_ok(const uint8_t *h, size_t n) {
    if (n == 0 || n > 253) return false;
    for (size_t i = 0; i < n; i++)
        if (!((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9') || h[i] == '-' || h[i] == '.' || h[i] == '_'))
            return false;
    return true;
}

/* "localhost" and the names under it are the loopback without asking anyone (RFC 6761, section 6.3). */
static bool is_localhost_name(const char *h) {
    size_t n = strlen(h);
    if (n && h[n - 1] == '.') n--;
    return (n == 9 && !strncasecmp(h, "localhost", 9)) || (n > 10 && !strncasecmp(h + n - 10, ".localhost", 10));
}

/* ssh -R under sshd.remoteforward=local: the addresses that a listener on 127.0.0.1 stands in for. "" and "*" (and 0.0.0.0) mean every address in
 * RFC 4254; they get the loopback only, as OpenSSH's own server does without GatewayPorts. */
static bool loopback_bind_ok(const char *a) {
    return !a[0] || !strcmp(a, "*") || !strcmp(a, "0.0.0.0") || !strcmp(a, "127.0.0.1") || !strcasecmp(a, "localhost");
}

/* ------------------------------------------------------------------------------------------------ replies */

static int reply_simple(Conn *c, uint8_t type, uint32_t peer) {
    uint8_t m[5];
    m[0] = type;
    put_be32(m + 1, peer);
    return send_msg(c, m, sizeof m);
}

static int send_open_failure(Conn *c, uint32_t peer, uint32_t reason, const char *text) {
    uint8_t buf[200];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_OPEN_FAILURE);
    wr_u32(&w, peer);
    wr_u32(&w, reason);
    wr_cstr(&w, text);
    wr_cstr(&w, "");
    return w.bad ? -1 : send_msg(c, buf, w.n);
}

/* ------------------------------------------------------------------------------------------------ channel life */

static void lookup_stop(Fwd *f, bool kill_it) {
    if (f->lk_fd >= 0) close(f->lk_fd);
    f->lk_fd = -1;
    if (f->lk_pid > 0) {
        if (kill_it) kill(f->lk_pid, SIGKILL);
        if (waitpid(f->lk_pid, NULL, WNOHANG) != f->lk_pid) {
            bool kept = false;
            for (int i = 0; i < FWD_MAX_HARD && !kept; i++)
                if (!g_zombies[i]) { g_zombies[i] = f->lk_pid; kept = true; }
            if (!kept) { kill(f->lk_pid, SIGKILL); waitpid(f->lk_pid, NULL, 0); }
        }
        g_lookups--;
    }
    f->lk_pid = 0;
}

static void reap_zombies(bool final) {
    for (int i = 0; i < FWD_MAX_HARD; i++) {
        if (!g_zombies[i]) continue;
        if (final) kill(g_zombies[i], SIGKILL);
        if (waitpid(g_zombies[i], NULL, final ? 0 : WNOHANG) == g_zombies[i] || final) g_zombies[i] = 0;
    }
}

static void fwd_free(Fwd *f) {
    if (f->st == F_FREE) return;
    if (f->sock >= 0) close(f->sock);
    lookup_stop(f, true);
    free(f->buf);
    slot_give(f->slot);
    memset(f, 0, sizeof *f);
    f->sock = f->lk_fd = f->slot = -1;
    f->st = F_FREE;
    g_nfwd--;
}

/* A new channel in a free place, counted in; a scratch one (also counted, so that fwd_free() counts it out the same way) when there is none, which
 * the caller then refuses: a refusal is logged like any other channel's end. */
static Fwd *fwd_new(Fwd *scratch) {
    Fwd *f = scratch;
    for (int i = 0; i < FWD_MAX_HARD && f == scratch; i++)
        if (g_f[i].st == F_FREE) f = &g_f[i];
    memset(f, 0, sizeof *f);
    f->sock = f->lk_fd = f->slot = -1;
    f->started = mono_ms();
    f->st = F_LOOKUP;                                         /* (not free; the caller sets the real state) */
    g_nfwd++;
    return f;
}

/* The caps; NULL when the channel may go on, else why not (and then it has a slot when it may). */
static const char *caps_refuse(Fwd *f, bool scratch, char *why, size_t n) {
    if (scratch || g_nfwd > g_fwd_max) {
        snprintf(why, n, "too many forwarded channels on this connection (sshd.forwardmax=%d)", g_fwd_max);
        return why;
    }
    int slot = slot_take();
    if (slot == -1) { snprintf(why, n, "too many forwarded channels on this server (sshd.forwardtotal=%d)", g_fwd_total); return why; }
    if (slot < 0) { snprintf(why, n, "the server cannot count its forwarded channels (%s)", FWD_RUN_DIR); return why; }
    f->slot = slot;
    f->gen = ++g_gen;
    return NULL;
}

/* The open is refused: the peer hears why, the log too, and the place is free again. The client prints the reason's name in front of the text
 * ("open failed: connect failed: Connection refused"), so the text does not repeat it. */
static int open_fails(Conn *c, Fwd *f, uint32_t reason, const char *text) {
    flog(f, true, false, "%s: %s", reason == OPEN_CONNECT_FAILED ? "failed" : "refused", text);
    int rc = send_open_failure(c, f->peer, reason, text);
    fwd_free(f);
    return rc;
}

/* The bytes may flow: the buffer for what the peer sends, and no Nagle delay for what arrives in one packet (a VNC screen, keystrokes). */
static bool make_open(Fwd *f) {
    f->buf = malloc(FWD_WINDOW);
    if (!f->buf) return false;
    int one = 1;
    setsockopt(f->sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    f->our_win = FWD_WINDOW;
    return true;
}

/* The target took the connection: the peer gets its confirmation. */
static int confirm(Conn *c, Fwd *f) {
    if (!make_open(f)) return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, "out of memory");
    uint8_t buf[24];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_OPEN_CONFIRMATION);
    wr_u32(&w, f->peer);
    wr_u32(&w, fwd_id(f));
    wr_u32(&w, FWD_WINDOW);
    wr_u32(&w, FWD_MAX_PACKET);
    f->st = F_OPEN;
    flog(f, true, true, "open");
    return send_msg(c, buf, w.n);
}

/* The address is known: is it allowed, and does it answer? */
static int start_connect(Conn *c, Fwd *f) {
    if (!allowed(f->addr)) return open_fails(c, f, OPEN_PROHIBITED, "only this machine may be reached (sshd.forward=local)");
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, "no socket");
    set_nonblock(s);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)f->port);
    sa.sin_addr.s_addr = htonl(f->addr);
    f->sock = s;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) return confirm(c, f);
    if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) return open_fails(c, f, OPEN_CONNECT_FAILED, strerror(errno));
    f->st = F_CONNECT;
    f->deadline = mono_ms() + FWD_CONNECT_MS;
    return 0;
}

/* The resolver, in a child process of its own. The answers it parses come from the network, so before it reads a single one it forgets the
 * secrets it was forked with (the host's private key, this connection's keys: it does not exec, so it has a copy of them) and takes the
 * resident's rights. It writes 5 bytes into the pipe: 0 and the address (network byte order), 1 "no such name", 2 "no name server answered". */
static int lookup_start(Fwd *f) {
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        close(p[0]);
        for (int fd = 3; fd < 64; fd++) if (fd != p[1]) close(fd);
        host_key_forget();
        memset(g_conn->main_in, 0, sizeof g_conn->main_in);
        memset(g_conn->hdr_in, 0, sizeof g_conn->hdr_in);
        memset(g_conn->main_out, 0, sizeof g_conn->main_out);
        memset(g_conn->hdr_out, 0, sizeof g_conn->hdr_out);
        if (g_acct->uid && (initgroups(g_acct->name, (gid_t)g_acct->gid) != 0 || setgid((gid_t)g_acct->gid) != 0 || setuid((uid_t)g_acct->uid) != 0)) _exit(1);
        uint8_t ans[5] = { 1, 0, 0, 0, 0 };
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(f->host, NULL, &hints, &res);
        if (rc == 0 && res && res->ai_addr) {
            ans[0] = 0;
            memcpy(ans + 1, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
        } else {
            ans[0] = rc == EAI_AGAIN ? 2 : 1;
        }
        if (write(p[1], ans, sizeof ans) != (ssize_t)sizeof ans) _exit(1);
        _exit(0);
    }
    close(p[1]);
    set_nonblock(p[0]);
    f->lk_fd = p[0];
    f->lk_pid = pid;
    g_lookups++;
    return 0;
}

void fwd_begin(Conn *c, const Account *a) {
    g_conn = c;
    g_acct = a;
    for (int i = 0; i < FWD_MAX_HARD; i++) {
        memset(&g_f[i], 0, sizeof g_f[i]);
        g_f[i].sock = g_f[i].lk_fd = g_f[i].slot = -1;
        g_pfi[i] = -1;
    }
    for (int i = 0; i < FWD_LISTENERS; i++) {
        memset(&g_l[i], 0, sizeof g_l[i]);
        g_l[i].fd = -1;
        g_lpfi[i] = -1;
    }
    g_nfwd = g_lookups = 0;
}

bool fwd_owns(uint32_t id) { return id >= FWD_BASE && id - FWD_BASE < FWD_MAX_HARD && g_f[id - FWD_BASE].st != F_FREE; }

static Fwd *fwd_of(uint32_t id) { return fwd_owns(id) ? &g_f[id - FWD_BASE] : NULL; }

/* A "direct-tcpip" open (RFC 4254, section 7.2): string host, uint32 port, string originator address, uint32 originator port. */
int fwd_open(Conn *c, Rd *r, uint32_t sender, uint32_t win, uint32_t max) {
    size_t hl, ol;
    const uint8_t *host = rd_str(r, &hl);
    uint32_t port = rd_u32(r);
    const uint8_t *orig = rd_str(r, &ol);
    uint32_t oport = rd_u32(r);
    if (r->bad) return -1;

    Fwd scratch;
    Fwd *f = fwd_new(&scratch);
    f->peer = sender;
    f->peer_win = win;
    f->peer_max = max < 128 ? 128 : max;
    f->port = port;
    printable(f->host, sizeof f->host, host, hl);
    char o[48];
    printable(o, sizeof o, orig, ol);
    snprintf(f->orig, sizeof f->orig, "%s:%u", o, (unsigned)oport);

    if (g_fwd_policy == FWD_OFF) return open_fails(c, f, OPEN_PROHIBITED, "port forwarding is off on this server (sshd.forward=off)");
    if (f == &scratch || g_nfwd > g_fwd_max) {                /* (the per-connection cap before anything else is looked at) */
        char why[100];
        snprintf(why, sizeof why, "too many forwarded channels on this connection (sshd.forwardmax=%d)", g_fwd_max);
        return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, why);
    }
    bool v6 = memchr(host, ':', hl) != NULL;
    if (v6 || !host_text_ok(host, hl)) return open_fails(c, f, OPEN_CONNECT_FAILED, v6 ? "IPv6 is not supported here" : "not a host name");
    if (port == 0 || port > 65535) return open_fails(c, f, OPEN_CONNECT_FAILED, "no such port");

    struct in_addr lit;
    if (f->host[0] >= '0' && f->host[0] <= '9' && inet_aton(f->host, &lit)) {
        f->addr = ntohl(lit.s_addr);
    } else if (is_localhost_name(f->host)) {
        f->addr = INADDR_LOOPBACK;
        f->named = true;
    } else {
        f->named = true;
    }
    if ((!f->named || f->addr) && !allowed(f->addr))          /* the address is known now: refuse it before anything is spent on it */
        return open_fails(c, f, OPEN_PROHIBITED, "only this machine may be reached (sshd.forward=local)");
    char why[100];
    if (caps_refuse(f, false, why, sizeof why)) return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, why);
    if (f->named && !f->addr) {                                /* LOOKUP: the resolver child starts when there is room for it */
        f->deadline = f->started + FWD_LOOKUP_MS;
        if (g_lookups < FWD_LOOKUPS && lookup_start(f) < 0) return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, "cannot look the name up now");
        return 0;
    }
    return start_connect(c, f);
}

/* ------------------------------------------------------------------------------------------------ ssh -R */

static int global_reply(Conn *c, bool want, bool ok, bool with_port, unsigned port) {
    if (!want) return 0;
    uint8_t m[5];
    m[0] = ok ? MSG_REQUEST_SUCCESS : MSG_REQUEST_FAILURE;
    put_be32(m + 1, port);
    return send_msg(c, m, ok && with_port ? 5 : 1);
}

/* "tcpip-forward" and "cancel-tcpip-forward" (RFC 4254, section 7.1): string address to bind, uint32 port (0: the server picks one, and says which). */
int fwd_global(Conn *c, bool cancel, Rd *r, bool want) {
    size_t al;
    const uint8_t *a = rd_str(r, &al);
    uint32_t port = rd_u32(r);
    if (r->bad) return -1;
    char asked[64];
    printable(asked, sizeof asked, a, al);
    if (cancel) {
        for (int i = 0; i < FWD_LISTENERS; i++) {
            Lst *l = &g_l[i];
            if (l->fd < 0 || strcmp(l->asked, asked) || (port != l->asked_port && port != l->port)) continue;
            close(l->fd);
            l->fd = -1;
            if (take_line()) slog("%s: %s: remote forward 127.0.0.1:%u (asked for %s:%u): closed at the client's request", c->peer, g_acct->name, l->port, asked, l->asked_port);
            return global_reply(c, want, true, false, 0);
        }
        return global_reply(c, want, false, false, 0);
    }
    const char *why = NULL;
    char err[80];
    int slot = -1;
    for (int i = 0; i < FWD_LISTENERS && slot < 0; i++)
        if (g_l[i].fd < 0) slot = i;
    if (g_rfwd_policy == RFWD_OFF) why = "remote forwarding is off on this server (sshd.remoteforward=off)";
    else if (al != strlen(asked) || !loopback_bind_ok(asked)) why = "only the loopback may listen (sshd.remoteforward=local)";
    else if (port > 65535) why = "no such port";
    else if (port && port < 1024) why = "the ports below 1024 are root's";
    else if (slot < 0) why = "too many remote forwards on this connection";
    int fd = -1;
    unsigned bound = 0;
    if (!why) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t sl = sizeof sa;
        if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 8) < 0 || getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
            snprintf(err, sizeof err, "cannot listen: %s", strerror(errno));
            why = err;
            if (fd >= 0) close(fd);
            fd = -1;
        } else {
            set_nonblock(fd);
            bound = ntohs(sa.sin_port);
        }
    }
    if (why) {
        if (take_line()) slog("%s: %s: remote forward %s:%u: refused: %s", c->peer, g_acct->name, asked, (unsigned)port, why);
        else g_quiet_refused++;
        return global_reply(c, want, false, false, 0);
    }
    Lst *l = &g_l[slot];
    l->fd = fd;
    snprintf(l->asked, sizeof l->asked, "%s", asked);
    l->asked_port = port;
    l->port = bound;
    if (take_line()) slog("%s: %s: remote forward 127.0.0.1:%u (asked for %s:%u): listening", c->peer, g_acct->name, bound, asked, (unsigned)port);
    return global_reply(c, want, true, port == 0, bound);
}

/* A connection came in on a listener: a "forwarded-tcpip" channel goes to the client (string address that was connected, uint32 port that was
 * connected, string originator address, uint32 originator port). The address is the one the client asked for: it finds its forward by it. */
static int accept_remote(Conn *c, Lst *l) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int s = accept(l->fd, (struct sockaddr *)&sa, &sl);
    if (s < 0) return 0;
    set_nonblock(s);
    Fwd scratch;
    Fwd *f = fwd_new(&scratch);
    f->remote = true;
    f->sock = s;
    f->port = l->port;
    snprintf(f->host, sizeof f->host, "127.0.0.1");
    char oa[20];
    ipstr(ntohl(sa.sin_addr.s_addr), oa, sizeof oa);
    snprintf(f->orig, sizeof f->orig, "%s:%u", oa, (unsigned)ntohs(sa.sin_port));
    char why[100];
    const char *no = caps_refuse(f, f == &scratch, why, sizeof why);
    if (!no && !make_open(f)) no = "out of memory";
    if (no) {                                                 /* the one who connected here sees the connection closed */
        flog(f, true, false, "refused: %s", no);
        fwd_free(f);
        return 0;
    }
    uint8_t buf[200];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_OPEN);
    wr_cstr(&w, "forwarded-tcpip");
    wr_u32(&w, fwd_id(f));
    wr_u32(&w, FWD_WINDOW);
    wr_u32(&w, FWD_MAX_PACKET);
    wr_cstr(&w, l->asked);
    wr_u32(&w, l->port);
    wr_cstr(&w, oa);
    wr_u32(&w, ntohs(sa.sin_port));
    f->st = F_OPENING;
    return w.bad ? -1 : send_msg(c, buf, w.n);
}

/* ------------------------------------------------------------------------------------------------ the peer's messages */

static int give_window_back(Conn *c, Fwd *f) {
    if (f->close_sent || f->peer_eof || f->to_adjust < FWD_WINDOW / 4) return 0;   /* one packet per quarter window, not one per keystroke */
    uint8_t m[9];
    m[0] = MSG_CHANNEL_WINDOW_ADJUST;
    put_be32(m + 1, f->peer);
    put_be32(m + 5, f->to_adjust);
    f->our_win += f->to_adjust;
    f->to_adjust = 0;
    return send_msg(c, m, sizeof m);
}

/* Nothing more can go to the target (it reset the connection, or closed it while bytes were still coming): what waits is dropped, and so is what
 * still comes, but the window stays open so that the peer is never stuck on it. */
static void output_failed(Fwd *f) {
    f->to_adjust += (uint32_t)f->buf_len;
    f->buf_len = 0;
    f->wr_shut = true;
}

static int on_data(Conn *c, Fwd *f, Rd *r, bool extended) {
    if (extended) rd_u32(r);
    size_t n;
    const uint8_t *data = rd_str(r, &n);
    if (r->bad) return -1;
    if (f->st != F_OPEN) return 0;
    if (n > f->our_win) {
        slog("%s: the peer sent %zu bytes into a window of %u", c->peer, n, (unsigned)f->our_win);
        send_disconnect(c, DISC_PROTOCOL_ERROR, "window exceeded");
        return -1;
    }
    f->our_win -= (uint32_t)n;
    if (extended || f->close_sent || f->peer_eof || f->wr_shut) {           /* nowhere to put it (and extended data means nothing on a forward) */
        f->to_adjust += (uint32_t)n;
        return 0;
    }
    size_t done = 0;
    if (f->buf_len == 0) {                                                 /* straight on when the socket has room: no copy, no wait */
        ssize_t w = send(f->sock, data, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) {
            done = (size_t)w;
            f->to_adjust += (uint32_t)w;
            f->up += (unsigned long long)w;
        } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
            f->to_adjust += (uint32_t)n;
            output_failed(f);
            return 0;
        }
    }
    memcpy(f->buf + f->buf_len, data + done, n - done);                    /* fits: buf_len + our_win + to_adjust never exceeds FWD_WINDOW */
    f->buf_len += n - done;
    return give_window_back(c, f);
}

int fwd_message(Conn *c, uint8_t type, Rd *r) {
    uint32_t id = rd_u32(r);
    Fwd *f = fwd_of(id);
    if (r->bad) return -1;
    if (!f) return 0;
    char cb[160];
    switch (type) {
    case MSG_CHANNEL_OPEN_CONFIRMATION: {                     /* ssh -R: the client took the channel we opened */
        uint32_t sender = rd_u32(r), win = rd_u32(r), max = rd_u32(r);
        if (r->bad) return -1;
        if (f->st != F_OPENING) return 0;
        f->peer = sender;
        f->peer_win = win;
        f->peer_max = max < 128 ? 128 : max;
        f->st = F_OPEN;
        flog(f, true, true, "open");
        return 0;
    }
    case MSG_CHANNEL_OPEN_FAILURE: {                          /* ... or would not (it could not connect on its side, say) */
        uint32_t reason = rd_u32(r);
        size_t dl;
        const uint8_t *d = rd_str(r, &dl);
        if (r->bad) return -1;
        if (f->st != F_OPENING) return 0;
        char shown[64];
        printable(shown, sizeof shown, d, dl);
        flog(f, true, false, "refused by the client (%u): %s", (unsigned)reason, shown);
        fwd_free(f);
        return 0;
    }
    case MSG_CHANNEL_DATA:
        return on_data(c, f, r, false);
    case MSG_CHANNEL_EXTENDED_DATA:
        return on_data(c, f, r, true);
    case MSG_CHANNEL_WINDOW_ADJUST: {
        uint32_t add = rd_u32(r);
        if (r->bad) return -1;
        f->peer_win = add > 0xFFFFFFFFu - f->peer_win ? 0xFFFFFFFFu : f->peer_win + add;
        return 0;
    }
    case MSG_CHANNEL_EOF:
        if (f->st == F_OPEN) f->peer_eof = true;
        return 0;
    case MSG_CHANNEL_CLOSE: {                                 /* the place is free once both CLOSEs have gone (ours may have gone already) */
        if (f->st != F_OPEN) {                                /* (not open yet: there is nothing to answer) */
            fwd_free(f);
            return 0;
        }
        int rc = 0;
        if (!f->close_sent) {
            rc = reply_simple(c, MSG_CHANNEL_CLOSE, f->peer);
            f->close_sent = true;
        }
        f->close_got = true;
        f->sock_eof = true;                                   /* nothing more is read from the target */
        f->up_at_close = f->up;
        if (f->sock >= 0 && f->buf_len && !f->wr_shut) {      /* the bytes that came before the CLOSE drain first (relay_step) */
            f->deadline = mono_ms() + FWD_DRAIN_MS;
            return rc;
        }
        flog(f, false, true, "closed by the client, %s", counts(f, "", cb, sizeof cb));
        fwd_free(f);
        return rc;
    }
    case MSG_CHANNEL_REQUEST: {                               /* none means anything on a forward */
        size_t tl;
        rd_str(r, &tl);
        bool want = rd_bool(r);
        if (r->bad) return -1;
        return want && f->st == F_OPEN && !f->close_sent ? reply_simple(c, MSG_CHANNEL_FAILURE, f->peer) : 0;
    }
    default:                                                  /* SUCCESS, FAILURE: answers to nothing we asked */
        return 0;
    }
}

/* ------------------------------------------------------------------------------------------------ the loop's side */

int fwd_poll_add(struct pollfd *pf, int n, int cap) {
    for (int i = 0; i < FWD_MAX_HARD; i++) {
        g_pfi[i] = -1;
        Fwd *f = &g_f[i];
        int fd = -1;
        short ev = 0;
        if (f->st == F_LOOKUP && f->lk_fd >= 0) { fd = f->lk_fd; ev = POLLIN; }
        else if (f->st == F_CONNECT) { fd = f->sock; ev = POLLOUT; }
        else if (f->st == F_OPEN && f->sock >= 0) {
            fd = f->sock;
            if (!f->sock_eof && f->peer_win > 0) ev |= POLLIN;            /* (a closed window: the target waits, its socket buffer fills up) */
            if (f->buf_len && !f->wr_shut) ev |= POLLOUT;
        }
        if (fd < 0 || !ev || n >= cap) continue;
        pf[n] = (struct pollfd){ .fd = fd, .events = ev };
        g_pfi[i] = n++;
        g_pfgen[i] = f->gen;
    }
    for (int i = 0; i < FWD_LISTENERS; i++) {
        g_lpfi[i] = -1;
        if (g_l[i].fd < 0 || n >= cap) continue;
        pf[n] = (struct pollfd){ .fd = g_l[i].fd, .events = POLLIN };
        g_lpfi[i] = n++;
    }
    return n;
}

bool fwd_busy(void) {
    for (int i = 0; i < FWD_MAX_HARD; i++)
        if (g_f[i].st == F_LOOKUP || g_f[i].st == F_CONNECT) return true;
    return false;
}

static int lookup_step(Conn *c, Fwd *f, short ev, long now) {
    if (f->lk_pid == 0) {                                     /* waiting for its turn */
        if (now > f->deadline) return open_fails(c, f, OPEN_CONNECT_FAILED, "name lookup timed out");
        if (g_lookups < FWD_LOOKUPS && lookup_start(f) < 0) return open_fails(c, f, OPEN_RESOURCE_SHORTAGE, "cannot look the name up now");
        return 0;
    }
    if (ev & (POLLIN | POLLHUP | POLLERR)) {
        uint8_t ans[5];
        ssize_t n = read(f->lk_fd, ans, sizeof ans);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        lookup_stop(f, false);
        if (n != (ssize_t)sizeof ans || ans[0] != 0)
            return open_fails(c, f, OPEN_CONNECT_FAILED, n == (ssize_t)sizeof ans && ans[0] == 2 ? "no name server answered" : "no such host");
        uint32_t a;
        memcpy(&a, ans + 1, 4);
        f->addr = ntohl(a);
        if (!f->addr) return open_fails(c, f, OPEN_CONNECT_FAILED, "no such host");
        return start_connect(c, f);
    }
    if (now > f->deadline) return open_fails(c, f, OPEN_CONNECT_FAILED, "name lookup timed out");
    return 0;
}

static int connect_step(Conn *c, Fwd *f, short ev, long now) {
    if (ev) {
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(f->sock, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err) return open_fails(c, f, OPEN_CONNECT_FAILED, strerror(err));
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        if (getpeername(f->sock, (struct sockaddr *)&sa, &sl) == 0) return confirm(c, f);
        if (ev & (POLLERR | POLLHUP)) return open_fails(c, f, OPEN_CONNECT_FAILED, strerror(ECONNREFUSED));
        /* otherwise a wake-up without news: still connecting */
    }
    if (now > f->deadline) return open_fails(c, f, OPEN_CONNECT_FAILED, "the target did not answer in time");
    return 0;
}

/* What waited for room in the socket goes on, as far as there is room now. */
static void flush_buf(Fwd *f) {
    ssize_t put = send(f->sock, f->buf, f->buf_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (put > 0) {
        memmove(f->buf, f->buf + put, f->buf_len - (size_t)put);
        f->buf_len -= (size_t)put;
        f->to_adjust += (uint32_t)put;
        f->up += (unsigned long long)put;
    } else if (put < 0 && errno != EAGAIN && errno != EINTR) {
        output_failed(f);
    }
}

static int relay_step(Conn *c, Fwd *f, short ev) {
    char cb[160];
    if (f->close_got) {                                       /* the peer has closed; the last of what it sent drains, then the channel is gone */
        if (f->buf_len && !f->wr_shut && (ev & (POLLOUT | POLLHUP | POLLERR))) flush_buf(f);
        if (!f->buf_len || f->wr_shut || mono_ms() > f->deadline) {
            char after[40];
            snprintf(after, sizeof after, " (%llu after the close)", f->up - f->up_at_close);
            flog(f, false, true, "closed by the client, %s%s", counts(f, after, cb, sizeof cb), f->buf_len ? "; the rest did not get through in time" : "");
            fwd_free(f);
        }
        return 0;
    }
    if (f->close_sent) return 0;                              /* waiting for the peer's CLOSE */
    /* from the target to the peer, as much as the peer's window and packet size allow */
    if (!f->sock_eof && f->peer_win > 0 && (ev & (POLLIN | POLLHUP | POLLERR))) {
        uint32_t room = min3(f->peer_win, f->peer_max, FWD_CHUNK);
        ssize_t got = recv(f->sock, g_pkt + 9, room, 0);
        if (got > 0) {
            g_pkt[0] = MSG_CHANNEL_DATA;
            put_be32(g_pkt + 1, f->peer);
            put_be32(g_pkt + 5, (uint32_t)got);
            f->peer_win -= (uint32_t)got;
            f->down += (unsigned long long)got;
            if (send_msg(c, g_pkt, 9 + (size_t)got) < 0) return -1;
        } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
            if (got < 0) output_failed(f);                    /* a reset: nothing goes that way either */
            f->sock_eof = true;
            if (reply_simple(c, MSG_CHANNEL_EOF, f->peer) < 0) return -1;
        }
    }
    /* from the peer to the target: what waited for room */
    if (f->buf_len && !f->wr_shut && (ev & (POLLOUT | POLLHUP | POLLERR))) flush_buf(f);
    /* the peer's EOF goes on once everything before it is delivered */
    if (f->peer_eof && !f->buf_len && !f->wr_shut) {
        shutdown(f->sock, SHUT_WR);
        f->wr_shut = true;
    }
    if (give_window_back(c, f) < 0) return -1;
    if (f->sock_eof && f->wr_shut) {                          /* both ways done */
        close(f->sock);
        f->sock = -1;
        f->close_sent = true;
        flog(f, false, true, "closed, %s", counts(f, "", cb, sizeof cb));
        f->quiet = true;                                      /* (said; the peer's CLOSE that ends it says nothing more) */
        return reply_simple(c, MSG_CHANNEL_CLOSE, f->peer);
    }
    return 0;
}

int fwd_service(Conn *c, const struct pollfd *pf) {
    long now = mono_ms();
    for (int i = 0; i < FWD_MAX_HARD; i++) {
        Fwd *f = &g_f[i];
        if (f->st == F_FREE) continue;
        short ev = g_pfi[i] >= 0 && g_pfgen[i] == f->gen ? pf[g_pfi[i]].revents : 0;
        int rc = 0;
        if (f->st == F_LOOKUP) rc = lookup_step(c, f, ev, now);
        else if (f->st == F_CONNECT) rc = connect_step(c, f, ev, now);
        else if (f->st == F_OPEN) rc = relay_step(c, f, ev);
        if (rc < 0) return -1;
    }
    for (int i = 0; i < FWD_LISTENERS; i++) {                 /* (a listener cancelled during this round is closed: its event goes nowhere) */
        if (g_lpfi[i] >= 0 && g_l[i].fd >= 0 && (pf[g_lpfi[i]].revents & POLLIN) && accept_remote(c, &g_l[i]) < 0) return -1;
        g_lpfi[i] = -1;
    }
    for (int i = 0; i < FWD_MAX_HARD; i++) g_pfi[i] = -1;
    reap_zombies(false);
    return 0;
}

void fwd_end(void) {
    char cb[160];
    for (int i = 0; i < FWD_MAX_HARD; i++) {
        Fwd *f = &g_f[i];
        if (f->st == F_OPEN && f->close_got) {
            char rest[60];
            snprintf(rest, sizeof rest, "; the connection ended before the last %zu got through", f->buf_len);
            flog(f, false, true, "closed by the client, %s%s", counts(f, "", cb, sizeof cb), rest);
        } else if (f->st == F_OPEN && !f->close_sent) {
            flog(f, false, true, "closed with the connection, %s", counts(f, "", cb, sizeof cb));
        }
        fwd_free(f);
    }
    for (int i = 0; i < FWD_LISTENERS; i++) {
        if (g_l[i].fd < 0) continue;
        close(g_l[i].fd);
        g_l[i].fd = -1;
        if (take_line()) slog("%s: %s: remote forward 127.0.0.1:%u: closed with the connection", g_conn->peer, g_acct->name, g_l[i].port);
    }
    reap_zombies(true);
    if (g_quiet_opened || g_quiet_refused)
        slog("%s: %s: %u more forwards were opened and %u refused without a line each", g_conn->peer, g_acct->name, g_quiet_opened, g_quiet_refused);
}
