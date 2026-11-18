/*
 * AF_INET and AF_INET6 sockets: the Sock object shared by TCP, UDP and the ICMP "ping" sockets, and the entry points kernel/fs/socket.c calls for the socket system calls when
 * the descriptor is an Internet socket. Everything here runs with the net lock held.
 *
 * A socket has a family (what the program sees: AF_INET or AF_INET6) and a binding that matches IPv4 packets, IPv6 packets or both: an AF_INET6 socket bound to :: matches both
 * (dual-stack, IPV6_V6ONLY off, as on Linux), one bound to ::ffff:a.b.c.d is an IPv4 socket whose addresses are shown mapped, one bound to another address is an IPv6 socket.
 * A connection also remembers which protocol it runs over (wire6): an IPv4 connection accepted by a dual-stack listener keeps IPv4 addresses and shows them mapped.
 */
#ifndef K_NETSOCK_H
#define K_NETSOCK_H

#include "net.h"
#include "net6k.h"
#include "vfs.h"

enum { SK_TCP = 1, SK_UDP = 2, SK_ICMP = 3, SK_ICMP6 = 4 };

/* TCP states (RFC 793); numbering follows Linux's /proc/net/tcp "st" column */
enum {
    TCPS_ESTABLISHED = 1, TCPS_SYN_SENT = 2, TCPS_SYN_RCVD = 3, TCPS_FIN_WAIT1 = 4, TCPS_FIN_WAIT2 = 5, TCPS_TIME_WAIT = 6,
    TCPS_CLOSED = 7, TCPS_CLOSE_WAIT = 8, TCPS_LAST_ACK = 9, TCPS_LISTEN = 10, TCPS_CLOSING = 11,
};

/* A socket address inside the kernel: IPv4 (host order) or IPv6, a port (host order), and a scope id for a link-local IPv6 address. A v4-mapped address of a program is IPv4 here. */
typedef struct SockAddr {
    u8   fam;                           /* AF_INET or AF_INET6 */
    u16  port;
    u32  a4;
    In6  a6;
    u32  scope;
} SockAddr;

/* The two ends of a packet as they travel: IPv4 or IPv6 addresses (the source and destination of this packet, not "local" and "remote"). */
typedef struct Wire {
    bool    v6;
    u32     src4, dst4;
    In6     src6, dst6;
    u32     scope;                      /* the scope id of a link-local peer (the card it is reached through) */
    NetDev *dev;                        /* the card a received packet came in on */
} Wire;

typedef struct Dgram {                  /* a queued UDP datagram (or whole ICMP packet / ICMPv6 message) */
    struct Dgram *next;
    u32  saddr;
    u16  sport;
    u32  len;
    bool v6;                            /* the sender is saddr6 (with scope), not saddr */
    u32  scope;
    In6  saddr6;
    u8   data[];
} Dgram;

#define TCP_BUF      131072             /* send and receive ring size (power of two) */
#define TCP_OOO      8                  /* out-of-order intervals remembered per connection */

