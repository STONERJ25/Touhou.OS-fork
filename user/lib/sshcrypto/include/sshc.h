/*
 * sshc: the client side of SSH-2 for TouhouOS (docs/SSH_CLIENT.md). It sits in the sshcrypto library because it is built from the same primitives (SHA-512,
 * X25519, Ed25519, ChaCha20-Poly1305) and the programs that use it (ssh, ssh-keygen, ssh-keyscan, scp, sftp) are in user/bin/ssh*, scp and sftp. Nothing
 * here is shared with the server (user/sbin/sshd), which has its own copy of the wire code: the two were written to be able to disagree, so that a
 * misreading of the RFCs on one side shows up as a failure on the other.
 *
 * Specifications followed: RFC 4250-4254 (architecture, transport, authentication, connection), RFC 5656 and RFC 8709 for the key formats, RFC 4256
 * (keyboard-interactive), RFC 8731 (curve25519-sha256), RFC 4716's cousin the OpenSSH PROTOCOL files (chacha20-poly1305@openssh.com, the strict key
 * exchange extension, PROTOCOL.key for openssh-key-v1) and the SFTP draft version 3 (draft-ietf-secsh-filexfer-02).
 *
 * All names start with sshc_ / Sshc / SSHC_ so that they can never collide with the server's.
 *
 * The library compiles for the host too (-DSSHC_HOST, tests/host/test_sshclient.py): the only places that differ are sshc_random() and the headers.
 */
#ifndef SSHC_H
#define SSHC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------------------------------------ message numbers (RFC 4250, section 4.1) */

enum {
    SSHC_MSG_DISCONNECT = 1, SSHC_MSG_IGNORE = 2, SSHC_MSG_UNIMPLEMENTED = 3, SSHC_MSG_DEBUG = 4, SSHC_MSG_SERVICE_REQUEST = 5,
    SSHC_MSG_SERVICE_ACCEPT = 6, SSHC_MSG_EXT_INFO = 7,
    SSHC_MSG_KEXINIT = 20, SSHC_MSG_NEWKEYS = 21, SSHC_MSG_KEX_ECDH_INIT = 30, SSHC_MSG_KEX_ECDH_REPLY = 31,
    SSHC_MSG_USERAUTH_REQUEST = 50, SSHC_MSG_USERAUTH_FAILURE = 51, SSHC_MSG_USERAUTH_SUCCESS = 52, SSHC_MSG_USERAUTH_BANNER = 53,
    SSHC_MSG_USERAUTH_PK_OK = 60,                       /* also PASSWD_CHANGEREQ and INFO_REQUEST: the number depends on the method */
    SSHC_MSG_USERAUTH_INFO_RESPONSE = 61,
    SSHC_MSG_GLOBAL_REQUEST = 80, SSHC_MSG_REQUEST_SUCCESS = 81, SSHC_MSG_REQUEST_FAILURE = 82,
    SSHC_MSG_CHANNEL_OPEN = 90, SSHC_MSG_CHANNEL_OPEN_CONFIRMATION = 91, SSHC_MSG_CHANNEL_OPEN_FAILURE = 92, SSHC_MSG_CHANNEL_WINDOW_ADJUST = 93,
    SSHC_MSG_CHANNEL_DATA = 94, SSHC_MSG_CHANNEL_EXTENDED_DATA = 95, SSHC_MSG_CHANNEL_EOF = 96, SSHC_MSG_CHANNEL_CLOSE = 97,
    SSHC_MSG_CHANNEL_REQUEST = 98, SSHC_MSG_CHANNEL_SUCCESS = 99, SSHC_MSG_CHANNEL_FAILURE = 100,
};

enum {                                                  /* disconnect reasons (RFC 4253, section 11.1) */
    SSHC_DISC_PROTOCOL_ERROR = 2, SSHC_DISC_KEY_EXCHANGE_FAILED = 3, SSHC_DISC_MAC_ERROR = 5, SSHC_DISC_SERVICE_NOT_AVAILABLE = 7,
    SSHC_DISC_HOST_KEY_NOT_VERIFIABLE = 9, SSHC_DISC_BY_APPLICATION = 11, SSHC_DISC_NO_MORE_AUTH_METHODS = 14,
};

enum {                                                  /* channel open failure reasons (RFC 4254, section 5.1) */
    SSHC_OPEN_ADMIN_PROHIBITED = 1, SSHC_OPEN_CONNECT_FAILED = 2, SSHC_OPEN_UNKNOWN_CHANNEL_TYPE = 3, SSHC_OPEN_RESOURCE_SHORTAGE = 4,
};

/* ------------------------------------------------------------------------------------------------ small things (sshc_util.c) */

