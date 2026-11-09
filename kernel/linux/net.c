/*
 * Sockets for the Linux personality (docs/COMPAT.md): socket, socketpair, accept4, sendmsg, recvmsg, sendmmsg, recvmmsg, sendfile, and the adaptations of
 * fcntl and ioctl that sockets need (F_DUPFD_CLOEXEC, FIONBIO). The calls whose arguments and results are the same as the native ones (bind, listen, connect,
 * accept, shutdown, getsockname, getpeername, sendto, recvfrom, setsockopt, getsockopt) are routed to the native handlers by the table in syscall.c; the
 * native sockets already use Linux's errno values, option numbers, flag values and sockaddr_in, which is why that is enough.
 *
 * AF_INET6, AF_NETLINK, AF_PACKET and every other family answer EAFNOSUPPORT: the stack has IPv4 only, and a C library falls back to it. Written from the
 * manual pages socket(2), socketpair(2), accept(2), sendmsg(2), recvmsg(2), sendmmsg(2), recvmmsg(2), sendfile(2), fcntl(2), ioctl_tty(2) and unix(7) and the
 * System V x86-64 ABI for the structure layouts (netabi.h); the behaviour is checked against a real Linux by tests/linux/src/net_*.c.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "numbers.h"
#include "netabi.h"
#include "calls.h"
#include "netsock.h"
#include "unixsock.h"
#include "pollq.h"

static inline Process *me(void) { return current->proc; }

#define SEND_CHUNK 65536                    /* bytes moved through a kernel buffer at a time */
#define UIO_MAXIOV 1024

static long native(int nr, TrapFrame *tf) {
    SyscallFn f = syscall_native(nr);
    return f ? f(tf) : -ENOSYS;
}

/* The file behind a descriptor that must be a socket (a reference to put back): EBADF, or ENOTSOCK for any other kind of file. */
static long socket_file(int fd, File **out) {
    File *f = fd_get_ref(me(), fd);
    if (!f) return -EBADF;
    if (f->fops != &g_inet_fops && !unix_is_sock(f)) { file_put(f); return -ENOTSOCK; }
    *out = f;
    return 0;
}

/* ------------------------------------------------------------------ creating and accepting */

/* socket(domain, type, protocol): the type may carry SOCK_NONBLOCK and SOCK_CLOEXEC, which the native call understands too. */
long lnx_socket(TrapFrame *tf) {
    int domain = (int)A0(tf), type = (int)A1(tf), protocol = (int)A2(tf);
    if (type & ~(LNX_SOCK_TYPE_MASK | LNX_SOCK_NONBLOCK | LNX_SOCK_CLOEXEC)) return -EINVAL;
    if (domain == LNX_AF_UNIX) {
        if (protocol != 0 && protocol != LNX_AF_UNIX) return -EPROTONOSUPPORT;                  /* unix(7): the protocol is 0 (or PF_UNIX, which is 1) */
    } else if (domain != LNX_AF_INET) {
        return -EAFNOSUPPORT;                                  /* INET6, NETLINK, PACKET, ...: not here */
    }
    return native(SYS_socket, tf);
}

/* socketpair(domain, type, protocol, sv): the native call takes (type, sv). Only UNIX-domain pairs exist. */
long lnx_socketpair(TrapFrame *tf) {
    int domain = (int)A0(tf), type = (int)A1(tf), protocol = (int)A2(tf);
    if (type & ~(LNX_SOCK_TYPE_MASK | LNX_SOCK_NONBLOCK | LNX_SOCK_CLOEXEC)) return -EINVAL;
    if (domain != LNX_AF_UNIX) return domain == LNX_AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
    if (protocol != 0 && protocol != LNX_AF_UNIX) return -EPROTONOSUPPORT;
    TrapFrame t = *tf;
    t.rdi = (u64)(long)type;
    t.rsi = A3(tf);
    return native(SYS_socketpair, &t);
}

/* accept4(fd, addr, addrlen, flags): accept, and the new descriptor may be made non-blocking and close-on-exec at once. */
long lnx_accept4(TrapFrame *tf) {
    int flags = (int)A3(tf);
    if (flags & ~(LNX_SOCK_NONBLOCK | LNX_SOCK_CLOEXEC)) return -EINVAL;
    long fd = native(SYS_accept, tf);
    if (fd >= 0 && flags) {
        File *nf = fd_get(me(), (int)fd);
        if (nf && (flags & LNX_SOCK_NONBLOCK)) nf->flags |= O_NONBLOCK;
        if (flags & LNX_SOCK_CLOEXEC) me()->fd_flags[fd] |= FD_CLOEXEC;
    }
    return fd;
}

