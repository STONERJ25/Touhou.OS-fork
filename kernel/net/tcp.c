/*
 * TCP: the RFC 793 state machine with the usual extensions -- MSS and window-scale options, retransmission with RFC 6298
 * round-trip estimation, slow start / congestion avoidance / fast retransmit and recovery (Reno with the NewReno partial-ACK
 * rule), delayed ACKs, out-of-order reassembly in place, zero-window probes, TIME_WAIT.
 *
 * Both byte rings are indexed by sequence number (seq & (TCP_BUF-1)), so out-of-order data lands where it belongs and
 * retransmission reads straight from the send ring. Everything runs with the net lock held.
 */
#include "netsock.h"
#include "kernel.h"
#include "cpu.h"
#include "mm.h"
#include "random.h"

#define SEQ_LT(a, b)  ((s32)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((s32)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((s32)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((s32)((a) - (b)) >= 0)

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

#define RTO_MAX      60000
#define DELACK_MS    40
#define FIN_WAIT2_MS 60000

static void tcp_output(Sock *s);
static void tcp_destroy(Sock *s);
static void tcp_release(Sock *s);
static void tcp_abort(Sock *s, int err, bool send_rst);

/* ------------------------------------------------------------------ small helpers */

static u32 tcp_ack_num(Sock *s) { return s->rcv_nxt + (s->fin_rcvd ? 1 : 0); }

static u32 tcp_rcv_window(Sock *s) {
    u32 used = s->rcv_nxt - s->rd_nxt;
    u32 free_space = used >= TCP_BUF ? 0 : TCP_BUF - used;
    u32 thresh = MIN((u32)TCP_BUF / 2, (u32)s->mss);
    u32 promised = SEQ_GT(s->rcv_adv, s->rcv_nxt) ? s->rcv_adv - s->rcv_nxt : 0;     /* the right edge never moves left */
    u32 w = free_space >= thresh ? free_space : 0;
    if (w < promised) w = promised;
    if (!s->ws_on && w > 65535) w = 65535;
    return w;
}

static u16 tcp_local_mss(u32 laddr, u32 raddr) {
    u32 mtu = 1500;
    if ((raddr >> 24) == 127 || net_is_local(raddr)) mtu = netdev_loopback()->mtu;
    else { Route *r = ip_route(raddr); if (r) mtu = r->dev->mtu; }
    (void)laddr;
    return (u16)(mtu - 40);
}

static void tcp_alloc_bufs(Sock *s) {
    if (!s->sbuf) s->sbuf = kmalloc(TCP_BUF);
    if (!s->rbuf) s->rbuf = kmalloc(TCP_BUF);
}

static void tcp_free_bufs(Sock *s) {
    kfree(s->sbuf);
    s->sbuf = NULL;
    if (!s->has_file || s->rcv_nxt == s->rd_nxt) {          /* data the application has not read yet must outlive the connection */
        kfree(s->rbuf);
        s->rbuf = NULL;
    }
}

void tcp_sock_init(Sock *s) {
    s->state = TCPS_CLOSED;
    s->mss = 536;
    s->rto = 1000;
}

/* ------------------------------------------------------------------ segment output */

static void tcp_emit_raw(u32 src, u32 dst, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags, u16 win, const u8 *opts, size_t optlen,
                         Sock *s, size_t dlen, u8 ttl, u8 tos) {
    NetBuf *nb = nb_alloc();
    if (!nb) return;
    size_t hl = 20 + optlen;
    u8 *t = nb_put(nb, hl + dlen);
    wr16(t, sport); wr16(t + 2, dport);
    wr32(t + 4, seq); wr32(t + 8, ack);
    t[12] = (u8)((hl / 4) << 4);
    t[13] = flags;
    wr16(t + 14, win);
    t[16] = t[17] = 0; t[18] = t[19] = 0;
    if (optlen) memcpy(t + 20, opts, optlen);
    if (dlen) {
        size_t idx = seq & (TCP_BUF - 1);
        size_t first = MIN(dlen, (size_t)TCP_BUF - idx);
        memcpy(t + hl, s->sbuf + idx, first);
        if (dlen > first) memcpy(t + hl + first, s->sbuf, dlen - first);
    }
    u16 c = (u16)~net_csum(t, hl + dlen, net_pseudo_csum(src, dst, IPPROTO_TCP, (u16)(hl + dlen)));
    memcpy(t + 16, &c, 2);
    ip_output(src, dst, IPPROTO_TCP, ttl, tos, true, nb);
}

/* A segment of the connection: acknowledges what we have received, carries `dlen` bytes of the send ring starting at seq. */
static void tcp_emit(Sock *s, u32 seq, u8 flags, size_t dlen) {
    u8 opts[12];
    size_t optlen = 0;
    u32 w = tcp_rcv_window(s);
    if (flags & TH_SYN) {
        opts[0] = 2; opts[1] = 4; wr16(opts + 2, s->mss);
        optlen = 4;
        if (!(flags & TH_ACK) || s->ws_on) {                                  /* a SYN+ACK only echoes window scaling the peer offered */
            opts[4] = 1; opts[5] = 3; opts[6] = 3; opts[7] = s->rcv_ws;       /* NOP, window scale */
            optlen = 8;
        }
        if (w > 65535) w = 65535;
    } else {
        w >>= s->ws_on ? s->rcv_ws : 0;
        if (w > 65535) w = 65535;
        if (flags & TH_ACK) s->rcv_adv = s->rcv_nxt + (w << (s->ws_on ? s->rcv_ws : 0));
    }
    if (flags & TH_ACK) { s->t_delack = 0; s->segs_unacked = 0; s->ack_now = false; }
    tcp_emit_raw(s->laddr, s->raddr, s->lport, s->rport, seq, (flags & TH_ACK) ? tcp_ack_num(s) : 0, flags, (u16)w, opts, optlen, s, dlen,
                 s->ttl ? s->ttl : 64, s->tos);
}