/* Readers and writers for the protocol's data types (RFC 4251, section 5). A reader that runs past its end sets `bad` and returns zeros from then on, so a
 * parser can read a whole message and look at `bad` once. A writer works on the caller's buffer and sets `bad` instead of overflowing. */
typedef struct { const uint8_t *p; size_t n; bool bad; } SshcRd;
typedef struct { uint8_t *p; size_t n, cap; bool bad; } SshcWr;

void     sshc_rd_init(SshcRd *r, const void *p, size_t n);
uint8_t  sshc_rd_u8(SshcRd *r);
uint32_t sshc_rd_u32(SshcRd *r);
uint64_t sshc_rd_u64(SshcRd *r);
bool     sshc_rd_bool(SshcRd *r);
const uint8_t *sshc_rd_str(SshcRd *r, size_t *len);         /* points into the input; never NULL */
void     sshc_rd_skip(SshcRd *r, size_t n);

void sshc_wr_init(SshcWr *w, void *buf, size_t cap);
void sshc_wr_u8(SshcWr *w, uint8_t v);
void sshc_wr_u32(SshcWr *w, uint32_t v);
void sshc_wr_u64(SshcWr *w, uint64_t v);
void sshc_wr_bool(SshcWr *w, bool v);
void sshc_wr_raw(SshcWr *w, const void *p, size_t n);
void sshc_wr_str(SshcWr *w, const void *p, size_t n);
void sshc_wr_cstr(SshcWr *w, const char *s);
size_t sshc_mpint(uint8_t *out, const uint8_t *be, size_t n);      /* string-encoded; out needs n + 5 bytes */

bool sshc_str_is(const uint8_t *p, size_t n, const char *s);        /* is the (not NUL-terminated) string exactly s */
bool sshc_namelist_has(const uint8_t *list, size_t n, const char *name);

size_t sshc_b64_encode(const uint8_t *in, size_t n, char *out, bool pad);      /* returns the length; NUL-terminates; out needs 4*((n+2)/3)+1 */
int    sshc_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);    /* bytes written, or -1 (padding optional, whitespace not allowed) */
void   sshc_hex(const uint8_t *in, size_t n, char *out);                       /* lower-case hex, NUL-terminated; out needs 2n+1 */

/* Text from the peer made safe for a terminal or a log: anything but printable ASCII (and, with `keep_nl`, newline and tab) becomes '?', cut to fit. */
void sshc_printable(char *out, size_t cap, const uint8_t *p, size_t n, bool keep_nl);

/* Bounded copies that always terminate. */
void sshc_cpy(char *dst, size_t cap, const char *src);
bool sshc_fmt(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));      /* false when it did not fit */

void sshc_random(void *p, size_t n);                            /* the kernel's generator; stops the program if there is none: never go on with weak numbers */
long sshc_mono_ms(void);                                        /* milliseconds on a clock that never steps (CLOCK_MONOTONIC) */
int  sshc_ct_equal(const void *a, const void *b, size_t n);     /* 1 when equal, constant time */

/* Paths. sshc_home() is $HOME, else the passwd entry, else "/". sshc_expand_path() turns "~" and "~/x" into the home directory and substitutes the
 * tokens %d (home), %u (local user), %h (remote host), %r (remote user), %p (port) and %% ; false when it does not fit or a token is unknown. */
const char *sshc_home(void);
const char *sshc_local_user(void);
bool sshc_expand_path(const char *in, char *out, size_t cap, const char *host, const char *ruser, int port);
/* Makes dir (and its parents) with mode 0700 when missing. 0 or -1. */
int  sshc_mkdir_p(const char *dir);

/* Verbosity: -1 for -q, 0 normally, 1 to 3 for -v, -vv and -vvv. sshc_debug(n, ...) prints a "debugN: ..." line to standard error when sshc_verbose >= n. */
extern int sshc_verbose;
void sshc_debug(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------------------------------------ hashes (sshc_sha1.c) */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } SshcSha1;
void sshc_sha1_init(SshcSha1 *c);
void sshc_sha1_update(SshcSha1 *c, const void *data, size_t n);
void sshc_sha1_final(SshcSha1 *c, uint8_t out[20]);
void sshc_hmac_sha1(uint8_t out[20], const void *key, size_t klen, const void *data, size_t n);      /* RFC 2104; known_hosts' hashed names use it */
void sshc_sha256(uint8_t out[32], const void *data, size_t n);
void sshc_md5(uint8_t out[16], const void *data, size_t n);

/* ------------------------------------------------------------------------------------------------ public keys and known_hosts (sshc_hostkeys.c) */

#define SSHC_BLOB_MAX 1100