/* ------------------------------------------------------------------ vectored messages */

/* The program's iovec array, copied in (kmalloc; the caller frees it). At most UIO_MAXIOV elements; the lengths add up to *total (EINVAL if that overflows). */
static long iov_load(u64 uiov, u64 cnt, struct lnx_iov **out, size_t *total) {
    *out = NULL;
    *total = 0;
    if (cnt > UIO_MAXIOV) return -EMSGSIZE;
    struct lnx_iov *v = kmalloc(cnt ? cnt * sizeof *v : 1);
    if (!v) return -ENOMEM;
    if (cnt && copy_from_user(v, (const void *)uiov, cnt * sizeof *v)) { kfree(v); return -EFAULT; }
    u64 sum = 0;
    for (u64 i = 0; i < cnt; i++) {
        if (v[i].iov_len > 0x7ffff000UL || sum + v[i].iov_len > 0x7ffff000UL) { kfree(v); return -EINVAL; }
        sum += v[i].iov_len;
    }
    *out = v;
    *total = (size_t)sum;
    return 0;
}

typedef struct IovCur { const struct lnx_iov *v; u64 cnt, i, off; } IovCur;

/* Copies up to n bytes from the user buffers at the cursor into dst; *got says how many arrived (fewer only at the end of the vector). */
static long iov_gather(IovCur *c, u8 *dst, size_t n, size_t *got) {
    size_t done = 0;
    while (done < n && c->i < c->cnt) {
        u64 room = c->v[c->i].iov_len - c->off;
        size_t take = (size_t)MIN((u64)(n - done), room);
        if (take && copy_from_user(dst + done, (const u8 *)c->v[c->i].iov_base + c->off, take)) return -EFAULT;
        done += take;
        c->off += take;
        if (c->off >= c->v[c->i].iov_len) { c->i++; c->off = 0; }
    }
    *got = done;
    return 0;
}

/* The other direction: spreads src over the user buffers at the cursor. */
static long iov_scatter(IovCur *c, const u8 *src, size_t n) {
    size_t done = 0;
    while (done < n && c->i < c->cnt) {
        u64 room = c->v[c->i].iov_len - c->off;
        size_t take = (size_t)MIN((u64)(n - done), room);
        if (take && copy_to_user((u8 *)c->v[c->i].iov_base + c->off, src + done, take)) return -EFAULT;
        done += take;
        c->off += take;
        if (c->off >= c->v[c->i].iov_len) { c->i++; c->off = 0; }
    }
    return 0;
}

/* ---- control messages: SCM_RIGHTS (open files) and SCM_CREDENTIALS travel with a message on a UNIX socket */

#define OPTMEM_MAX 20480                    /* the most control data one sendmsg may carry (Linux's optmem_max) */

static void files_put_all(File **fds, int n) {
    for (int i = 0; i < n; i++) file_put(fds[i]);
    kfree(fds);
}

/* Reads the control messages of a sendmsg: each SCM_RIGHTS gives the open files its descriptors name (references; *fds is a kmalloc'd array of *nfds files), an
 * SCM_CREDENTIALS only has to be well formed (the credentials sent are always the caller's own). Anything else on a UNIX socket is EINVAL; on an Internet socket
 * the control data (IP_PKTINFO and the like) is ignored, but passing files makes no sense there. */
