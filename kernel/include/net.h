/*
 * Networking: packet buffers, network devices, the one big network lock, IPv4 / ICMP / UDP / TCP and the AF_INET sockets.
 * Yukari opens the gaps between machines. See docs/NETWORK.md for the design.
 *
 * Concurrency: one Mutex (net_lock) protects the whole stack. System calls take it while they touch socket state and drop it
 * (net_wait) while they sleep; the "Yukari" kernel thread takes it to poll the devices and run the timers. Device drivers
 * are therefore only ever entered with the lock held and need no locking of their own.
 */
#ifndef K_NET_H
#define K_NET_H

#include "types.h"
#include "task.h"
#include "abi.h"

/* ---- byte order and addresses (addresses and ports are kept in host order inside the kernel) ---- */
static inline u16 hton16(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntoh16(u16 v) { return hton16(v); }
static inline u32 hton32(u32 v) { return __builtin_bswap32(v); }
static inline u32 ntoh32(u32 v) { return __builtin_bswap32(v); }
static inline u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

#define IP4(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
size_t net_ip_str(u32 ip, char *buf, size_t n);         /* "10.0.2.15" */
bool   net_ip_parse(const char *s, u32 *out);

/* ---- packet buffers ---- */
#define NB_SIZE     2048
#define NB_HEAD     80                  /* headroom reserved in front of fresh buffers for headers */
#define NB_MAXFRAME 1514                /* largest Ethernet frame we build (MTU 1500 + header) */

typedef struct NetBuf {
    struct NetBuf *next;
    struct NetDev *dev;                 /* device it arrived on / leaves by */
    u16 off, len;                       /* data is buf[off .. off+len) */
    u32 pad;
    u8  buf[NB_SIZE - 32];
} NetBuf;

NetBuf *nb_alloc(void);                 /* empty buffer with NB_HEAD bytes of headroom, NULL when memory is short */
void    nb_free(NetBuf *nb);
static inline u8 *nb_data(NetBuf *nb) { return nb->buf + nb->off; }
static inline u8 *nb_put(NetBuf *nb, size_t n) { u8 *p = nb->buf + nb->off + nb->len; nb->len = (u16)(nb->len + n); return p; }
static inline u8 *nb_push(NetBuf *nb, size_t n) { nb->off = (u16)(nb->off - n); nb->len = (u16)(nb->len + n); return nb->buf + nb->off; }
static inline void nb_pull(NetBuf *nb, size_t n) { nb->off = (u16)(nb->off + n); nb->len = (u16)(nb->len - n); }
static inline size_t nb_room(NetBuf *nb) { return sizeof nb->buf - nb->off - nb->len; }

/* ---- the lock ---- */
void net_lock(void);
void net_unlock(void);
#define NET_FOREVER (~0ULL)
/* Sleeps on wq with the net lock released; returns 0 when woken, -EINTR if a signal arrived, -ETIMEDOUT after ms. */
int  net_wait(WaitQueue *wq, u64 ms);
void net_kick(void);                    /* wake the Yukari thread: there is work queued for it */
u64  net_now(void);                     /* uptime in milliseconds */

/* ---- network devices ---- */
#define NDF_UP        0x01
#define NDF_BROADCAST 0x02
#define NDF_LOOPBACK  0x08
#define NDF_LINK      0x40              /* carrier present */
#define NDF_NOARP     0x80
#define NDF_DHCP      0x10000

typedef struct NetDev {
    struct NetDev *next;
    char name[16];
    char driver[16];
    u8   mac[6];
    u32  mtu;
    u32  flags;
    u32  addr, mask, bcast;             /* IPv4 configuration, 0 = none */
    int  index;
    int  (*tx)(struct NetDev *d, NetBuf *nb);   /* takes ownership of the frame; 0 or -errno; lock held */
    void (*poll)(struct NetDev *d);             /* receive whatever has arrived (net_rx), refresh the link; lock held */
    void *priv;
    u64  rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped, tx_dropped;
    void *dhcp;                         /* dhcp.c state */
} NetDev;

NetDev *netdev_register(const char *prefix, NetDev *d);      /* names it eth0, eth1, ...; the caller allocates d (kzalloc) */
NetDev *netdev_find(const char *name);
NetDev *netdev_by_index(int i);
NetDev *netdev_loopback(void);
NetDev *netdev_for_addr(u32 ip);                             /* the device that owns this local address, or NULL */
bool    net_is_local(u32 ip);
void    net_rx(NetDev *d, NetBuf *nb);                       /* a frame came in: hand it to the stack (takes ownership) */
void    net_set_link(NetDev *d, bool up);                    /* drivers report carrier changes here */
int     netdev_set_addr(NetDev *d, u32 addr, u32 mask);      /* also maintains the connected route */
bool    net_have_hardware(void);

/* ---- Ethernet / ARP ---- */
#define ETH_HLEN    14
#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
int  eth_output(NetDev *d, u32 nexthop, u16 ethertype, NetBuf *nb);   /* prepends the Ethernet header, resolves ARP */
void arp_input(NetDev *d, const u8 *p, size_t len);
void arp_tick(u64 now);
int  arp_list(int index, struct gnet_arp *out);
int  arp_set(u32 ip, const u8 *mac);
int  arp_del(u32 ip);
void arp_announce(NetDev *d);

/* ---- IPv4 ---- */
#define IP_HLEN     20
typedef struct Route {
    u32 dst, mask, gw;
    NetDev *dev;
    u32 metric;
    bool used;
} Route;

u16  net_csum(const void *data, size_t n, u32 init);                  /* folded 16-bit sum in little-endian word order; ~sum is the field value */
u32  net_pseudo_csum(u32 src, u32 dst, u8 proto, u16 len);            /* unfolded pseudo-header sum to start from */
void ip_input(NetDev *d, NetBuf *nb);
int  ip_output(u32 src, u32 dst, u8 proto, u8 ttl, u8 tos, bool df, NetBuf *nb);   /* payload at nb->off; src 0 = pick one */
int  ip_output_buf(u32 src, u32 dst, u8 proto, u8 ttl, u8 tos, bool df, const u8 *payload, size_t len);   /* any size: fragments */
extern const u8 *g_net_rx_ip;                                         /* the IP header (and 8 payload bytes) of the packet being delivered */
extern size_t g_net_rx_iplen;
int  ip_send_on(NetDev *dev, u32 src, u32 dst, u8 proto, NetBuf *nb);   /* bypasses routing: DHCP before the interface has an address */
Route *ip_route(u32 dst);
u32  ip_source_for(u32 dst);                                          /* the address we would send from; 0 = unroutable */
int  route_add(u32 dst, u32 mask, u32 gw, NetDev *d, u32 metric);
int  route_del(u32 dst, u32 mask);
int  route_list(int index, struct gnet_route *out);
void route_flush_dev(NetDev *d);
void ip_tick(u64 now);

void icmp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst);
void icmp_send_unreachable(const u8 *orig_ip, size_t len, u8 code);   /* orig_ip: the offending IP packet (header first) */