/* A public key in the wire form of RFC 4253 section 6.6 (string type, then the key's fields), which is also what ssh-keygen -l hashes. Any type can be
 * held and shown; only ssh-ed25519 can be used to verify or to sign. */
typedef struct { char type[48]; uint8_t blob[SSHC_BLOB_MAX]; size_t blob_len; } SshcPub;

int  sshc_pub_from_blob(const uint8_t *blob, size_t n, SshcPub *k);                          /* 0, or -1 when it is not a key blob */
int  sshc_pub_parse(const char *line, SshcPub *k, char *comment, size_t ccap);               /* "ssh-ed25519 AAAA... comment": 0, or -1 */
void sshc_pub_to_line(const SshcPub *k, const char *comment, char *out, size_t cap);         /* "type base64 comment" without a newline */
void sshc_ed25519_blob(const uint8_t pk[32], uint8_t out[51]);
bool sshc_pub_ed25519(const SshcPub *k, uint8_t pk[32]);                                     /* is it an ssh-ed25519 key; its 32 bytes */
void sshc_fingerprint(const uint8_t *blob, size_t n, char out[64]);                          /* "SHA256:..." as ssh-keygen -l prints it */
void sshc_fingerprint_md5(const uint8_t *blob, size_t n, char out[64]);                      /* "MD5:aa:bb:..." (ssh-keygen -E md5) */
int  sshc_pub_bits(const SshcPub *k);                                                        /* the size ssh-keygen -l shows: 256 for ed25519 */
const char *sshc_pub_label(const SshcPub *k);                                                /* "ED25519", "RSA", "ECDSA", ... */
/* The strongest picture of a key that can be drawn: ssh-keygen -lv's randomart (OpenSSH's "drunken bishop"), 9 lines of 19 characters plus the frame. */
void sshc_randomart(const uint8_t *blob, size_t n, const char *type_label, int bits, char *out, size_t cap);

typedef enum { SSHC_HK_OK, SSHC_HK_NEW, SSHC_HK_CHANGED, SSHC_HK_REVOKED } SshcHkStatus;
typedef struct {
    SshcHkStatus status;
    char file[256];                     /* the file and line of the entry that matched (OK) or that the key contradicts (CHANGED, REVOKED) */
    int line;
    bool other_types;                   /* NEW: the host is known with keys of other types, which this client cannot use */
} SshcHkResult;

/* How a host is written in known_hosts: "name", or "[name]:port" when the port is not 22. */
void sshc_hk_name(const char *host, int port, char *out, size_t cap);
/* Looks the host up in the files (in order) and compares: OK when an entry for the host holds this key; REVOKED when an @revoked line holds it; CHANGED when
 * the host has an entry of the same key type with another key; NEW otherwise. Hashed entries ("|1|salt|hash") and plain ones with wildcards (* ?), several
 * names and !negations are understood; "@cert-authority" lines are ignored. */
SshcHkResult sshc_hk_check(const char *const *files, int nfiles, const char *host, int port, const uint8_t *blob, size_t n);
/* Appends "name keytype base64" (with the name hashed when `hash`), creating the directory (0700) and file (0600) when needed. 0 or -1. */
int  sshc_hk_add(const char *file, const char *host, int port, bool hash, const SshcPub *key);
/* The same decisions on text in memory, for tests and for sshc_hk_check. `file` is only used to fill the result. */
SshcHkResult sshc_hk_check_text(const char *text, size_t n, const char *file, const char *host, int port, const uint8_t *blob, size_t blob_len);
bool sshc_pattern_match(const char *pattern, const char *s);        /* * and ?, case-insensitive: known_hosts and ssh_config patterns */

/* ------------------------------------------------------------------------------------------------ private key files (sshc_keyfile.c) */

/* OpenSSH's own format, "-----BEGIN OPENSSH PRIVATE KEY-----" (PROTOCOL.key): one ed25519 key, plain or encrypted with a passphrase (bcrypt_pbkdf and
 * aes256-ctr / aes192-ctr / aes128-ctr / chacha20-poly1305@openssh.com). The public half is in the clear, so a login can ask the server whether it would accept
 * the key before it asks for the passphrase. */
typedef struct {
    char cipher[40], kdf[16];
    uint8_t salt[64];
    size_t salt_len;
    uint32_t rounds;
    SshcPub pub;
    uint8_t *priv;                      /* the private section as stored (encrypted; for the AEAD cipher with its tag), heap */
    size_t priv_len;
    bool encrypted;
} SshcKeyFile;

typedef struct { uint8_t seed[32]; uint8_t pk[32]; char comment[128]; } SshcEd25519;