static long cmsg_collect(const struct lnx_msghdr *mh, bool inet, File ***fds, int *nfds) {
    *fds = NULL;
    *nfds = 0;
    size_t total = (size_t)mh->msg_controllen;
    if (!mh->msg_control || !total) return 0;
    if (total > OPTMEM_MAX) return -ENOBUFS;
    u8 *ctl = kmalloc(total);
    if (!ctl) return -ENOMEM;
    if (copy_from_user(ctl, (const void *)mh->msg_control, total)) { kfree(ctl); return -EFAULT; }
    long rc = 0;
    File **files = NULL;
    int n = 0;
    size_t off = 0;
    while (!rc && total - off >= sizeof(struct lnx_cmsghdr)) {
        struct lnx_cmsghdr h;
        memcpy(&h, ctl + off, sizeof h);
        if (h.cmsg_len < sizeof h || h.cmsg_len > total - off) { rc = -EINVAL; break; }
        size_t dlen = (size_t)h.cmsg_len - sizeof h;
        const u8 *data = ctl + off + sizeof h;
        if (h.cmsg_level == LNX_SOL_SOCKET && h.cmsg_type == LNX_SCM_RIGHTS) {
            int count = (int)(dlen / 4);
            if (inet || dlen % 4 || n + count > UNIX_MAX_FDS) { rc = -EINVAL; break; }
            if (!files && count) {
                files = kmalloc(UNIX_MAX_FDS * sizeof *files);
                if (!files) { rc = -ENOMEM; break; }
            }
            for (int i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, data + 4 * i, sizeof fd);
                File *f = fd_get_ref(me(), fd);
                if (!f) { rc = -EBADF; break; }
                files[n++] = f;
            }
        } else if (h.cmsg_level == LNX_SOL_SOCKET && h.cmsg_type == LNX_SCM_CREDENTIALS) {
            if (!inet && dlen != sizeof(struct lnx_ucred)) rc = -EINVAL;
        } else if (!inet) {
            rc = -EINVAL;
        }
        off += LNX_CMSG_ALIGN((size_t)h.cmsg_len);
    }
    kfree(ctl);
    if (rc) {
        if (files) files_put_all(files, n);
        return rc;
    }
    *fds = files;
    *nfds = n;
    return 0;
}

/* sendmsg for one message: the iovec elements are gathered into kernel buffers and sent; a stream goes out in pieces of SEND_CHUNK (the files of a control
 * message go with the first), a datagram as one. */
static long msg_send(File *f, const struct lnx_msghdr *mh, int flags) {
    struct lnx_iov *iov;
    size_t total;
    long rc = iov_load(mh->msg_iov, mh->msg_iovlen, &iov, &total);
    if (rc) return rc;
    bool inet = f->fops == &g_inet_fops;
    bool dgram = inet ? ((Sock *)f->priv)->kind != SK_TCP : unix_is_dgram(f);       /* the kind never changes after socket() */
    File **fds;
    int nfds;
    rc = cmsg_collect(mh, inet, &fds, &nfds);
    if (rc) { kfree(iov); return rc; }
    u32 daddr = 0;
    u16 dport = 0;
    bool have = false;
    if (mh->msg_name && mh->msg_namelen && inet) {
        rc = sk_get_sockaddr(mh->msg_name, mh->msg_namelen, &daddr, &dport);
        if (rc) { kfree(iov); if (fds) files_put_all(fds, nfds); return rc; }
        have = true;
    }
    size_t chunk = dgram ? total : MIN(total, (size_t)SEND_CHUNK);
    u8 *k = (dgram && total > (inet ? 65507u : 130000u)) ? NULL : kmalloc(chunk ? chunk : 1);
    if (!k) {
        kfree(iov);
        if (fds) files_put_all(fds, nfds);
        return dgram && total > (inet ? 65507u : 130000u) ? -EMSGSIZE : -ENOMEM;
    }
    IovCur cur = { iov, mh->msg_iovlen, 0, 0 };
    size_t sent = 0;
    rc = 0;
    do {
        size_t want = MIN(total - sent, chunk), got = 0;
        long e = iov_gather(&cur, k, want, &got);
        if (e) { rc = sent ? (long)sent : e; break; }
        ssize_t w;
        if (inet) {
            w = sk_send(f, k, want, flags, have, daddr, dport);
        } else {
            File **mine = fds;                                              /* handed over: the callee puts them if the send fails */
            int nmine = nfds;
            fds = NULL;
            nfds = 0;
            w = unix_send_msg(f, k, want, flags, mh->msg_name && mh->msg_namelen ? mh->msg_name : 0, mh->msg_namelen, mine, nmine);
        }
        if (w < 0) { rc = sent ? (long)sent : w; break; }
        sent += (size_t)w;
        rc = (long)sent;
        if ((size_t)w < want) break;
    } while (sent < total);
    if (fds) files_put_all(fds, nfds);                                      /* never handed over (an Internet socket, or the loop did not get there) */
    kfree(k);
    kfree(iov);
    return rc;
}