static void tcp_ack_now(Sock *s) {
    tcp_emit(s, s->snd_nxt, TH_ACK, 0);
}

/* A reply to a segment that belongs to no connection (RFC 793 page 36). */
static void tcp_reset_reply(u32 src, u32 dst, u16 sport, u16 dport, u32 seq, u32 ack, u8 flags, size_t dlen) {
    if (flags & TH_RST) return;
    if (flags & TH_ACK) tcp_emit_raw(dst, src, dport, sport, ack, 0, TH_RST, 0, NULL, 0, NULL, 0, 64, 0);
    else {
        u32 a = seq + (u32)dlen + ((flags & TH_SYN) ? 1 : 0) + ((flags & TH_FIN) ? 1 : 0);
        tcp_emit_raw(dst, src, dport, sport, 0, a, TH_RST | TH_ACK, 0, NULL, 0, NULL, 0, 64, 0);
    }
}

static void tcp_send_syn(Sock *s, bool with_ack) {
    tcp_emit(s, s->iss, with_ack ? (TH_SYN | TH_ACK) : TH_SYN, 0);
}

/* ------------------------------------------------------------------ timers */

static void tcp_arm_rexmit(Sock *s) {
    s->t_rexmit = net_now() + s->rto;
}

static void tcp_rtt_sample(Sock *s, u32 r) {
    if (!s->srtt) {
        s->srtt = r ? r : 1;
        s->rttvar = s->srtt / 2;
    } else {
        u32 delta = s->srtt > r ? s->srtt - r : r - s->srtt;
        s->rttvar = (3 * s->rttvar + delta) / 4;
        s->srtt = (7 * s->srtt + r) / 8;
    }
    u32 rto = s->srtt + MAX(1u, 4 * s->rttvar);
    if (rto < (u32)g_net_rto_min_ms) rto = (u32)g_net_rto_min_ms;
    if (rto > RTO_MAX) rto = RTO_MAX;
    s->rto = rto;
}

static void tcp_enter_timewait(Sock *s) {
    s->state = TCPS_TIME_WAIT;
    s->t_rexmit = s->t_delack = 0;
    s->t_misc = net_now() + (u64)g_net_timewait_ms;
    tcp_free_bufs(s);
    sock_wake(s);
}

/* ------------------------------------------------------------------ output engine */

static void tcp_output(Sock *s) {
    switch (s->state) {
    case TCPS_ESTABLISHED: case TCPS_CLOSE_WAIT: case TCPS_FIN_WAIT1: case TCPS_CLOSING: case TCPS_LAST_ACK: break;
    default: return;
    }
    if (!s->sbuf) return;
    for (int guard = 0; guard < 4096; guard++) {
        u32 flight = s->snd_nxt - s->snd_una;
        u32 wnd = MIN(s->snd_wnd, s->cwnd);
        u32 data_unsent = SEQ_LT(s->snd_nxt, s->snd_end) ? s->snd_end - s->snd_nxt : 0;
        bool fin_unsent = s->fin_pending && s->snd_nxt == s->fin_seq && data_unsent == 0;
        if (!data_unsent && !fin_unsent) break;
        u32 win = wnd > flight ? wnd - flight : 0;
        u32 len = MIN(data_unsent, MIN(win, (u32)s->mss));
        if (len == 0 && data_unsent) {                          /* the window is shut: arm the probe timer if nothing is in flight */
            if (s->snd_wnd == 0 && flight == 0 && !s->t_rexmit) { s->persist = true; s->t_rexmit = net_now() + s->rto; }
            break;
        }
        if (len < s->mss && len == data_unsent && flight > 0 && !s->nodelay && !s->fin_pending) break;      /* Nagle */
        if (len < s->mss && len < data_unsent && flight > 0) break;                                             /* silly window avoidance */
        bool fin = s->fin_pending && len == data_unsent && s->snd_nxt + len == s->fin_seq;
        u8 flags = TH_ACK | ((len && len == data_unsent) ? TH_PSH : 0) | (fin ? TH_FIN : 0);
        bool retransmit = SEQ_LT(s->snd_nxt, s->snd_max);
        tcp_emit(s, s->snd_nxt, flags, len);
        u32 adv = len + (fin ? 1 : 0);
        if (!retransmit && !s->rtt_on && adv) { s->rtt_on = true; s->rtt_seq = s->snd_nxt + adv; s->rtt_time = net_now(); }
        if (retransmit && s->rtt_on) s->rtt_on = false;         /* Karn: never time a retransmitted segment */
        s->snd_nxt += adv;
        if (SEQ_GT(s->snd_nxt, s->snd_max)) s->snd_max = s->snd_nxt;
        if (!s->t_rexmit) { s->persist = false; tcp_arm_rexmit(s); }
        if (!adv) break;
    }
}