int  sshc_keyfile_parse(const char *text, size_t n, SshcKeyFile *kf, const char **err);
int  sshc_keyfile_unlock(const SshcKeyFile *kf, const char *pass, SshcEd25519 *key, const char **err);     /* 0; -1 with err; -2 for a wrong passphrase */
void sshc_keyfile_free(SshcKeyFile *kf);
/* Builds the text of a key file (with a trailing newline) into a malloc()ed string; pass == NULL for no passphrase. `rounds` is bcrypt_pbkdf's (16 as OpenSSH). */
int  sshc_keyfile_make(const SshcEd25519 *key, const char *pass, uint32_t rounds, char **text);
/* Reads a file whole (up to `max` bytes) into malloc()ed memory with a NUL after it; it must be a regular file, and with `check_mode` (private keys) neither
 * group nor others may have any access to it, as OpenSSH insists (checked on the open descriptor). *why gets the reason (static text). NULL on failure. */
char *sshc_read_file(const char *path, size_t max, size_t *len, bool check_mode, const char **why);

/* The primitives behind it, exposed for the tests. */
void sshc_bcrypt_pbkdf(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen, uint8_t *key, size_t klen, unsigned rounds);
typedef struct { uint8_t rk[240]; int rounds; } SshcAes;
void sshc_aes_init(SshcAes *a, const uint8_t *key, int keylen);                                      /* 16, 24 or 32 bytes */
void sshc_aes_encrypt_block(const SshcAes *a, const uint8_t in[16], uint8_t out[16]);
void sshc_aes_ctr(const SshcAes *a, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);      /* big-endian counter, advanced in iv */

/* ------------------------------------------------------------------------------------------------ the transport layer (sshc_conn.c)
 * RFC 4253 with one algorithm set, the one the server (user/sbin/sshd) and every current OpenSSH prefer: key exchange curve25519-sha256 (or its older name
 * @libssh.org), host key ssh-ed25519, cipher chacha20-poly1305@openssh.com in both directions (an AEAD, so no MAC), no compression, and OpenSSH's strict key
 * exchange when the server has it. The socket is non-blocking: packets are encrypted into a send queue that is pushed out whenever the program waits, so a
 * program that waits to read never blocks the server's writes by not writing. */

#define SSHC_PKT_MAX 40000                              /* the largest packet_length accepted (RFC 4253 asks for 35000) */

typedef struct SshcConn SshcConn;
/* Called in the first key exchange with the host's key (its signature already checked). Return 0 to accept the host, anything else to refuse. */
typedef int (*SshcHostKeyFn)(void *ctx, SshcConn *c, const uint8_t *blob, size_t n);

/* ---- channels (RFC 4254): the connection layer's numbered streams, each with a window that keeps one side from outrunning the other. The table lives in the
 * connection; a program opens channels, gives them handlers, and feeds incoming messages to sshc_dispatch(). */

#define SSHC_MAX_CHAN 32
#define SSHC_WINDOW (1u << 20)                          /* what we let the server send before we reopen the window */
#define SSHC_MAXPKT 32768                               /* the largest data packet we take, and send */

typedef struct SshcChan SshcChan;
typedef struct {
    void (*confirmed)(SshcConn *c, SshcChan *ch);                                       /* our CHANNEL_OPEN was accepted */
    void (*open_failed)(SshcConn *c, SshcChan *ch, uint32_t reason, const char *text);   /* ... or refused: the slot is freed after this */
    void (*data)(SshcConn *c, SshcChan *ch, const uint8_t *d, size_t n, uint32_t ext);  /* ext 0: data, 1: stderr. Call sshc_chan_consumed() when the bytes are taken */
    void (*eof)(SshcConn *c, SshcChan *ch);                                             /* the peer will send no more */
    void (*closed)(SshcConn *c, SshcChan *ch);                                          /* both directions are closed: the slot is freed after this */
    bool (*request)(SshcConn *c, SshcChan *ch, const char *name, SshcRd *args, bool want_reply);   /* exit-status and the like; true = success */
    void (*reply)(SshcConn *c, SshcChan *ch, bool ok);                                  /* the answer to a request of ours that asked for one (in order) */
} SshcChanOps;

struct SshcChan {
    bool used, open;                                    /* used: the slot is taken; open: the peer has confirmed it */
    uint32_t id, remote_id;
    uint32_t remote_win, remote_max;                    /* what we may still send, and the largest packet the peer takes */
    uint32_t local_win, unacked;                        /* what the peer may still send; bytes taken by the program but not yet given back */
    bool eof_in, eof_out, close_in, close_out;
    const SshcChanOps *ops;
    void *user;
    int kind;                                           /* the program's own tag */
    int replies_due;                                    /* requests sent that asked for an answer */
};

