/*
 * sshd: the SSH-2 server (docs/SSH.md). Public-key logins only, ssh-ed25519 keys, curve25519-sha256 key exchange and
 * chacha20-poly1305@openssh.com packets, which is what OpenSSH negotiates by default. One process per connection.
 *
 *   wire.c       reading and writing the protocol's data types, base64
 *   transport.c  identification strings, packets, key exchange, key derivation (RFC 4253, with OpenSSH's strict KEX)
 *   auth.c       the host key, authorized_keys, the "publickey" method (RFC 4252)
 *   session.c    channels, ptys, the shell and exec requests, flow control (RFC 4254)
 *   forward.c    port forwarding: direct-tcpip channels (ssh -L, -D, -W, -J) and their policy (RFC 4254, section 7)
 *   main.c       options, the listening socket, one child per connection
 */
#ifndef SSHD_H
#define SSHD_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* message numbers (RFC 4250, section 4.1) */
enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4, MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6,
    MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52, MSG_USERAUTH_PK_OK = 60,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS = 81, MSG_REQUEST_FAILURE = 82,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92, MSG_CHANNEL_WINDOW_ADJUST = 93,
    MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95, MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};

/* disconnect reasons (RFC 4253, section 11.1) */
enum {
    DISC_PROTOCOL_ERROR = 2, DISC_KEY_EXCHANGE_FAILED = 3, DISC_MAC_ERROR = 5, DISC_SERVICE_NOT_AVAILABLE = 7,
    DISC_NO_MORE_AUTH_METHODS = 14, DISC_BY_APPLICATION = 11,
};

#define PKT_MAX      40000          /* the largest packet_length accepted (RFC 4253 asks for 35000) */
#define MAX_AUTH_TRIES 6
#define MAX_AUTH_REQUESTS 20        /* every authentication request counts, whatever it asks for: a client that only asks about keys does not go on for ever */
#define LOGIN_GRACE_MS 30000
#define MAX_PER_ADDRESS 4           /* simultaneous connections from one address */
#define THROTTLE_FAILS 5            /* this many connections from an address that ended without a login within THROTTLE_WINDOW_MS ... */
#define THROTTLE_WINDOW_MS 60000
#define THROTTLE_BLOCK_MS 60000     /* ... and the address is not served for this long */

/* Milliseconds from a clock that never steps (CLOCK_MONOTONIC). Every timeout of the server uses it, not gap_time_ms(): that is the wall clock, which sntp or
 * date -s can move by years (the Dell's says 2025), and a deadline that is measured on it fires at once or never. */
static inline long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

extern bool g_log_commands;         /* "sshd.logcmds" on the command line: the whole command of an exec request is logged, otherwise its first word and its length */

/* ------------------------------------------------------------------------------------------------ wire.c */

typedef struct { const uint8_t *p; size_t n; bool bad; } Rd;     /* a reader: once a read runs past the end, `bad` stays set */
typedef struct { uint8_t *p; size_t n, cap; bool bad; } Wr;      /* a writer on a caller's buffer */

void     rd_init(Rd *r, const uint8_t *p, size_t n);
uint8_t  rd_u8(Rd *r);
uint32_t rd_u32(Rd *r);
bool     rd_bool(Rd *r);
const uint8_t *rd_str(Rd *r, size_t *len);          /* points into the packet; never NULL */
void     rd_skip(Rd *r, size_t n);

void wr_init(Wr *w, uint8_t *buf, size_t cap);
void wr_u8(Wr *w, uint8_t v);
void wr_u32(Wr *w, uint32_t v);
void wr_bool(Wr *w, bool v);
void wr_raw(Wr *w, const void *p, size_t n);
void wr_str(Wr *w, const void *p, size_t n);
void wr_cstr(Wr *w, const char *s);
size_t mpint_encode(uint8_t *out, const uint8_t *be, size_t n);   /* string-encoded; out needs n + 5 bytes */

bool str_is(const uint8_t *p, size_t n, const char *s);           /* is the (not NUL-terminated) string exactly s */
bool namelist_has(const uint8_t *list, size_t n, const char *name);
const char *namelist_pick(const uint8_t *client, size_t n, const char *const *ours);   /* the first of the client's names that we offer */

size_t b64_encode(const uint8_t *in, size_t n, char *out, bool pad);     /* returns the length; NUL-terminates */
int    b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);   /* bytes written, or -1 */
/* Text from the peer, made safe for a log line: anything but printable ASCII becomes '?', and it is cut to fit. */
void   printable(char *out, size_t cap, const uint8_t *p, size_t n);
void   random_bytes(void *p, size_t n);                                    /* from the kernel's generator */

/* ------------------------------------------------------------------------------------------------ logging */

void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------------------------------------ transport.c */