/* Retransmits one segment starting at snd_una (fast retransmit and NewReno partial ACKs). */
static void tcp_retransmit_head(Sock *s) {
    u32 save = s->snd_nxt;
    u32 data = SEQ_LT(s->snd_una, s->snd_end) ? s->snd_end - s->snd_una : 0;
    u32 len = MIN(data, (u32)s->mss);
    bool fin = s->fin_pending && len == data && s->snd_una + len == s->fin_seq;
    if (!len && !fin) return;
    tcp_emit(s, s->snd_una, TH_ACK | (fin ? TH_FIN : 0), len);
    s->rtt_on = false;
    s->snd_nxt = save;
}

static void tcp_rexmit_timeout(Sock *s) {
    s->t_rexmit = 0;
    if (s->state == TCPS_SYN_SENT || s->state == TCPS_SYN_RCVD) {
        if (++s->nrexmit > g_net_syn_retries) { tcp_abort(s, ETIMEDOUT, false); return; }
        s->rto = MIN(s->rto * 2, (u32)RTO_MAX);
        tcp_send_syn(s, s->state == TCPS_SYN_RCVD);
        tcp_arm_rexmit(s);
        return;
    }
    u32 flight = s->snd_nxt - s->snd_una;
    if (s->persist || (s->snd_wnd == 0 && flight == 0)) {         /* zero-window probe: an ACK that asks for the current window */
        if (s->snd_wnd > 0) { s->persist = false; tcp_output(s); return; }
        s->rto = MIN(MAX(s->rto, 500u) * 2, (u32)RTO_MAX);
        tcp_emit(s, s->snd_una - 1, TH_ACK, 0);
        s->persist = true;
        s->t_rexmit = net_now() + s->rto;
        return;
    }
    if (flight == 0) return;
    if (++s->nrexmit > g_net_data_retries) { tcp_abort(s, ETIMEDOUT, true); return; }
    s->ssthresh = MAX(MIN(flight, s->cwnd) / 2, 2u * s->mss);
    s->cwnd = s->mss;
    s->recovering = false;
    s->dupacks = 0;
    s->rto = MIN(s->rto * 2, (u32)RTO_MAX);
    s->snd_nxt = s->snd_una;                                      /* go back and resend from the first unacknowledged byte */
    s->rtt_on = false;
    tcp_output(s);
    if (!s->t_rexmit) tcp_arm_rexmit(s);
}

void tcp_tick(u64 now) {
    Sock *next;
    for (Sock *s = g_socks; s; s = next) {
        next = s->next;
        if (s->kind != SK_TCP) continue;
        if (s->t_delack && now >= s->t_delack) {
            s->t_delack = 0;
            if (s->state >= TCPS_ESTABLISHED && s->state != TCPS_TIME_WAIT && s->state != TCPS_CLOSED) tcp_ack_now(s);
        }
        if (s->t_rexmit && now >= s->t_rexmit) { tcp_rexmit_timeout(s); if (!sock_exists(s)) continue; }
        if (s->t_misc && now >= s->t_misc) {
            s->t_misc = 0;
            if (s->state == TCPS_TIME_WAIT || (s->state == TCPS_FIN_WAIT2 && !s->has_file)) tcp_release(s);
        }
    }
}

/* ------------------------------------------------------------------ teardown */

static void tcp_unqueue(Sock *s) {                               /* take a child out of its listener's accept queue */
    Sock *l = s->listener;
    if (!l) return;
    Sock *prev = NULL;
    for (Sock *c = l->acceptq; c; prev = c, c = c->anext) {
        if (c != s) continue;
        if (prev) prev->anext = c->anext; else l->acceptq = c->anext;
        if (l->acceptq_tail == c) l->acceptq_tail = prev;
        l->naccept--;
        break;
    }
    s->listener = NULL;
    s->anext = NULL;
}

static void tcp_destroy(Sock *s) {
    tcp_unqueue(s);
    sock_wake(s);
    sock_unlink_free(s);
}

/* The connection is over for good. Free the socket unless a descriptor still points at it (then close() frees it). */
static void tcp_release(Sock *s) {
    if (!s->has_file) { tcp_destroy(s); return; }
    s->state = TCPS_CLOSED;
    s->raddr = 0;
    s->rport = 0;
    s->t_rexmit = s->t_delack = s->t_misc = 0;
    tcp_free_bufs(s);
    sock_wake(s);
}

/* Kills the connection: optionally tell the peer with a RST, tell the owner why, free everything if nobody holds the socket. */
static void tcp_abort(Sock *s, int err, bool send_rst) {
    if (send_rst && s->state != TCPS_SYN_SENT && s->state != TCPS_TIME_WAIT && s->state != TCPS_CLOSED && s->state != TCPS_LISTEN)
        tcp_emit_raw(s->laddr, s->raddr, s->lport, s->rport, s->snd_nxt, tcp_ack_num(s), TH_RST | TH_ACK, 0, NULL, 0, NULL, 0, 64, 0);
    if (err && !s->err) s->err = err;
    if (s->state == TCPS_SYN_RCVD && s->listener && s->listener->nsyn > 0) s->listener->nsyn--;
    s->state = TCPS_CLOSED;
    s->t_rexmit = s->t_delack = s->t_misc = 0;
    s->rd_shut = s->wr_shut = true;
    sock_wake(s);
    if (!s->has_file) tcp_destroy(s);
}

/* ------------------------------------------------------------------ options */

static void tcp_parse_opts(const u8 *o, size_t n, u16 *mss, int *ws) {
    size_t i = 0;
    while (i < n) {
        u8 kind = o[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= n) break;
        u8 l = o[i + 1];
        if (l < 2 || i + l > n) break;                          /* malformed: stop reading options, keep what we have */
        if (kind == 2 && l == 4) *mss = rd16(o + i + 2);
        else if (kind == 3 && l == 3) *ws = MIN(o[i + 2], 14);
        i += l;
    }
}