/* Called for a channel the SERVER opens (forwarded-tcpip, x11, agent). Fill in ch->ops/user and return 0 to accept, or the reason code to refuse. */
typedef int (*SshcOpenFn)(void *ctx, SshcConn *c, const char *type, SshcRd *args, SshcChan *ch);
typedef void (*SshcGlobalReplyFn)(void *ctx, SshcConn *c, bool ok, SshcRd *data);

struct SshcConn {
    int fd;
    char peer[64];                                      /* the address we are connected to, for messages: "10.0.2.2" */
    unsigned peer_port;
    char err[300];                                      /* why the last call failed, in words fit for the user */
    long deadline_ms;                                   /* sshc_mono_ms() by which the handshake must be done (ConnectTimeout); 0 = none */
    SshcHostKeyFn hostkey_fn;
    void *hostkey_ctx;
    uint64_t rekey_bytes;                               /* re-key after this many bytes in both directions together (0: 1 GiB, as OpenSSH's default for this cipher) */
    long rekey_ms;                                      /* ... or after this long (0: never) */
    long last_rx_ms;                                    /* when (sshc_mono_ms) the last packet arrived: ServerAliveInterval looks at it */

    /* the rest is private to the transport */
    uint32_t seq_in, seq_out, last_seq;
    bool enc_in, enc_out, strict, kex_done;
    uint8_t main_in[32], hdr_in[32], main_out[32], hdr_out[32];
    uint8_t session_id[32];
    char client_id[64], server_id[256];
    uint8_t ic[1024], *is;
    size_t ic_len, is_len;
    uint8_t *in, *out;
    uint8_t *txq;
    size_t tx_len, tx_off, tx_cap;
    uint8_t hostkey[SSHC_BLOB_MAX];
    size_t hostkey_len;
    uint64_t bytes_kex;
    long kex_at_ms;
    uint8_t *pend;                                      /* messages that came in while we waited for the server's KEXINIT of a re-key we started */
    size_t pend_len, pend_off, pend_cap;
    uint8_t *cur;                                       /* the message sshc_recv returned last when it came from `pend` */
    size_t cur_cap;

    /* the connection layer */
    SshcChan chan[SSHC_MAX_CHAN];
    SshcOpenFn open_fn;                                 /* channels the server opens; none are accepted without it */
    void *open_ctx;
    struct { SshcGlobalReplyFn fn; void *ctx; } greply[8];     /* global requests that wait for an answer, in order */
    int n_greply;
    uint8_t chunk[SSHC_MAXPKT + 16];                    /* a data message being built */
};

enum { SSHC_RECV_TIMEOUT = -2 };

/* Connects to host:port (IPv4), giving up after timeout_ms (0: the system's own patience). Returns the socket (non-blocking) or -1 with `why` set. */
int  sshc_tcp_connect(const char *host, int port, int timeout_ms, char *why, size_t why_cap, char *ip, size_t ip_cap);
SshcConn *sshc_conn_new(int fd);
void sshc_conn_free(SshcConn *c);                       /* closes the socket */
/* Version exchange, then the first key exchange (calling c->hostkey_fn). 0, or -1 with c->err. */
int  sshc_handshake(SshcConn *c);
/* Queues a message (payload as in RFC 4253 section 6: the message number first) and pushes out what the socket takes. 0, or -1 with c->err. */
int  sshc_send(SshcConn *c, const uint8_t *payload, size_t n);
/* The next message, waiting up to timeout_ms (0: only if data is there, -1: as long as it takes). Transport messages are handled here (ignore, debug, re-keying,
 * disconnect). Returns the length (>= 1), 0 when the peer closed the connection or sent a disconnect (c->err says how), -1 on an error, SSHC_RECV_TIMEOUT when
 * nothing came in time. The payload stays valid until the next call. */
int  sshc_recv(SshcConn *c, const uint8_t **payload, int timeout_ms);
int  sshc_flush(SshcConn *c, int timeout_ms);           /* waits (up to timeout_ms, -1: for ever) until everything queued has been sent: 0, or -1 */
bool sshc_tx_pending(const SshcConn *c);                /* is something waiting for the socket to become writable (poll POLLOUT then, and call sshc_flush(c, 0)) */
bool sshc_rx_pending(const SshcConn *c);               /* is a message waiting that sshc_recv(c, &p, 0) would return without reading the socket */
void sshc_disconnect(SshcConn *c, uint32_t reason, const char *text);      /* tells the server, best effort */
/* Starts a key exchange now if the limits are reached (or `force`); messages that arrive meanwhile are kept for sshc_recv. 0, or -1. */
int  sshc_rekey_if_due(SshcConn *c, bool force);

