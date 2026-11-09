/*
 * UDP through a statically linked C library: two sockets on the loopback address exchange datagrams with sendto and recvfrom, a connected socket uses send and
 * recv, a datagram that does not fit the buffer is cut (MSG_TRUNC reports the real length), MSG_PEEK, sendmsg and recvmsg with several buffers and a sender
 * address, sendmmsg and recvmmsg, EMSGSIZE and EDESTADDRREQ, and readable and writable polling. Prints only what the manual pages promise, so a real Linux prints
 * the same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int udp_socket(struct sockaddr_in *sa) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)sa, sizeof *sa);
    socklen_t l = sizeof *sa;
    getsockname(s, (struct sockaddr *)sa, &l);
    return s;
}

static void wait_readable(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    poll(&p, 1, 10000);
}

int main(void) {
    struct sockaddr_in a_addr, b_addr, from;
    int a = udp_socket(&a_addr), b = udp_socket(&b_addr);
    int type = 0;
    socklen_t tl = sizeof type;
    getsockopt(a, SOL_SOCKET, SO_TYPE, &type, &tl);
    printf("SO_TYPE: %d; two sockets bound, different ports: %d\n", type, a_addr.sin_port != b_addr.sin_port);

    ssize_t w = sendto(a, "hello udp", 9, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    printf("sendto: %zd\n", w);
    wait_readable(b);
    char buf[64];
    memset(buf, 0, sizeof buf);
    socklen_t fl = sizeof from;
    ssize_t r = recvfrom(b, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    printf("recvfrom: %zd '%s' from 127.0.0.1: %d, from a's port: %d, address length %u\n", r, buf, from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
           from.sin_port == a_addr.sin_port, fl);

    /* message boundaries: two datagrams are two reads, even when the buffer is big enough for both */
    sendto(a, "one", 3, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    sendto(a, "second", 6, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    wait_readable(b);
    usleep(50000);
    memset(buf, 0, sizeof buf);
    r = recv(b, buf, sizeof buf, 0);
    printf("first read: %zd '%s'", r, buf);
    memset(buf, 0, sizeof buf);
    r = recv(b, buf, sizeof buf, 0);
    printf(", second read: %zd '%s'\n", r, buf);

    /* a datagram that does not fit: the rest is lost, MSG_TRUNC tells the real length */
    sendto(a, "0123456789", 10, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    wait_readable(b);
    memset(buf, 0, sizeof buf);
    r = recv(b, buf, 4, MSG_PEEK | MSG_TRUNC);
    printf("peek with MSG_TRUNC and a 4-byte buffer: %zd", r);
    memset(buf, 0, sizeof buf);
    r = recv(b, buf, 4, 0);
    printf(", then a plain 4-byte read: %zd '%s'", r, buf);
    errno = 0;
    r = recv(b, buf, sizeof buf, MSG_DONTWAIT);
    printf(", and the rest is gone: %zd errno %d (EAGAIN %d)\n", r, errno, EAGAIN);

    /* recvmsg: several buffers, the sender's address, and MSG_TRUNC in msg_flags */
    sendto(a, "abcdefghij", 10, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    wait_readable(b);
    char m1[4] = { 0 }, m2[3] = { 0 };
    struct iovec iov[2] = { { m1, 3 }, { m2, 2 } };
    struct sockaddr_in src;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &src;
    mh.msg_namelen = sizeof src;
    mh.msg_iov = iov;
    mh.msg_iovlen = 2;
    r = recvmsg(b, &mh, 0);
    printf("recvmsg into 3+2 bytes: %zd '%s' '%s', truncated flag %d, sender port is a's: %d, name length %u\n", r, m1, m2, (mh.msg_flags & MSG_TRUNC) != 0,
           src.sin_port == a_addr.sin_port, mh.msg_namelen);

    /* sendmsg with three pieces and an explicit destination */
    char p1[] = "aa", p2[] = "bbb", p3[] = "c";
    struct iovec out[3] = { { p1, 2 }, { p2, 3 }, { p3, 1 } };
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &b_addr;
    mh.msg_namelen = sizeof b_addr;
    mh.msg_iov = out;
    mh.msg_iovlen = 3;
    printf("sendmsg of 2+3+1 bytes to b: %zd\n", sendmsg(a, &mh, 0));
    wait_readable(b);
    memset(buf, 0, sizeof buf);
    r = recv(b, buf, sizeof buf, 0);
    printf("one datagram of %zd bytes: '%s'\n", r, buf);

    /* a connected socket: send and recv without addresses, and only the peer's datagrams get through */
    printf("connect: %d\n", connect(a, (struct sockaddr *)&b_addr, sizeof b_addr));
    printf("send: %zd\n", send(a, "connected", 9, 0));
    wait_readable(b);
    memset(buf, 0, sizeof buf);
    socklen_t bl = sizeof from;
    r = recvfrom(b, buf, sizeof buf, 0, (struct sockaddr *)&from, &bl);
    printf("b got %zd '%s'\n", r, buf);
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    getpeername(a, (struct sockaddr *)&peer, &pl);
    printf("getpeername of the connected socket: b's port %d\n", peer.sin_port == b_addr.sin_port);
    struct sockaddr_in c_addr;
    int c = udp_socket(&c_addr);
    sendto(c, "stranger", 8, 0, (struct sockaddr *)&a_addr, sizeof a_addr);
    sendto(b, "friend", 6, 0, (struct sockaddr *)&a_addr, sizeof a_addr);
    wait_readable(a);
    usleep(50000);
    memset(buf, 0, sizeof buf);
    r = recv(a, buf, sizeof buf, MSG_DONTWAIT);
    printf("the connected socket receives %zd '%s' (the stranger's datagram was filtered out)\n", r, buf);
    errno = 0;
    r = recv(a, buf, sizeof buf, MSG_DONTWAIT);
    printf("and nothing else: %zd errno %d\n", r, errno);
    close(c);

    /* sendmmsg and recvmmsg */
    struct mmsghdr mm[3];
    struct iovec mv[3];
    const char *texts[3] = { "first", "second", "third" };
    memset(mm, 0, sizeof mm);
    for (int i = 0; i < 3; i++) {
        mv[i].iov_base = (void *)texts[i];
        mv[i].iov_len = strlen(texts[i]);
        mm[i].msg_hdr.msg_iov = &mv[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    int sm = sendmmsg(a, mm, 3, 0);
    printf("sendmmsg of three datagrams on the connected socket: %d, lengths %u %u %u\n", sm, mm[0].msg_len, mm[1].msg_len, mm[2].msg_len);
    wait_readable(b);
    usleep(50000);
    char rb[3][16];
    struct iovec rv[3];
    struct mmsghdr rm[3];
    memset(rm, 0, sizeof rm);
    memset(rb, 0, sizeof rb);
    for (int i = 0; i < 3; i++) {
        rv[i].iov_base = rb[i];
        rv[i].iov_len = sizeof rb[i];
        rm[i].msg_hdr.msg_iov = &rv[i];
        rm[i].msg_hdr.msg_iovlen = 1;
    }
    int got = recvmmsg(b, rm, 3, MSG_DONTWAIT, NULL);
    printf("recvmmsg: %d datagrams: '%s' (%u) '%s' (%u) '%s' (%u)\n", got, rb[0], rm[0].msg_len, rb[1], rm[1].msg_len, rb[2], rm[2].msg_len);

    /* the errors */
    int unconnected = socket(AF_INET, SOCK_DGRAM, 0);
    errno = 0;
    w = send(unconnected, "x", 1, 0);
    printf("send on a socket with no destination: %zd errno %d (EDESTADDRREQ %d)\n", w, errno, EDESTADDRREQ);
    static char huge[70000];
    errno = 0;
    w = sendto(unconnected, huge, sizeof huge, 0, (struct sockaddr *)&b_addr, sizeof b_addr);
    printf("a 70000-byte datagram: %zd errno %d (EMSGSIZE %d)\n", w, errno, EMSGSIZE);
    errno = 0;
    r = recv(unconnected, buf, sizeof buf, MSG_DONTWAIT);
    printf("recv with nothing queued: %zd errno %d\n", r, errno);
    struct pollfd pw = { unconnected, POLLOUT, 0 };
    poll(&pw, 1, 1000);
    printf("a UDP socket is writable: %d\n", (pw.revents & POLLOUT) != 0);
    close(unconnected);
    close(a);
    close(b);
    return 0;
}