/* ------------------------------------------------------------------ out-of-order data */

static void ooo_add(Sock *s, u32 start, u32 end) {
    for (int i = 0; i < s->nooo; i++) {
        if (SEQ_LEQ(start, s->ooo[i].end) && SEQ_GEQ(end, s->ooo[i].start)) {      /* overlaps or touches: merge */
            u32 ns = SEQ_LT(start, s->ooo[i].start) ? start : s->ooo[i].start;
            u32 ne = SEQ_GT(end, s->ooo[i].end) ? end : s->ooo[i].end;
            s->ooo[i] = s->ooo[--s->nooo];
            ooo_add(s, ns, ne);
            return;
        }
    }
    if (s->nooo < TCP_OOO) { s->ooo[s->nooo].start = start; s->ooo[s->nooo].end = end; s->nooo++; }
}

static void tcp_absorb(Sock *s) {
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < s->nooo; i++) {
            if (SEQ_LEQ(s->ooo[i].start, s->rcv_nxt)) {
                if (SEQ_GT(s->ooo[i].end, s->rcv_nxt)) s->rcv_nxt = s->ooo[i].end;
                s->ooo[i] = s->ooo[--s->nooo];
                again = true;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ input */

typedef struct Seg {
    u32 src, dst;
    u16 sport, dport;
    u32 seq, ack;
    u8  flags;
    u32 win;
    const u8 *data;
    size_t len;
    u16 mss;
    int ws;
} Seg;

static Sock *tcp_lookup(u32 laddr, u16 lport, u32 raddr, u16 rport, Sock **listener) {
    Sock *l = NULL;
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_TCP || s->lport != lport) continue;
        if (s->state == TCPS_LISTEN) {
            if ((s->laddr == 0 || s->laddr == laddr) && (!l || (s->laddr && !l->laddr))) l = s;
            continue;
        }
        if (s->state == TCPS_CLOSED) continue;
        if (s->raddr == raddr && s->rport == rport && s->laddr == laddr) return s;
    }
    *listener = l;
    return NULL;
}

static void tcp_init_conn(Sock *s, u32 laddr, u32 raddr) {
    s->iss = (u32)random_u64();
    s->snd_una = s->snd_nxt = s->snd_max = s->iss;
    s->snd_end = s->iss + 1;
    s->mss = tcp_local_mss(laddr, raddr);
    s->rcv_ws = 2;
    s->rto = 1000;
    s->srtt = s->rttvar = 0;
    s->ssthresh = 0x7FFFFFFF;
}

static void tcp_apply_peer_opts(Sock *s, const Seg *g) {
    u16 pm = g->mss ? g->mss : 536;
    if (pm < s->mss) s->mss = pm;
    if (s->mss < 64) s->mss = 64;
    if (g->ws >= 0) { s->ws_on = true; s->snd_ws = (u8)g->ws; } else { s->ws_on = false; s->snd_ws = 0; }
    s->cwnd = 10u * s->mss;
}

/* A SYN arrived at a listener: create the child connection and answer with SYN+ACK. */
static void tcp_listen_syn(Sock *l, const Seg *g) {
    if (l->naccept + l->nsyn >= l->backlog) return;               /* queue full: stay silent, the client retransmits its SYN */
    Sock *c = sock_new(SK_TCP);
    if (!c) return;
    tcp_sock_init(c);
    c->laddr = g->dst; c->lport = g->dport; c->raddr = g->src; c->rport = g->sport;
    c->bound = true;
    c->uid = l->uid;
    c->reuseaddr = l->reuseaddr; c->nodelay = l->nodelay; c->keepalive = l->keepalive;
    c->rcvtimeo = l->rcvtimeo; c->sndtimeo = l->sndtimeo;
    c->ttl = l->ttl; c->tos = l->tos;
    tcp_alloc_bufs(c);
    if (!c->sbuf || !c->rbuf) { sock_unlink_free(c); return; }
    tcp_init_conn(c, c->laddr, c->raddr);
    c->irs = g->seq;
    c->rcv_nxt = c->rd_nxt = g->seq + 1;
    c->rcv_adv = c->rcv_nxt;
    c->snd_wnd = g->win;
    tcp_apply_peer_opts(c, g);
    c->state = TCPS_SYN_RCVD;
    c->listener = l;
    l->nsyn++;
    tcp_send_syn(c, true);
    c->rtt_on = true;
    c->rtt_time = net_now();
    c->rtt_seq = c->iss + 1;
    c->snd_nxt = c->iss + 1;
    c->snd_max = c->snd_nxt;
    c->snd_una = c->iss;
    tcp_arm_rexmit(c);
}

static void tcp_established(Sock *s) {
    s->state = TCPS_ESTABLISHED;
    s->nrexmit = 0;
    s->t_rexmit = 0;
    Sock *l = s->listener;
    if (l) {                                                     /* a child: hand it to accept() */
        if (l->nsyn > 0) l->nsyn--;
        s->anext = NULL;
        if (l->acceptq_tail) l->acceptq_tail->anext = s; else l->acceptq = s;
        l->acceptq_tail = s;
        l->naccept++;
        sock_wake(l);
    }
    sock_wake(s);
}

