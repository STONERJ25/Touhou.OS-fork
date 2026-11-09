/*
 * The constants and structures of the Linux socket, poll, epoll, eventfd and timerfd calls (docs/COMPAT.md). Values are the ones x86-64 Linux defines in its
 * public headers (<sys/socket.h>, <netinet/in.h>, <netinet/tcp.h>, <sys/epoll.h>, <sys/eventfd.h>, <sys/timerfd.h>, <poll.h>); the native system uses the same
 * numbers for the common ones (shared/netabi.h), but Linux has more of them and several structures differ in size or layout. tests/host/test_linux_net_abi.py
 * compares every name here with the header of the machine it runs on, and the size and field offsets of every structure with the C library's.
 */
#ifndef K_LINUX_NETABI_H
#define K_LINUX_NETABI_H

#include "types.h"

/* address families, socket types and the flags or-ed into the type */
#define LNX_AF_UNSPEC       0
#define LNX_AF_UNIX         1
#define LNX_AF_INET         2
#define LNX_AF_INET6        10
#define LNX_AF_NETLINK      16
#define LNX_AF_PACKET       17
#define LNX_SOCK_STREAM     1
#define LNX_SOCK_DGRAM      2
#define LNX_SOCK_RAW        3
#define LNX_SOCK_SEQPACKET  5
#define LNX_SOCK_NONBLOCK   0x800
#define LNX_SOCK_CLOEXEC    0x80000
#define LNX_SOCK_TYPE_MASK  0xf

/* protocol levels, and options at the SOL_SOCKET level */
#define LNX_SOL_SOCKET      1
#define LNX_SOL_IP          0
#define LNX_SOL_TCP         6
#define LNX_SO_DEBUG        1
#define LNX_SO_REUSEADDR    2
#define LNX_SO_TYPE         3
#define LNX_SO_ERROR        4
#define LNX_SO_DONTROUTE    5
#define LNX_SO_BROADCAST    6
#define LNX_SO_SNDBUF       7
#define LNX_SO_RCVBUF       8
#define LNX_SO_KEEPALIVE    9
#define LNX_SO_OOBINLINE    10
#define LNX_SO_NO_CHECK     11
#define LNX_SO_PRIORITY     12
#define LNX_SO_LINGER       13
#define LNX_SO_REUSEPORT    15
#define LNX_SO_PASSCRED     16
#define LNX_SO_PEERCRED     17
#define LNX_SO_RCVLOWAT     18
#define LNX_SO_SNDLOWAT     19
#define LNX_SO_RCVTIMEO     20
#define LNX_SO_SNDTIMEO     21
#define LNX_SO_TIMESTAMP    29
#define LNX_SO_ACCEPTCONN   30
#define LNX_SO_SNDBUFFORCE  32
#define LNX_SO_RCVBUFFORCE  33
#define LNX_SO_PROTOCOL     38
#define LNX_SO_DOMAIN       39

/* options at the IP and TCP levels */
#define LNX_IP_TOS          1
#define LNX_IP_TTL          2
#define LNX_IP_MTU_DISCOVER 10
#define LNX_IP_RECVERR      11
#define LNX_TCP_NODELAY     1
#define LNX_TCP_MAXSEG      2
#define LNX_TCP_CORK        3
#define LNX_TCP_KEEPIDLE    4
#define LNX_TCP_KEEPINTVL   5
#define LNX_TCP_KEEPCNT     6
#define LNX_TCP_SYNCNT      7
#define LNX_TCP_LINGER2     8
#define LNX_TCP_DEFER_ACCEPT 9
#define LNX_TCP_QUICKACK    12
#define LNX_TCP_USER_TIMEOUT 18

/* flags of send/recv and of the msghdr, and the control messages */
#define LNX_MSG_OOB         0x1
#define LNX_MSG_PEEK        0x2
#define LNX_MSG_DONTROUTE   0x4
#define LNX_MSG_CTRUNC      0x8
#define LNX_MSG_TRUNC       0x20
#define LNX_MSG_DONTWAIT    0x40
#define LNX_MSG_EOR         0x80
#define LNX_MSG_WAITALL     0x100
#define LNX_MSG_NOSIGNAL    0x4000
#define LNX_MSG_MORE        0x8000
#define LNX_MSG_WAITFORONE  0x10000
#define LNX_MSG_CMSG_CLOEXEC 0x40000000
#define LNX_SCM_RIGHTS      1
#define LNX_SCM_CREDENTIALS 2
#define LNX_SHUT_RD         0
#define LNX_SHUT_WR         1
#define LNX_SHUT_RDWR       2
#define LNX_INADDR_ANY      0u