/* ------------------------------------------------------------------------------------------------ the connection layer (sshc_chan.c) */

/* Opens a channel of `type` (extra: the type-specific part of the message, already encoded). Returns the slot, or NULL when the table is full. The result of the
 * open arrives through ops->confirmed / ops->open_failed once messages are dispatched. */
SshcChan *sshc_chan_open(SshcConn *c, const char *type, const uint8_t *extra, size_t extra_len, const SshcChanOps *ops, void *user);
/* A channel request (RFC 4254, section 5.4); `args` is the encoded type-specific part. With want_reply the answer comes to ops->reply. 0, or -1. */
int  sshc_chan_request(SshcConn *c, SshcChan *ch, const char *name, bool want_reply, const uint8_t *args, size_t n);
/* Sends as much of the data as the peer's window and packet size allow, and returns how many bytes that was (0 when the window is closed). */
size_t sshc_chan_send(SshcConn *c, SshcChan *ch, const void *data, size_t n);
bool sshc_chan_can_send(const SshcConn *c, const SshcChan *ch);        /* is there room: the peer's window is open and not much is queued for the socket */
int  sshc_chan_send_eof(SshcConn *c, SshcChan *ch);
int  sshc_chan_close(SshcConn *c, SshcChan *ch);                      /* our CLOSE; the slot is freed when the peer's has come too */
void sshc_chan_consumed(SshcConn *c, SshcChan *ch, size_t n);         /* the program has taken n received bytes: gives window back to the peer */
int  sshc_global_request(SshcConn *c, const char *name, const uint8_t *args, size_t n, SshcGlobalReplyFn fn, void *ctx);   /* fn != NULL asks for a reply */
bool sshc_chans_open(const SshcConn *c);                              /* is any channel slot in use */
/* Handles one message from sshc_recv: channels, global requests, anything else is answered "unimplemented". 0, or -1 with c->err for a protocol violation. */
int  sshc_dispatch(SshcConn *c, const uint8_t *payload, size_t n);
/* Waits for and dispatches messages for up to timeout_ms (-1: until one has been handled): 1 handled, 0 nothing came, -1 the connection is gone (c->err; a clean
 * disconnect is -1 as well, with the server's words). For the programs that wait for an answer without anything else to do. */
int  sshc_pump(SshcConn *c, int timeout_ms);

/* ------------------------------------------------------------------------------------------------ user authentication (sshc_auth.c)
 * RFC 4252: the methods are tried in the order publickey (the identity files, asking the server first and the user for a passphrase only when the
 * server would take the key), keyboard-interactive (RFC 4256), password. */

typedef struct {
    const char *user;
    const char *const *identity_files;                  /* already expanded paths, in order */
    int n_identities;
    bool pubkey, kbdint, password;                      /* which methods may be tried */
    bool batch;                                         /* never ask the user anything (BatchMode) */
    int max_prompts;                                    /* password attempts (NumberOfPasswordPrompts) */
    const char *host;                                   /* for the prompts: "user@host's password: " */
} SshcAuthOpts;

/* Asks the user a question on the terminal (/dev/tty, else standard input if that is a terminal), without echo unless `echo`. 0, or -1 when there is no terminal
 * to ask on or the user gave up (end of file). Declared here because the authentication and the host-key prompt both use it. */
int  sshc_prompt(const char *prompt, bool echo, char *buf, size_t cap);
extern bool sshc_batch;                                 /* BatchMode: sshc_prompt() refuses at once */

/* 0 when logged in; -1 with c->err ("Permission denied (publickey,password).") otherwise. */
int  sshc_authenticate(SshcConn *c, const SshcAuthOpts *o);

/* ------------------------------------------------------------------------------------------------ an SFTP client (sshc_sftp.c)
 * SFTP protocol version 3 (draft-ietf-secsh-filexfer-02, what every server speaks) on a session channel with the subsystem "sftp". Calls are synchronous; the file
 * transfers keep up to 16 requests of 32 KiB in flight, which is what makes them fast over a link with latency. Paths are the server's: relative ones start in
 * its idea of the home directory. Every call returns 0, or -1 with the reason in sshc_sftp_err(). */

typedef struct SshcSftp SshcSftp;
enum { SSHC_ATTR_SIZE = 1, SSHC_ATTR_UIDGID = 2, SSHC_ATTR_PERM = 4, SSHC_ATTR_TIME = 8 };
typedef struct { uint32_t flags; uint64_t size; uint32_t uid, gid, perm, atime, mtime; } SshcAttrs;
typedef struct { char name[256]; char longname[512]; SshcAttrs a; } SshcDirEnt;
typedef void (*SshcProgress)(void *ctx, uint64_t done, uint64_t total);