static void tcp_fin_consumed(Sock *s) {
    s->fin_rcvd = true;
    s->rd_shut = true;
    s->ack_now = true;
    switch (s->state) {
    case TCPS_ESTABLISHED: s->state = TCPS_CLOSE_WAIT; break;
    case TCPS_FIN_WAIT1: s->state = s->fin_acked ? TCPS_TIME_WAIT : TCPS_CLOSING; if (s->state == TCPS_TIME_WAIT) { tcp_ack_now(s); tcp_enter_timewait(s); } break;
    case TCPS_FIN_WAIT2: tcp_ack_now(s); tcp_enter_timewait(s); break;
    default: break;
    }
    sock_wake(s);
}

/* Processes the acknowledgement field. Returns false if the segment must be dropped (after any reply was sent). */
static bool tcp_process_ack(Sock *s, const Seg *g) {
    if (SEQ_GT(g->ack, s->snd_max)) { tcp_ack_now(s); return false; }          /* acknowledges something never sent */
    u64 now = net_now();
    bool dup = g->ack == s->snd_una;
    if (SEQ_LT(g->ack, s->snd_una)) return true;                              /* old duplicate, harmless */
    bool win_changed = false;
    if (SEQ_LT(s->snd_wl1, g->seq) || (s->snd_wl1 == g->seq && SEQ_LEQ(s->snd_wl2, g->ack))) {
        u32 w = g->win << s->snd_ws;
        win_changed = w != s->snd_wnd;
        s->snd_wnd = w;
        s->snd_wl1 = g->seq;
        s->snd_wl2 = g->ack;
    }
    if (dup) {
        bool pure = g->len == 0 && !(g->flags & (TH_SYN | TH_FIN));
        if (pure && !win_changed && s->snd_una != s->snd_max) {
            s->dupacks++;
            if (s->dupacks == 3 && !s->recovering) {
                u32 flight = s->snd_nxt - s->snd_una;
                s->ssthresh = MAX(flight / 2, 2u * s->mss);
                s->recover = s->snd_max;
                s->recovering = true;
                tcp_retransmit_head(s);
                s->cwnd = s->ssthresh + 3u * s->mss;
            } else if (s->dupacks > 3 && s->recovering) {
                s->cwnd += s->mss;
                tcp_output(s);
            }
        }
        if (win_changed) { s->persist = false; tcp_output(s); }
        return true;
    }
    /* new data acknowledged */
    u32 acked = g->ack - s->snd_una;
    if (s->rtt_on && SEQ_GEQ(g->ack, s->rtt_seq)) { tcp_rtt_sample(s, (u32)(now - s->rtt_time)); s->rtt_on = false; }
    if (s->recovering) {
        if (SEQ_GEQ(g->ack, s->recover)) {
            s->cwnd = MIN(s->ssthresh, MAX(s->snd_nxt - g->ack, (u32)s->mss) + s->mss);
            s->recovering = false;
            s->dupacks = 0;
        } else {                                                  /* partial ACK: the next hole is lost too */
            s->snd_una = g->ack;
            tcp_retransmit_head(s);
            s->cwnd = s->cwnd > acked ? s->cwnd - acked + s->mss : s->mss;
        }
    } else {
        s->dupacks = 0;
        if (s->cwnd < s->ssthresh) s->cwnd += MIN(acked, (u32)s->mss);
        else s->cwnd += MAX(1u, (u32)s->mss * s->mss / s->cwnd);
        if (s->cwnd > (1u << 26)) s->cwnd = 1u << 26;
    }
    s->snd_una = g->ack;
    if (SEQ_LT(s->snd_nxt, s->snd_una)) s->snd_nxt = s->snd_una;
    s->nrexmit = 0;
    s->persist = false;
    if (s->fin_pending && SEQ_GT(s->snd_una, s->fin_seq)) s->fin_acked = true;
    if (s->snd_una == s->snd_max) s->t_rexmit = 0; else tcp_arm_rexmit(s);
    sock_wake(s);
    return true;
}

static void tcp_state_after_ack(Sock *s) {
    if (!s->fin_acked) return;
    switch (s->state) {
    case TCPS_FIN_WAIT1:
        s->state = TCPS_FIN_WAIT2;
        if (!s->has_file) s->t_misc = net_now() + FIN_WAIT2_MS;      /* nobody will ever read the rest: do not wait forever */
        sock_wake(s);
        break;
    case TCPS_CLOSING: tcp_enter_timewait(s); break;
    case TCPS_LAST_ACK: tcp_abort(s, 0, false); break;
    default: break;
    }
}

