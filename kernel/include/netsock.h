/*
 * AF_INET sockets: the Sock object shared by TCP, UDP and the ICMP "ping" sockets, and the entry points kernel/fs/socket.c
 * calls for the socket system calls when the descriptor is an Internet socket. Everything here runs with the net lock held.
 */
#ifndef K_NETSOCK_H
#define K_NETSOCK_H

#include "net.h"
#include "vfs.h"

enum { SK_TCP = 1, SK_UDP = 2, SK_ICMP = 3 };

/* TCP states (RFC 793); numbering follows Linux's /proc/net/tcp "st" column */
enum {
    TCPS_ESTABLISHED = 1, TCPS_SYN_SENT = 2, TCPS_SYN_RCVD = 3, TCPS_FIN_WAIT1 = 4, TCPS_FIN_WAIT2 = 5, TCPS_TIME_WAIT = 6,
    TCPS_CLOSED = 7, TCPS_CLOSE_WAIT = 8, TCPS_LAST_ACK = 9, TCPS_LISTEN = 10, TCPS_CLOSING = 11,
};

typedef struct Dgram {                  /* a queued UDP datagram (or whole ICMP packet) */
    struct Dgram *next;
    u32  saddr;
    u16  sport;
    u32  len;
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
    u8   ttl, tos;
    u32  laddr, raddr;
    u16  lport, rport;
    int  err;                           /* pending socket error, positive errno (SO_ERROR) */
    u32  rcvtimeo, sndtimeo;            /* milliseconds, 0 = wait forever */
    u32  uid;
    u32  ino;
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

/* sock.c */
Sock *sock_new(int kind);
void  sock_unlink_free(Sock *s);                        /* removes from the list and frees (and its buffers) */
u16   sock_alloc_port(int kind);
bool  sock_port_conflict(Sock *me, u32 addr, u16 port);
void  sock_wake(Sock *s);
bool  sock_exists(Sock *s);

ssize_t sk_recv(File *f, u8 *buf, size_t n, int mflags, u32 *from_addr, u16 *from_port);
ssize_t sk_send(File *f, const u8 *buf, size_t n, int mflags, bool have_addr, u32 daddr, u16 dport);
int   sk_block(Sock *s, u32 tmo, u64 *deadline);          /* sleeps on the socket with the lock dropped; 0, -EAGAIN (timeout), -EINTR */
int   sk_get_sockaddr(u64 uaddr, u64 len, u32 *addr, u16 *port);
int   sk_put_sockaddr(u64 uaddr, u64 ulenp, u32 addr, u16 port);
extern const VNodeOps g_inet_vn_ops;

/* tcp.c */
void  tcp_sock_init(Sock *s);
int   tcp_connect(Sock *s, u32 raddr, u16 rport);        /* sends the SYN; the caller waits for the state to change */
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
long inet_bind(File *f, u64 uaddr, u64 len);
long inet_listen(File *f, int backlog);
long inet_accept(File *f, u64 uaddr, u64 ulenp);
long inet_connect(File *f, u64 uaddr, u64 len);
long inet_shutdown(File *f, int how);

#endif