/* Builds the control data of a received message at msg_control: the sender's credentials for a socket that asked (SO_PASSCRED), then the descriptors, which
 * become descriptors of this process. What does not fit is dropped and MSG_CTRUNC says so. Sets msg_controllen to the bytes used. */
static long cmsg_build(struct lnx_msghdr *mh, UnixCtl *ctl, int rflags, u32 *out_flags) {
    size_t room = mh->msg_control ? (size_t)mh->msg_controllen : 0, used = 0;
    long rc = 0;
    if (ctl->have_cred) {
        if (room - used >= LNX_CMSG_LEN(sizeof(struct lnx_ucred))) {
            struct { struct lnx_cmsghdr h; struct lnx_ucred c; } m = { { LNX_CMSG_LEN(sizeof(struct lnx_ucred)), LNX_SOL_SOCKET, LNX_SCM_CREDENTIALS },
                                                                       { ctl->pid, ctl->uid, ctl->gid } };
            if (copy_to_user((u8 *)mh->msg_control + used, &m, sizeof m)) rc = -EFAULT;
            used += MIN(LNX_CMSG_SPACE(sizeof(struct lnx_ucred)), room - used);
        } else {
            *out_flags |= LNX_MSG_CTRUNC;
        }
    }
    if (ctl->nfds) {
        int fit = 0;
        if (ctl->fds && room - used >= LNX_CMSG_LEN(sizeof(int)))
            fit = (int)MIN((u64)ctl->nfds, (room - used - sizeof(struct lnx_cmsghdr)) / sizeof(int));
        if (fit < ctl->nfds) *out_flags |= LNX_MSG_CTRUNC;
        int fdnums[UNIX_MAX_FDS], made = 0;
        for (int i = 0; ctl->fds && i < ctl->nfds; i++) {
            if (i < fit) {
                long fd = fd_install(me(), ctl->fds[i], 0, (rflags & LNX_MSG_CMSG_CLOEXEC) ? FD_CLOEXEC : 0);
                if (fd >= 0) { fdnums[made++] = (int)fd; continue; }
                *out_flags |= LNX_MSG_CTRUNC;
            }
            file_put(ctl->fds[i]);                                           /* no room for it, or no free descriptor */
        }
        kfree(ctl->fds);
        ctl->fds = NULL;
        if (made) {
            struct lnx_cmsghdr h = { LNX_CMSG_LEN((size_t)made * sizeof(int)), LNX_SOL_SOCKET, LNX_SCM_RIGHTS };
            if (copy_to_user((u8 *)mh->msg_control + used, &h, sizeof h) || copy_to_user((u8 *)mh->msg_control + used + sizeof h, fdnums, (size_t)made * sizeof(int)))
                rc = -EFAULT;
            used += MIN(LNX_CMSG_SPACE((size_t)made * sizeof(int)), room - used);
        }
    }
    mh->msg_controllen = used;
    return rc;
}

/* recvmsg for one message. A stream gets up to the room in the vector (all of it with MSG_WAITALL); a datagram is one message, cut to the room with MSG_TRUNC set
 * in msg_flags if it was longer (and with MSG_TRUNC given, the result is the real length). The sender's address is stored for a datagram socket, and the control
 * messages (files, credentials) of a UNIX socket in msg_control. */