static void tcp_segment(Sock *s, const Seg *g) {
    u8 flags = g->flags;
    switch (s->state) {
    case TCPS_SYN_SENT: {
        bool ack_ok = false;
        if (flags & TH_ACK) {
            if (SEQ_LEQ(g->ack, s->iss) || SEQ_GT(g->ack, s->snd_nxt)) { tcp_reset_reply(g->src, g->dst, g->sport, g->dport, g->seq, g->ack, flags, g->len); return; }
            ack_ok = true;
        }
        if (flags & TH_RST) { if (ack_ok) tcp_abort(s, ECONNREFUSED, false); return; }
        if (!(flags & TH_SYN)) return;
        s->irs = g->seq;
        s->rcv_nxt = s->rd_nxt = g->seq + 1;
        s->rcv_adv = s->rcv_nxt;
        s->snd_wnd = g->win;
        s->snd_wl1 = g->seq;
        s->snd_wl2 = g->ack;
        tcp_apply_peer_opts(s, g);
        if (ack_ok) {
            s->snd_una = g->ack;
            if (s->rtt_on) { tcp_rtt_sample(s, (u32)(net_now() - s->rtt_time)); s->rtt_on = false; }
            s->nrexmit = 0;
            tcp_established(s);
            tcp_ack_now(s);
        } else {                                                  /* simultaneous open */
            s->state = TCPS_SYN_RCVD;
            tcp_send_syn(s, true);
        }
        return;
    }
    case TCPS_TIME_WAIT:
        if (flags & TH_RST) { tcp_release(s); return; }
        if (flags & TH_SYN) return;                               /* handled by the caller (a new connection may reuse the tuple) */
        tcp_ack_now(s);
        s->t_misc = net_now() + (u64)g_net_timewait_ms;
        return;
    default: break;
    }

    /* 1. acceptable sequence number? (RFC 793 page 69) */
    u32 seq = g->seq;
    size_t dlen = g->len;
    u32 rnxt = tcp_ack_num(s);
    u32 wl = s->rd_nxt + TCP_BUF;                                 /* first sequence number beyond the buffer */
    u32 rwnd = SEQ_GT(wl, rnxt) ? wl - rnxt : 0;
    u32 seglen = (u32)dlen + ((flags & TH_FIN) ? 1 : 0);
    bool ok;
    if (seglen == 0) ok = rwnd == 0 ? seq == rnxt : (SEQ_GEQ(seq, rnxt) && SEQ_LT(seq, rnxt + rwnd));
    else if (rwnd == 0) ok = false;
    else ok = (SEQ_GEQ(seq, rnxt) && SEQ_LT(seq, rnxt + rwnd)) || (SEQ_GEQ(seq + seglen - 1, rnxt) && SEQ_LT(seq + seglen - 1, rnxt + rwnd));
    if (seglen && SEQ_LT(seq, rnxt) && SEQ_LT(seq + seglen - 1, rnxt)) ok = false;     /* wholly old: a duplicate */
    if (!ok) {
        if (!(flags & TH_RST)) tcp_ack_now(s);                    /* also answers zero-window probes and keepalives */
        return;
    }
    /* 2. RST (RFC 5961: only an exact match tears the connection down) */
    if (flags & TH_RST) {
        if (seq == rnxt) tcp_abort(s, s->state == TCPS_SYN_RCVD ? ECONNREFUSED : ECONNRESET, false);
        else tcp_ack_now(s);
        return;
    }
    /* 3. SYN inside the window: challenge */
    if (flags & TH_SYN) {
        if (s->state == TCPS_SYN_RCVD && seq == s->irs) return;   /* a retransmitted SYN: our SYN+ACK timer handles it */
        tcp_ack_now(s);
        return;
    }
    if (!(flags & TH_ACK)) return;
    /* 4. ACK */
    if (s->state == TCPS_SYN_RCVD) {
        if (SEQ_LEQ(g->ack, s->snd_una) || SEQ_GT(g->ack, s->snd_nxt)) { tcp_reset_reply(g->src, g->dst, g->sport, g->dport, g->seq, g->ack, flags, g->len); return; }
        s->snd_una = g->ack;
        s->snd_wnd = g->win << s->snd_ws;
        s->snd_wl1 = g->seq;
        s->snd_wl2 = g->ack;
        s->snd_max = MAX(s->snd_max, s->snd_nxt);
        tcp_established(s);
    } else if (!tcp_process_ack(s, g)) {
        return;
    }
    tcp_state_after_ack(s);
    if (s->state == TCPS_CLOSED || !sock_exists(s)) return;

    /* 5. data */
    bool data_state = s->state == TCPS_ESTABLISHED || s->state == TCPS_FIN_WAIT1 || s->state == TCPS_FIN_WAIT2;
    const u8 *data = g->data;
    if (dlen && !s->has_file && data_state && s->state != TCPS_ESTABLISHED) {          /* RFC 1122 4.2.2.13: data after close() means the app is gone */
        tcp_abort(s, ECONNRESET, true);
        return;
    }
    if (dlen && data_state) {
        if (SEQ_LT(seq, s->rcv_nxt)) {                            /* part of it we already have */
            u32 trim = s->rcv_nxt - seq;
            if (trim >= dlen) { dlen = 0; s->ack_now = true; }
            else { data += trim; dlen -= trim; seq += trim; }
        }
        if (dlen) {
            u32 room = wl - seq;
            if (dlen > room) { dlen = room; flags &= (u8)~TH_FIN; }
            if (dlen) {
                size_t idx = seq & (TCP_BUF - 1);
                size_t first = MIN(dlen, (size_t)TCP_BUF - idx);
                memcpy(s->rbuf + idx, data, first);
                if (dlen > first) memcpy(s->rbuf, data + first, dlen - first);
                if (seq == s->rcv_nxt) {
                    s->rcv_nxt += (u32)dlen;
                    tcp_absorb(s);
                    if (++s->segs_unacked >= 2) s->ack_now = true;
                    else if (!s->t_delack) s->t_delack = net_now() + DELACK_MS;
                } else {
                    ooo_add(s, seq, seq + (u32)dlen);
                    s->ack_now = true;                           /* a duplicate ACK tells the sender about the hole */
                }
                sock_wake(s);
            }
        }
    } else if (dlen && !data_state) {
        s->ack_now = true;
    }
    /* 6. FIN */
    if ((flags & TH_FIN) && (data_state || s->state == TCPS_CLOSE_WAIT || s->state == TCPS_CLOSING || s->state == TCPS_LAST_ACK)) {
        s->fin_seen = true;
        s->fin_rcv_seq = g->seq + (u32)g->len;
    }
    if ((flags & TH_FIN) && s->fin_rcvd) s->ack_now = true;       /* a retransmitted FIN: our ACK was lost */
    if (s->fin_seen && !s->fin_rcvd && s->rcv_nxt == s->fin_rcv_seq && data_state) tcp_fin_consumed(s);
    if (!sock_exists(s)) return;
    if (s->state != TCPS_CLOSED) {
        if (s->ack_now) tcp_ack_now(s);
        tcp_output(s);
    }
}