typedef struct Sock {
    struct Sock *next, *prev;           /* every socket in the system */
    u8   kind;                          /* SK_* */
    u8   state;                         /* TCPS_* for TCP; 0 otherwise */
    bool has_file;                      /* an open descriptor refers to it (false: orphaned by close, or not yet accepted) */
    bool bound;
    bool reuseaddr, broadcast, nodelay, keepalive;
    bool rd_shut, wr_shut;              /* the peer's FIN arrived (rd_shut) / we shut down our sending side (wr_shut) */
    bool rd_shut_user;                  /* shutdown(SHUT_RD) or close(): reads return end-of-file */
    bool connected;                     /* UDP: connect() set a default peer */
    u8   ttl, tos;                      /* the hop limit and traffic class of IPv6 sockets live here too (IPV6_UNICAST_HOPS, IPV6_TCLASS) */
    u32  laddr, raddr;                  /* the IPv4 addresses (host order): the binding, or the two ends of a connection over IPv4 */
    u16  lport, rport;
    /* IPv6 */
    u8   family;                        /* 0 and AF_INET: an IPv4 socket; AF_INET6: it sees addresses as IPv6 (an IPv4 peer shows as ::ffff:a.b.c.d) */
    bool v6only;                        /* IPV6_V6ONLY: a socket bound to :: takes no IPv4 packets */
    bool m4, m6;                        /* an AF_INET6 socket's binding matches IPv4 packets (laddr) and/or IPv6 packets (laddr6); an AF_INET socket always matches IPv4 */
    bool wire6;                         /* the connection (or connected datagram socket) runs over IPv6: laddr6 and raddr6 are the ends, not laddr and raddr */
    In6  laddr6, raddr6;
    u32  scope;                         /* sin6_scope_id: the card of a link-local binding or peer (card index + 1) */
    int  err;                           /* pending socket error, positive errno (SO_ERROR) */
    u32  rcvtimeo, sndtimeo;            /* milliseconds, 0 = wait forever */
    u32  uid;
    u32  ino;
    /* options a program set and reads back; they change nothing here (no keep-alive probes are sent, closing never waits or resets because of a linger) */
    u16  ka_idle, ka_intvl;             /* TCP_KEEPIDLE, TCP_KEEPINTVL in seconds; 0 = the Linux defaults (7200, 75) */
    u8   ka_cnt;                        /* TCP_KEEPCNT; 0 = 9 */
    s32  linger_on, linger_sec;         /* SO_LINGER */
    bool recvpktinfo, recvhoplimit, recvtclass;   /* IPV6_RECVPKTINFO, IPV6_RECVHOPLIMIT, IPV6_RECVTCLASS: accepted and read back; no control messages are delivered */
    u8   mcast_hops;                    /* IPV6_MULTICAST_HOPS (1 on Linux unless set) */
    bool mcast_loop;                    /* IPV6_MULTICAST_LOOP */
    WaitQueue wq;                       /* everything that can happen to the socket wakes this */

    /* listening sockets */
    int  backlog;
    int  naccept;                       /* completed connections waiting in acceptq */
    int  nsyn;                          /* children still in SYN_RCVD */
    struct Sock *acceptq, *acceptq_tail;
    struct Sock *anext;                 /* link in the listener's accept queue */
    struct Sock *listener;              /* the listening socket a not-yet-accepted child belongs to */

    /* datagram sockets */
    Dgram *rq, *rq_tail;
    u32  rq_bytes;
    u16  icmp_ident;

    /* TCP control block */
    u32  iss, irs;
    u32  snd_una, snd_nxt, snd_max, snd_end, snd_wnd, snd_wl1, snd_wl2;
    u32  rcv_nxt, rd_nxt, rcv_adv;
    u16  mss;
    u8   snd_ws, rcv_ws;
    bool ws_on;
    u32  cwnd, ssthresh;
    u32  srtt, rttvar, rto;
    u64  t_rexmit, t_delack, t_misc;
    u8   nrexmit, dupacks;
    u8   nprobes, npersist;                  /* zero-window probes sent without any answer / probes sent to a socket nobody holds any more (see tcp_rexmit_timeout) */
    bool recovering, persist, rtt_on, ack_now, fin_pending, fin_acked, fin_rcvd, fin_seen;
    int  segs_unacked;
    u32  recover, rtt_seq, fin_seq, fin_rcv_seq; /* fin_seen: a FIN arrived (maybe out of order); fin_rcvd: it was consumed in order */
    u64  rtt_time;
    u8   *sbuf, *rbuf;
    struct { u32 start, end; } ooo[TCP_OOO];
    int  nooo;
} Sock;

extern Sock *g_socks;
extern const FileOps g_inet_fops;

/* What a socket's binding covers (for the conflict check of bind). */
typedef struct SockBind { bool m4, m6; u32 a4; In6 a6; } SockBind;

static inline bool sk_binds4(const Sock *s) { return s->family != AF_INET6 || s->m4; }
static inline bool sk_binds6(const Sock *s) { return s->family == AF_INET6 && s->m6; }

/* Does an IPv4 packet addressed to dst belong to the binding of s? And an IPv6 one (arrived on dev)? */
static inline bool sock_match4(const Sock *s, u32 dst) { return sk_binds4(s) && (s->laddr == 0 || s->laddr == dst); }
static inline bool sock_match6(const Sock *s, const In6 *dst, const NetDev *dev) {
    if (!sk_binds6(s)) return false;
    if (in6_is_any(&s->laddr6)) return true;
    if (!in6_eq(&s->laddr6, dst)) return false;
    return !(s->scope && dev && in6_is_linklocal(dst) && ip6_dev_scope((NetDev *)dev) != s->scope);
}