static long msg_recv(File *f, struct lnx_msghdr *mh, int flags) {
    struct lnx_iov *iov;
    size_t cap;
    long rc = iov_load(mh->msg_iov, mh->msg_iovlen, &iov, &cap);
    if (rc) return rc;
    bool inet = f->fops == &g_inet_fops;
    bool dgram = inet ? ((Sock *)f->priv)->kind != SK_TCP : unix_is_dgram(f);
    size_t chunk = dgram ? MIN(cap, inet ? (size_t)65536 : (size_t)130000) : MIN(cap, (size_t)SEND_CHUNK);
    u8 *k = kmalloc(chunk ? chunk : 1);
    if (!k) { kfree(iov); return -ENOMEM; }
    IovCur cur = { iov, mh->msg_iovlen, 0, 0 };
    size_t got_total = 0, full = 0;
    u32 from = 0;
    u16 fport = 0;
    u32 out_flags = 0;
    UnixCtl uctl;
    memset(&uctl, 0, sizeof uctl);
    uctl.want_fds = mh->msg_control && mh->msg_controllen;
    rc = 0;
    for (;;) {
        size_t want = MIN(cap - got_total, chunk);
        ssize_t r = inet ? sk_recv_ex(f, k, want, flags & ~LNX_MSG_TRUNC, &from, &fport, &full)
                         : unix_recv_msg(f, k, want, flags & ~LNX_MSG_TRUNC, &uctl);
        if (r < 0) { rc = got_total ? (long)got_total : r; break; }
        long e = iov_scatter(&cur, k, (size_t)r);
        if (e) { rc = got_total ? (long)got_total : e; break; }
        got_total += (size_t)r;
        rc = (long)got_total;
        if (!inet && dgram) full = uctl.full;
        if (dgram) {
            if (full > (size_t)r) out_flags |= LNX_MSG_TRUNC;
            if (flags & LNX_MSG_TRUNC) rc = (long)full;
            break;
        }
        if (r == 0 || !(flags & LNX_MSG_WAITALL) || got_total >= cap || (size_t)r < want) break;
        if (!inet && uctl.nfds) break;                                      /* the files of the next message start another call */
    }
    kfree(k);
    kfree(iov);
    if (rc < 0) {
        if (uctl.fds) files_put_all(uctl.fds, uctl.nfds);
        return rc;
    }
    long ce = 0;
    if (!inet) ce = cmsg_build(mh, &uctl, flags, &out_flags);
    else mh->msg_controllen = 0;                                            /* no control messages are delivered on an Internet socket */
    mh->msg_flags = out_flags;
    if (ce) return ce;
    if (mh->msg_name && mh->msg_namelen) {
        if (inet && dgram) {
            struct gsockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.family = AF_INET;
            sa.port = hton16(fport);
            sa.addr = hton32(from);
            size_t n = MIN((size_t)mh->msg_namelen, sizeof sa);
            if (copy_to_user((void *)mh->msg_name, &sa, n)) return -EFAULT;
            mh->msg_namelen = sizeof sa;
        } else if (!inet && dgram && uctl.fromlen) {
            struct lnx_sockaddr_un sun;
            memset(&sun, 0, sizeof sun);
            sun.sun_family = LNX_AF_UNIX;
            memcpy(sun.sun_path, uctl.from, uctl.fromlen);
            size_t full_len = sizeof(u16) + uctl.fromlen;
            if (copy_to_user((void *)mh->msg_name, &sun, MIN((size_t)mh->msg_namelen, full_len))) return -EFAULT;
            mh->msg_namelen = (u32)full_len;
        } else {
            mh->msg_namelen = 0;                                            /* a connected stream, or a sender without a name, has no address to report */
        }
    } else {
        mh->msg_namelen = 0;
    }
    return rc;
}

long lnx_sendmsg(TrapFrame *tf) {
    File *f;
    long rc = socket_file((int)A0(tf), &f);
    if (rc) return rc;
    struct lnx_msghdr mh;
    if (copy_from_user(&mh, (const void *)A1(tf), sizeof mh)) rc = -EFAULT;
    else rc = msg_send(f, &mh, (int)A2(tf));
    file_put(f);
    return rc;
}

long lnx_recvmsg(TrapFrame *tf) {
    File *f;
    long rc = socket_file((int)A0(tf), &f);
    if (rc) return rc;
    struct lnx_msghdr mh;
    if (copy_from_user(&mh, (const void *)A1(tf), sizeof mh)) { file_put(f); return -EFAULT; }
    rc = msg_recv(f, &mh, (int)A2(tf));
    if (rc >= 0 && copy_to_user((void *)A1(tf), &mh, sizeof mh)) rc = -EFAULT;       /* msg_namelen, msg_controllen and msg_flags have changed */
    file_put(f);
    return rc;
}

/* sendmmsg(fd, msgvec, vlen, flags): the messages one after the other; the result is how many went out, an error only if the first one failed. */
long lnx_sendmmsg(TrapFrame *tf) {
    File *f;
    long rc = socket_file((int)A0(tf), &f);
    if (rc) return rc;
    u64 vlen = MIN(A2(tf), (u64)UIO_MAXIOV);
    long sent = 0;
    for (u64 i = 0; i < vlen; i++) {
        struct lnx_mmsghdr mm;
        u64 at = A1(tf) + i * sizeof mm;
        if (copy_from_user(&mm, (const void *)at, sizeof mm)) { rc = -EFAULT; break; }
        rc = msg_send(f, &mm.msg_hdr, (int)A3(tf));
        if (rc < 0) break;
        mm.msg_len = (u32)rc;
        if (copy_to_user((u8 *)at + offsetof(struct lnx_mmsghdr, msg_len), &mm.msg_len, sizeof mm.msg_len)) { rc = -EFAULT; break; }
        sent++;
    }
    file_put(f);
    return sent ? sent : rc;
}