SshcSftp *sshc_sftp_start(SshcConn *c);                         /* the channel, the subsystem and the version exchange; NULL with c->err set */
void sshc_sftp_end(SshcSftp *s);                                /* closes the channel */
const char *sshc_sftp_err(const SshcSftp *s);
int  sshc_sftp_stat(SshcSftp *s, const char *path, SshcAttrs *a, bool follow);
int  sshc_sftp_setstat(SshcSftp *s, const char *path, const SshcAttrs *a);
int  sshc_sftp_mkdir(SshcSftp *s, const char *path, uint32_t mode);
int  sshc_sftp_rmdir(SshcSftp *s, const char *path);
int  sshc_sftp_remove(SshcSftp *s, const char *path);
int  sshc_sftp_rename(SshcSftp *s, const char *from, const char *to);
int  sshc_sftp_symlink(SshcSftp *s, const char *target, const char *path);
int  sshc_sftp_readlink(SshcSftp *s, const char *path, char *out, size_t cap);
int  sshc_sftp_realpath(SshcSftp *s, const char *path, char *out, size_t cap);
/* Calls fn for every entry of the directory (not "." and ".."), in the server's order; a nonzero return from fn stops the listing (and is returned). */
int  sshc_sftp_readdir(SshcSftp *s, const char *path, int (*fn)(const SshcDirEnt *e, void *ctx), void *ctx);
/* Copies a remote file to / from an open local descriptor. `total` is only for the progress callback (0: unknown). put creates the remote file with `mode`. */
int  sshc_sftp_get(SshcSftp *s, const char *remote, int local_fd, uint64_t total, SshcProgress pr, void *ctx);
int  sshc_sftp_put(SshcSftp *s, int local_fd, const char *remote, uint32_t mode, uint64_t total, SshcProgress pr, void *ctx);
bool sshc_sftp_dead(const SshcSftp *s);                          /* the channel or the connection is gone */

/* Whole-file and whole-tree copies on top of the SFTP calls (sshc_xfer.c), for scp and sftp. Problems are reported through ui->problem and counted; the functions
 * return how many things could not be copied (0 = all went well). A directory is copied only with `recursive`. */
typedef struct {
    bool recursive, preserve;                           /* -r; -p (keep times and modes) */
    void *ctx;
    void (*problem)(void *ctx, const char *what, const char *why);
    void (*begin)(void *ctx, const char *name);         /* a file is about to be copied (for a progress meter) */
    SshcProgress progress;
    void (*end)(void *ctx, bool ok);
} SshcXfer;

int  sshc_xfer_upload(SshcSftp *s, const SshcXfer *x, const char *local, const char *remote);
int  sshc_xfer_download(SshcSftp *s, const SshcXfer *x, const char *remote, const char *local);
bool sshc_xfer_remote_is_dir(SshcSftp *s, const char *path);
/* Expands a remote path whose last component has * or ?: the matching entries (malloc()ed array of 1024-byte strings), or the path itself when it has none. */
int  sshc_xfer_glob(SshcSftp *s, const char *path, char (**out)[1024], int *count);
const char *sshc_basename(const char *path);
void sshc_join(char *out, size_t cap, const char *dir, const char *name);

/* ------------------------------------------------------------------------------------------------ ssh_config and -o options (sshc_config.c)
 * The command line, ~/.ssh/config and /etc/ssh/ssh_config all say things in the same words; as in OpenSSH the FIRST value obtained for an option is the one
 * used (so the command line, read first, wins over the user's file, which wins over the system's), except for the options that collect a list. */

typedef enum {
    SSHC_O_HOSTNAME, SSHC_O_USER, SSHC_O_PORT, SSHC_O_IDENTITYFILE, SSHC_O_IDENTITIESONLY, SSHC_O_STRICT, SSHC_O_USERKNOWNHOSTS, SSHC_O_GLOBALKNOWNHOSTS,
    SSHC_O_CONNECTTIMEOUT, SSHC_O_SERVERALIVEINTERVAL, SSHC_O_SERVERALIVECOUNTMAX, SSHC_O_BATCHMODE, SSHC_O_PASSWORDAUTH, SSHC_O_PUBKEYAUTH,
    SSHC_O_KBDAUTH, SSHC_O_NUMPROMPTS, SSHC_O_HASHKNOWNHOSTS, SSHC_O_REQUESTTTY, SSHC_O_LOGLEVEL, SSHC_O_ESCAPECHAR, SSHC_O_HOSTKEYALIAS,
    SSHC_O_REKEYLIMIT, SSHC_O_LOCALFORWARD, SSHC_O_REMOTEFORWARD, SSHC_O_DYNAMICFORWARD, SSHC_O_EXITONFORWARDFAILURE, SSHC_O_PROXYJUMP,
    SSHC_O_PREFERREDAUTH, SSHC_O_ADDRESSFAMILY, SSHC_O_CONNECTIONATTEMPTS, SSHC_O_FORWARDAGENT, SSHC_O_FORWARDX11, SSHC_O_SENDENV,
    SSHC_O_COUNT
} SshcOpt;