/* ---- transports and sockets ---- */
void udp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst);
void tcp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst);
void tcp_tick(u64 now);
void udp_error(u32 laddr, u16 lport, u32 raddr, u16 rport, int err);  /* ICMP told us a datagram we sent did not arrive */
void tcp_error(u32 laddr, u16 lport, u32 raddr, u16 rport, int err);
void icmp_sock_deliver(const u8 *ip, size_t len);                     /* echo replies and errors for ICMP sockets (whole IP packet) */
size_t net_proc_tcp(char *buf, size_t cap);
size_t net_proc_udp(char *buf, size_t cap);

/* ---- configuration / tunables ---- */
long net_ctl(unsigned long cmd, void *arg);                           /* SYS_netctl backend; arg already in kernel memory */
void dhcp_start(NetDev *d);
void dhcp_tick(u64 now);
void dhcp_input(NetDev *d, const u8 *p, size_t len);                  /* a datagram for port 68 */
size_t dhcp_dns(u32 *servers, size_t max);                            /* name servers learned from DHCP */
extern int g_net_lo_drop, g_net_lo_dup, g_net_lo_reorder, g_net_lo_corrupt;   /* fault injection on lo, percent */
extern int g_net_rto_min_ms, g_net_syn_retries, g_net_data_retries, g_net_timewait_ms;
extern u64 g_net_stat_ip_bad, g_net_stat_tcp_bad;

#endif