/* sock.c */
Sock *sock_new(int kind);
void  sock_unlink_free(Sock *s);                        /* removes from the list and frees (and its buffers) */
u16   sock_alloc_port(int kind);
bool  sock_port_conflict(Sock *me, const SockBind *b, u16 port);
void  sock_autobind(Sock *s, u16 port);                 /* gives an unbound socket the port and the default binding of its family (:: and, unless V6ONLY, 0.0.0.0 for AF_INET6) */
void  sock_wake(Sock *s);
bool  sock_exists(Sock *s);

ssize_t sk_recv(File *f, u8 *buf, size_t n, int mflags, u32 *from_addr, u16 *from_port);
ssize_t sk_recv_ex(File *f, u8 *buf, size_t n, int mflags, u32 *from_addr, u16 *from_port, size_t *full);   /* + the full length of a datagram (for MSG_TRUNC) */
ssize_t sk_recv_from(File *f, u8 *buf, size_t n, int mflags, SockAddr *from, size_t *full);                   /* the same with an address of either family */
ssize_t sk_send(File *f, const u8 *buf, size_t n, int mflags, bool have_addr, u32 daddr, u16 dport);
ssize_t sk_send_to(File *f, const u8 *buf, size_t n, int mflags, const SockAddr *to);                         /* to == NULL: the connected peer */
int   sk_block(Sock *s, u32 tmo, u64 *deadline);          /* sleeps on the socket with the lock dropped; 0, -EAGAIN (timeout), -EINTR */
int   sk_get_sockaddr(u64 uaddr, u64 len, u32 *addr, u16 *port);        /* IPv4 only (the first interface of these calls) */
int   sk_put_sockaddr(u64 uaddr, u64 ulenp, u32 addr, u16 port);
int   sk_get_addr(const Sock *s, u64 uaddr, u64 len, SockAddr *out);   /* reads a sockaddr_in or sockaddr_in6 as the socket's family allows; a v4-mapped address becomes IPv4 */
int   sk_get_addr_fam(u8 family, u64 uaddr, u64 len, SockAddr *out);
size_t sk_addr_bytes(u8 family, const SockAddr *a, void *out);            /* the address as the family shows it (16 or 28 bytes); returns the length */
int   sk_put_addr(u8 family, u64 uaddr, u64 ulenp, const SockAddr *a);   /* writes it in the socket's family (an IPv4 address of an AF_INET6 socket is mapped) */
void  sk_local_addr(Sock *s, SockAddr *a);                /* getsockname */
bool  sk_peer_addr(Sock *s, SockAddr *a);                 /* getpeername: false if not connected */
extern const VNodeOps g_inet_vn_ops;

/* tcp.c */
void  tcp_sock_init(Sock *s);
int   tcp_connect(Sock *s, const SockAddr *to);          /* sends the SYN; the caller waits for the state to change */
int   tcp_listen(Sock *s, int backlog);
Sock *tcp_accept_dequeue(Sock *l);
size_t tcp_send_space(Sock *s);
size_t tcp_queue(Sock *s, const u8 *data, size_t n);     /* copies into the send ring and pushes the data out; returns bytes taken */
size_t tcp_readable(Sock *s);
size_t tcp_read(Sock *s, u8 *out, size_t n, bool peek);
void  tcp_close(Sock *s);                                /* the descriptor was closed */
void  tcp_shutdown(Sock *s, int how);
int   tcp_poll(Sock *s);

/* socket system call backends (called from kernel/fs/socket.c) */
long inet_socket(int type, int protocol);
long inet6_socket(int type, int protocol);
long inet_bind(File *f, u64 uaddr, u64 len);
long inet_listen(File *f, int backlog);
long inet_accept(File *f, u64 uaddr, u64 ulenp);
long inet_connect(File *f, u64 uaddr, u64 len);
long inet_shutdown(File *f, int how);

#endif