typedef struct {
    char **vals[SSHC_O_COUNT];
    int n[SSHC_O_COUNT];
} SshcConfig;

void sshc_cfg_free(SshcConfig *cfg);
/* One "Keyword value" (or "Keyword=value") as -o gives it. 0, or -1 with `err` ("Bad configuration option: foo"). */
int  sshc_cfg_set(SshcConfig *cfg, const char *keyword, const char *value, char *err, size_t errcap);
int  sshc_cfg_set_line(SshcConfig *cfg, const char *line, char *err, size_t errcap);
/* Reads a config file for the connection to `host` as `user` (the names as the user typed them; Host and Match sections are applied accordingly). A missing
 * file is no error unless must_exist. 0, or -1 with `err`. */
int  sshc_cfg_load(SshcConfig *cfg, const char *path, const char *host, const char *user, bool must_exist, char *err, size_t errcap);
const char *sshc_cfg_str(const SshcConfig *cfg, SshcOpt o);                       /* the first value, or NULL */
int  sshc_cfg_bool(const SshcConfig *cfg, SshcOpt o, int dflt);                   /* yes/true/on, no/false/off */
long sshc_cfg_int(const SshcConfig *cfg, SshcOpt o, long dflt);
/* "30", "1m", "1h30m": seconds, or -1. "256K", "1G", "4096": bytes, or -1. */
long sshc_parse_time(const char *s);
long long sshc_parse_size(const char *s);
/* Splits a string into words as ssh_config does (blanks separate, double quotes group); returns the count, words copied into `out` (NUL-separated, in `buf`). */
int  sshc_words(const char *s, char *buf, size_t cap, char **out, int max);

/* ------------------------------------------------------------------------------------------------ connect and log in, the way every client program does (sshc_login.c) */

typedef struct {
    const char *host;                                   /* what to connect to (HostName, else the name as typed) */
    const char *name;                                   /* the name known_hosts and the prompts use (the name as typed, or HostKeyAlias) */
    int port;
    const char *user;
    int connect_timeout_s;                              /* ConnectTimeout; 0 leaves it to the system */
    const char *strict;                                 /* StrictHostKeyChecking: "ask", "yes", "no", "accept-new" */
    const char *known_hosts[4];                         /* files that are read; the first is where new keys are written */
    int n_known_hosts;
    bool hash_known_hosts;
    uint64_t rekey_bytes;
    long rekey_ms;
    SshcAuthOpts auth;                                  /* identity files and the methods allowed; user and host are filled in */
} SshcLogin;

/* What a client program needs to connect, worked out from the options (command line, ~/.ssh/config, /etc/ssh/ssh_config) the way ssh(1) does. The strings the
 * SshcLogin points to live in this struct, which must outlive it. */
typedef struct {
    SshcLogin lg;
    char hostname[300];
    char kh[4][512];
    char idfiles[8][512];
    const char *idptr[8];
    char err[300];
} SshcSetup;

/* Reads ~/.ssh/config and /etc/ssh/ssh_config (or only `config_file`, when given; "none" and /dev/null mean no file) for the host typed on the command line. */
int  sshc_load_configs(SshcConfig *cfg, const char *config_file, const char *host, char *err, size_t errcap);
/* Works out the connection from the options: sets sshc_batch and sshc_verbose from BatchMode and LogLevel (unless verbosity was set on the command line: pass
 * `cli_verbose` as -1 for -q, 1 to 3 for -v ... and 0 when nothing was said). 0, or -1 with s->err. */
int  sshc_login_setup(SshcSetup *s, const SshcConfig *cfg, const char *host_typed, int cli_verbose);
/* Splits "[user@]host" in place (the last '@' counts). Returns the host part. */
char *sshc_split_userhost(char *s, const char **user);

/* TCP connection, key exchange (the host key is checked against the known_hosts files with the policy and the questions of ssh(1): unknown hosts are asked about
 * or refused, a changed key is always refused), authentication. Returns the connection, or NULL with the message the user should see in `why`. */
SshcConn *sshc_login(const SshcLogin *lg, char *why, size_t why_cap);

#endif