void tcp_input(NetDev *d, const u8 *p, size_t len, u32 src, u32 dst) {
    (void)d;
    if (len < 20) { g_net_stat_tcp_bad++; return; }
    size_t off = (size_t)(p[12] >> 4) * 4;
    if (off < 20 || off > len) { g_net_stat_tcp_bad++; return; }
    if (net_csum(p, len, net_pseudo_csum(src, dst, IPPROTO_TCP, (u16)len)) != 0xFFFF) { g_net_stat_tcp_bad++; return; }
    if (src == 0xFFFFFFFFu || dst == 0xFFFFFFFFu || (dst >> 28) == 14 || (src >> 28) == 14) return;
    Seg g;
    g.src = src; g.dst = dst;
    g.sport = rd16(p); g.dport = rd16(p + 2);
    g.seq = rd32(p + 4); g.ack = rd32(p + 8);
    g.flags = p[13];
    g.win = rd16(p + 14);
    g.data = p + off;
    g.len = len - off;
    g.mss = 0;
    g.ws = -1;
    if (off > 20) tcp_parse_opts(p + 20, off - 20, &g.mss, &g.ws);
    if ((g.flags & (TH_SYN | TH_FIN)) == (TH_SYN | TH_FIN) || (g.flags & (TH_SYN | TH_RST)) == (TH_SYN | TH_RST)) { g_net_stat_tcp_bad++; return; }
    if (!(g.flags & (TH_SYN | TH_FIN | TH_RST | TH_ACK))) { g_net_stat_tcp_bad++; return; }          /* a null segment */
    if (g.sport == 0 || g.dport == 0) { g_net_stat_tcp_bad++; return; }

    Sock *l = NULL;
    Sock *s = tcp_lookup(dst, g.dport, src, g.sport, &l);
    if (s && s->state == TCPS_TIME_WAIT && (g.flags & TH_SYN) && !(g.flags & TH_ACK) && SEQ_GT(g.seq, s->rcv_nxt + (s->fin_rcvd ? 1u : 0u))) {
        tcp_release(s);                                           /* RFC 1122 4.2.2.13: a newer SYN reopens the tuple */
        s = NULL;
        tcp_lookup(dst, g.dport, src, g.sport, &l);
    }
    if (s) { tcp_segment(s, &g); return; }
    if (l) {
        if (g.flags & TH_RST) return;
        if (g.flags & TH_ACK) { tcp_reset_reply(src, dst, g.sport, g.dport, g.seq, g.ack, g.flags, g.len); return; }
        if (g.flags & TH_SYN) tcp_listen_syn(l, &g);
        return;
    }
    tcp_reset_reply(src, dst, g.sport, g.dport, g.seq, g.ack, g.flags, g.len);
}

void tcp_error(u32 laddr, u16 lport, u32 raddr, u16 rport, int err) {
    for (Sock *s = g_socks; s; s = s->next) {
        if (s->kind != SK_TCP || s->state != TCPS_SYN_SENT) continue;
        if (s->laddr == laddr && s->lport == lport && s->raddr == raddr && s->rport == rport) { tcp_abort(s, err, false); return; }
    }
}

/* ------------------------------------------------------------------ user-facing operations */

int tcp_connect(Sock *s, u32 raddr, u16 rport) {
    u32 laddr = s->laddr ? s->laddr : ip_source_for(raddr);
    if (!laddr) return -ENETUNREACH;
    if (!s->bound) {
        s->lport = sock_alloc_port(SK_TCP);
        s->bound = true;
    }
    for (Sock *o = g_socks; o; o = o->next)
        if (o != s && o->kind == SK_TCP && o->state != TCPS_LISTEN && o->lport == s->lport && o->laddr == laddr && o->raddr == raddr && o->rport == rport)
            return -EADDRNOTAVAIL;
    tcp_alloc_bufs(s);
    if (!s->sbuf || !s->rbuf) return -ENOMEM;
    s->laddr = laddr;
    s->raddr = raddr;
    s->rport = rport;
    s->err = 0;
    s->rd_shut = s->wr_shut = false;
    s->fin_pending = s->fin_acked = s->fin_rcvd = s->fin_seen = false;
    s->nooo = 0;
    tcp_init_conn(s, laddr, raddr);
    s->ws_on = true;                                              /* offered in our SYN; cleared if the peer does not answer in kind */
    s->cwnd = 10u * s->mss;
    s->rcv_nxt = s->rd_nxt = s->rcv_adv = 0;
    s->state = TCPS_SYN_SENT;
    s->nrexmit = 0;
    tcp_send_syn(s, false);
    s->rtt_on = true;
    s->rtt_time = net_now();
    s->rtt_seq = s->iss + 1;
    s->snd_nxt = s->iss + 1;
    s->snd_max = s->snd_nxt;
    tcp_arm_rexmit(s);
    return 0;
}