/* fcntl and ioctl requests beyond the native ones */
#define LNX_F_DUPFD_CLOEXEC 1030
#define LNX_F_SETPIPE_SZ    1031
#define LNX_F_GETPIPE_SZ    1032
#define LNX_FIONREAD        0x541B
#define LNX_FIONBIO         0x5421
#define LNX_FIONCLEX        0x5450
#define LNX_FIOCLEX         0x5451

/* poll(2) and the bits epoll shares with it */
#define LNX_POLLIN          0x001
#define LNX_POLLPRI         0x002
#define LNX_POLLOUT         0x004
#define LNX_POLLERR         0x008
#define LNX_POLLHUP         0x010
#define LNX_POLLNVAL        0x020
#define LNX_POLLRDNORM      0x040
#define LNX_POLLRDBAND      0x080
#define LNX_POLLWRNORM      0x100
#define LNX_POLLWRBAND      0x200
#define LNX_POLLRDHUP       0x2000

/* epoll(7) */
#define LNX_EPOLL_CLOEXEC   0x80000
#define LNX_EPOLL_CTL_ADD   1
#define LNX_EPOLL_CTL_DEL   2
#define LNX_EPOLL_CTL_MOD   3
#define LNX_EPOLLIN         0x001u
#define LNX_EPOLLPRI        0x002u
#define LNX_EPOLLOUT        0x004u
#define LNX_EPOLLERR        0x008u
#define LNX_EPOLLHUP        0x010u
#define LNX_EPOLLRDNORM     0x040u
#define LNX_EPOLLRDBAND     0x080u
#define LNX_EPOLLWRNORM     0x100u
#define LNX_EPOLLWRBAND     0x200u
#define LNX_EPOLLMSG        0x400u
#define LNX_EPOLLRDHUP      0x2000u
#define LNX_EPOLLEXCLUSIVE  (1u << 28)
#define LNX_EPOLLWAKEUP     (1u << 29)
#define LNX_EPOLLONESHOT    (1u << 30)
#define LNX_EPOLLET         (1u << 31)

/* eventfd(2) and timerfd_create(2) */
#define LNX_EFD_SEMAPHORE   1
#define LNX_EFD_CLOEXEC     0x80000
#define LNX_EFD_NONBLOCK    0x800
#define LNX_TFD_CLOEXEC     0x80000
#define LNX_TFD_NONBLOCK    0x800
#define LNX_TFD_TIMER_ABSTIME 1
#define LNX_TFD_TIMER_CANCEL_ON_SET 2
#define LNX_CLOCK_REALTIME  0
#define LNX_CLOCK_MONOTONIC 1
#define LNX_CLOCK_BOOTTIME  7

/* select(2) */
#define LNX_FD_SETSIZE      1024

struct lnx_iov { u64 iov_base, iov_len; };                          /* struct iovec */

struct lnx_msghdr {                                                  /* struct msghdr (56 bytes) */
    u64 msg_name;
    u32 msg_namelen, pad0;
    u64 msg_iov, msg_iovlen;
    u64 msg_control, msg_controllen;
    u32 msg_flags, pad1;
};

struct lnx_mmsghdr { struct lnx_msghdr msg_hdr; u32 msg_len, pad; };          /* struct mmsghdr (64 bytes) */
struct lnx_cmsghdr { u64 cmsg_len; s32 cmsg_level, cmsg_type; };              /* struct cmsghdr (16 bytes); the data follows, padded to 8 */
struct lnx_ucred { s32 pid; u32 uid, gid; };                                  /* SO_PEERCRED, SCM_CREDENTIALS */
struct lnx_linger { s32 l_onoff, l_linger; };                                 /* SO_LINGER */
struct lnx_sockaddr_un { u16 sun_family; char sun_path[108]; };               /* 110 bytes */
struct lnx_timespec { s64 tv_sec, tv_nsec; };
struct lnx_timeval { s64 tv_sec, tv_usec; };
struct lnx_itimerspec { struct lnx_timespec it_interval, it_value; };         /* 32 bytes */
struct lnx_pollfd { s32 fd; s16 events, revents; };                           /* 8 bytes */
struct lnx_epoll_event { u32 events; u64 data; } __attribute__((packed));     /* packed on x86-64: 12 bytes */

#define LNX_CMSG_ALIGN(n)   (((n) + 7) & ~7UL)
#define LNX_CMSG_LEN(n)     (sizeof(struct lnx_cmsghdr) + (n))
#define LNX_CMSG_SPACE(n)   (sizeof(struct lnx_cmsghdr) + LNX_CMSG_ALIGN(n))

#endif