/* recvmmsg(fd, msgvec, vlen, flags, timeout): up to vlen messages; MSG_WAITFORONE makes everything after the first one non-blocking; the timeout is looked at
 * between messages, as the manual page admits. */
long lnx_recvmmsg(TrapFrame *tf) {
    File *f;
    long rc = socket_file((int)A0(tf), &f);
    if (rc) return rc;
    int flags = (int)A3(tf);
    u64 deadline = ~0ULL;
    if (A4(tf)) {
        rc = deadline_from_timespec((const void *)A4(tf), &deadline);
        if (rc) { file_put(f); return rc; }
    }
    u64 vlen = MIN(A2(tf), (u64)UIO_MAXIOV);
    long got = 0;
    rc = 0;
    for (u64 i = 0; i < vlen; i++) {
        struct lnx_mmsghdr mm;
        u64 at = A1(tf) + i * sizeof mm;
        if (copy_from_user(&mm, (const void *)at, sizeof mm)) { rc = -EFAULT; break; }
        rc = msg_recv(f, &mm.msg_hdr, flags & ~LNX_MSG_WAITFORONE);
        if (rc < 0) break;
        mm.msg_len = (u32)rc;
        if (copy_to_user((void *)at, &mm, sizeof mm)) { rc = -EFAULT; break; }
        got++;
        if (flags & LNX_MSG_WAITFORONE) flags |= LNX_MSG_DONTWAIT;
        if (deadline != ~0ULL && uptime_ms() >= deadline) break;
    }
    file_put(f);
    if (got) return got;
    return rc;
}

/* ------------------------------------------------------------------ sendfile */

/* sendfile(out_fd, in_fd, offset, count): copies from a regular file to any descriptor through a kernel buffer. With an offset the file position is left alone and
 * the offset advances; without one the file's position moves. A short result (the output is full, or the file ended) is not an error. */
long lnx_sendfile(TrapFrame *tf) {
    File *out = fd_get_ref(me(), (int)A0(tf)), *in = fd_get_ref(me(), (int)A1(tf));
    long rc = 0;
    if (!out || !in) { rc = -EBADF; goto done; }
    if ((in->flags & O_ACCMODE) == O_WRONLY || (out->flags & O_ACCMODE) == O_RDONLY) { rc = -EBADF; goto done; }
    if (!in->vn || !S_ISREG(in->vn->mode) || in->fops) { rc = -EINVAL; goto done; }             /* the input must be something that can be read at an offset */
    s64 off = (s64)in->off;
    if (A2(tf) && copy_from_user(&off, (const void *)A2(tf), sizeof off)) { rc = -EFAULT; goto done; }
    if (off < 0) { rc = -EINVAL; goto done; }
    u64 count = MIN(A3(tf), (u64)0x7ffff000);
    u8 *k = kmalloc(SEND_CHUNK);
    if (!k) { rc = -ENOMEM; goto done; }
    long total = 0;
    u64 saved = in->off;
    while ((u64)total < count) {
        size_t want = (size_t)MIN(count - (u64)total, (u64)SEND_CHUNK);
        in->off = (u64)off;
        ssize_t r = vfs_read(in, k, want);
        if (r < 0) { rc = total ? total : r; break; }
        if (r == 0) { rc = total; break; }
        ssize_t w = vfs_write(out, k, (size_t)r);
        if (w < 0) { rc = total ? total : w; break; }
        total += w;
        off += w;
        rc = total;
        if (w < r || (size_t)r < want) break;
    }
    in->off = saved;
    kfree(k);
    if (A2(tf)) { if (copy_to_user((void *)A2(tf), &off, sizeof off)) rc = -EFAULT; }
    else in->off = (u64)off;
done:
    if (out) file_put(out);
    if (in) file_put(in);
    return rc;
}

/* ------------------------------------------------------------------ fcntl and ioctl */