typedef struct Conn {
    int fd;
    uint32_t seq_in, seq_out;
    bool enc_in, enc_out;                           /* the keys of a finished key exchange are in use */
    uint8_t main_in[32], hdr_in[32], main_out[32], hdr_out[32];
    bool strict;                                    /* OpenSSH's strict key exchange (Terrapin countermeasure) */
    bool kex_done;                                  /* the first key exchange has finished */
    uint8_t session_id[32];
    char client_id[256], server_id[64];
    uint8_t ic[PKT_MAX], is[1024];                  /* the two KEXINIT payloads, part of the exchange hash */
    size_t ic_len, is_len;
    long deadline;                                  /* absolute time (ms) by which a read must be done; 0 = only the per-packet stall limit */
    char peer[48];                                  /* "10.0.2.2" */
    unsigned peer_port;
    char local[48];
    unsigned local_port;
    uint32_t last_seq;                              /* the sequence number of the packet next_msg returned last */
    long last_rx;                                   /* when (mono_ms) the last packet arrived */
    unsigned keepalive_ms;                          /* ask a silent client this often whether it is there; three silent rounds end the connection (0: never) */
} Conn;

extern const char SERVER_ID[];

int  transport_start(Conn *c);                      /* identification strings and the first key exchange; 0 or -1 */
/* The next message from the peer. Returns the payload length (>= 1) and sets *payload (valid until the next call), 0 when the peer disconnected,
 * -1 on an error (the connection is unusable). next_msg() waits as long as it takes, swallowing ignore/debug/unimplemented messages and doing a
 * peer-started re-keying on the way. next_msg_step() reads one packet only and returns NEXT_SWALLOWED when that was such a message: the session
 * loop needs the chance to look at its programs again after every packet. */
#define NEXT_SWALLOWED (-2)
int  next_msg(Conn *c, const uint8_t **payload);
int  next_msg_step(Conn *c, const uint8_t **payload);
int  send_msg(Conn *c, const uint8_t *payload, size_t n);       /* 0 or -1 */
void send_disconnect(Conn *c, uint32_t reason, const char *text);
void send_unimplemented(Conn *c);                    /* answers the packet next_msg returned last */

/* ------------------------------------------------------------------------------------------------ auth.c */

typedef struct { char name[32]; int uid, gid; char home[96], shell[64]; } Account;

extern uint8_t g_host_pk[32];
extern uint8_t g_host_blob[51];                      /* string "ssh-ed25519", string key */
extern char    g_host_fp[64];                        /* "SHA256:..." as ssh-keygen -l prints it */

void host_key_init(void);                            /* loads or makes the host key; logs the fingerprint */
void host_key_sign(uint8_t sig[64], const uint8_t *msg, size_t n);
void host_key_forget(void);                          /* in a forked helper that does not exec: the private key leaves its memory */
/* Runs the user-authentication phase. 0 with *who filled in when the peer got in, -1 when the connection is over (already told why). */
int  auth_run(Conn *c, Account *who);

/* ------------------------------------------------------------------------------------------------ session.c */

int  session_run(Conn *c, const Account *who);       /* the connection phase; returns when the peer is gone */

/* ------------------------------------------------------------------------------------------------ forward.c */

enum { FWD_OFF, FWD_LOCAL, FWD_ANY };
enum { RFWD_OFF, RFWD_LOCAL };
extern int g_fwd_policy;                             /* sshd.forward=off|local|any (local unless said) */
extern int g_rfwd_policy;                            /* sshd.remoteforward=off|local (off unless said): ssh -R, listening on the loopback */
extern int g_fwd_max;                                /* sshd.forwardmax=N: forwarded channels per connection (1 .. FWD_MAX_HARD) */
extern int g_fwd_total;                              /* sshd.forwardtotal=N: forwarded channels of the whole server (0: no such cap) */
#define FWD_MAX_HARD 32                              /* (the poll set of a connection holds at most 64 descriptors) */
#define FWD_LISTENERS 4                              /* ssh -R ports per connection */
#define FWD_TOTAL_MAX 1024
#define FWD_BASE 0x100u                              /* our channel numbers from here on are forwards; the session channels are below */

void fwd_setup(void);                               /* the listening process: the directory of the server-wide count, and a log line with the policy */
void fwd_reaped(pid_t pid);                          /* the listening process: a connection process has ended, whatever it held is free */
void fwd_begin(Conn *c, const Account *who);         /* the connection phase begins */
int  fwd_open(Conn *c, Rd *r, uint32_t sender, uint32_t win, uint32_t max);    /* a "direct-tcpip" open; r is after the common fields */
int  fwd_global(Conn *c, bool cancel, Rd *r, bool want);     /* a "tcpip-forward" (or "cancel-...") request; r is after want-reply */
bool fwd_owns(uint32_t id);                          /* is our channel number id a forward */
int  fwd_message(Conn *c, uint8_t type, Rd *r);      /* a channel message for a forward; r is after the message type */
int  fwd_poll_add(struct pollfd *pf, int n, int cap);   /* adds the forwards' descriptors from pf[n] on; returns the new count */
int  fwd_service(Conn *c, const struct pollfd *pf);  /* after poll(): connects, lookups, bytes, ends; -1 when the connection is unusable */
bool fwd_busy(void);                                 /* a lookup or a connect is under way (its deadline wants a short poll) */
void fwd_end(void);                                  /* the connection is over: everything is closed */

/* ------------------------------------------------------------------------------------------------ main.c */

int  serve(int fd);                                   /* one connection, in the child process; returns when it is over: 0 after a login, SERVE_NO_LOGIN (3) when none happened */

#endif