int tcp_listen(Sock *s, int backlog) {
    if (s->state != TCPS_CLOSED) return -EINVAL;
    s->backlog = backlog < 1 ? 1 : backlog > 128 ? 128 : backlog;
    s->state = TCPS_LISTEN;
    return 0;
}

Sock *tcp_accept_dequeue(Sock *l) {
    Sock *c = l->acceptq;
    if (!c) return NULL;
    l->acceptq = c->anext;
    if (!l->acceptq) l->acceptq_tail = NULL;
    l->naccept--;
    c->anext = NULL;
    c->listener = NULL;
    return c;
}

size_t tcp_send_space(Sock *s) {
    if (s->state != TCPS_ESTABLISHED && s->state != TCPS_CLOSE_WAIT) return 0;
    if (s->wr_shut || s->fin_pending) return 0;
    u32 queued = SEQ_GT(s->snd_end, s->snd_una) ? s->snd_end - s->snd_una : 0;
    return queued >= TCP_BUF ? 0 : TCP_BUF - queued;
}

size_t tcp_queue(Sock *s, const u8 *data, size_t n) {
    size_t room = tcp_send_space(s);
    size_t k = MIN(n, room);
    if (!k) return 0;
    size_t idx = s->snd_end & (TCP_BUF - 1);
    size_t first = MIN(k, (size_t)TCP_BUF - idx);
    memcpy(s->sbuf + idx, data, first);
    if (k > first) memcpy(s->sbuf, data + first, k - first);
    s->snd_end += (u32)k;
    tcp_output(s);
    return k;
}

size_t tcp_readable(Sock *s) {
    if (!s->rbuf) return 0;
    return s->rcv_nxt - s->rd_nxt;
}

size_t tcp_read(Sock *s, u8 *out, size_t n, bool peek) {
    size_t avail = tcp_readable(s);
    size_t k = MIN(n, avail);
    if (!k) return 0;
    size_t idx = s->rd_nxt & (TCP_BUF - 1);
    size_t first = MIN(k, (size_t)TCP_BUF - idx);
    memcpy(out, s->rbuf + idx, first);
    if (k > first) memcpy(out + first, s->rbuf, k - first);
    if (peek) return k;
    s->rd_nxt += (u32)k;
    if (s->state != TCPS_CLOSED && s->state != TCPS_TIME_WAIT) {
        u32 newedge = s->rd_nxt + TCP_BUF;                       /* the window edge we could advertise now */
        if (SEQ_GEQ(newedge, s->rcv_adv + MIN((u32)TCP_BUF / 2, (u32)s->mss))) tcp_ack_now(s);
    }
    return k;
}

static void tcp_start_fin(Sock *s) {
    if (s->fin_pending) return;
    s->fin_pending = true;
    s->fin_seq = s->snd_end;
    switch (s->state) {
    case TCPS_ESTABLISHED: case TCPS_SYN_RCVD: s->state = TCPS_FIN_WAIT1; break;
    case TCPS_CLOSE_WAIT: s->state = TCPS_LAST_ACK; break;
    default: break;
    }
    tcp_output(s);
}

void tcp_shutdown(Sock *s, int how) {
    if (how == SHUT_RD || how == SHUT_RDWR) { s->rd_shut_user = true; sock_wake(s); }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        s->wr_shut = true;
        if (s->state == TCPS_ESTABLISHED || s->state == TCPS_CLOSE_WAIT) tcp_start_fin(s);
        sock_wake(s);
    }
}

void tcp_close(Sock *s) {
    s->has_file = false;
    switch (s->state) {
    case TCPS_LISTEN: {
        Sock *next;
        for (Sock *c = g_socks; c; c = next) {                  /* nobody will ever accept these */
            next = c->next;
            if (c->listener == s) { c->listener = s; tcp_abort(c, 0, true); }
        }
        s->state = TCPS_CLOSED;
        tcp_destroy(s);
        return;
    }
    case TCPS_CLOSED: case TCPS_SYN_SENT:
        s->state = TCPS_CLOSED;
        tcp_destroy(s);
        return;
    case TCPS_TIME_WAIT:
        return;                                                 /* the timer frees it */
    default: break;
    }
    if (tcp_readable(s) > 0 && s->state != TCPS_LAST_ACK && s->state != TCPS_CLOSING) {      /* unread data: tell the peer the line is dead */
        tcp_abort(s, 0, true);
        return;
    }
    s->rd_shut_user = true;
    s->wr_shut = true;
    tcp_start_fin(s);
    if (s->state == TCPS_FIN_WAIT2) s->t_misc = net_now() + FIN_WAIT2_MS;
}

int tcp_poll(Sock *s) {
    int m = 0;
    switch (s->state) {
    case TCPS_LISTEN: return s->naccept ? POLLIN : 0;
    case TCPS_SYN_SENT: case TCPS_SYN_RCVD: return s->err ? (POLLERR | POLLHUP | POLLOUT) : 0;
    case TCPS_CLOSED:
        m = POLLOUT | POLLHUP;
        if (s->err) m |= POLLERR;
        if (tcp_readable(s)) m |= POLLIN;
        return m;
    default: break;
    }
    if (tcp_readable(s) || s->rd_shut || s->rd_shut_user) m |= POLLIN;
    if (s->err) m |= POLLERR;
    if (tcp_send_space(s) > 0) m |= POLLOUT;
    if (s->rd_shut && (s->wr_shut || s->state == TCPS_LAST_ACK || s->state == TCPS_CLOSING || s->state == TCPS_TIME_WAIT)) m |= POLLHUP;
    return m;
}
