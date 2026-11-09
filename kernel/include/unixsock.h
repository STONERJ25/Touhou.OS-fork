/*
 * AF_UNIX sockets (kernel/fs/socket.c): what the Internet-socket code (kernel/net/sockcall.c) and the Linux system call layer (kernel/linux/net.c) need to
 * serve a descriptor that is a UNIX-domain socket. The system calls themselves (socket, bind, listen, accept, connect, shutdown, socketpair) are registered
 * by socket.c; everything here works on kernel buffers, so a caller that gathers a message from several user buffers does it first.
 *
 * Two kinds of socket live here: streams (a connection is a pair of byte queues) and datagrams (every socket has a queue of whole messages that any socket that
 * knows its name, or was connected to it, can add to). Both can carry descriptors in a message (SCM_RIGHTS): the sender's open files are held by the message
 * and become descriptors of the receiver when it reads the message with a control buffer.
 */
#ifndef K_UNIXSOCK_H
#define K_UNIXSOCK_H

#include "vfs.h"

#define UNIX_PATH_MAX 108               /* sizeof(((struct sockaddr_un *)0)->sun_path) */
#define UNIX_MAX_FDS  253               /* descriptors in one message (SCM_MAX_FD) */

bool    unix_is_sock(const File *f);
bool    unix_is_dgram(const File *f);

/* Stream and datagram I/O with the MSG_* flags of send(2) and recv(2): MSG_DONTWAIT, MSG_PEEK, MSG_WAITALL, MSG_NOSIGNAL. The result is the byte count, 0 at end of
 * file, or a negative errno. unix_send raises SIGPIPE on a broken connection unless MSG_NOSIGNAL is given. A datagram goes to the socket it was connected to; a
 * datagram read into a buffer that is too small loses the rest. */
ssize_t unix_send(File *f, const void *buf, size_t n, int mflags);
ssize_t unix_recv(File *f, void *buf, size_t n, int mflags);

/* The same with a destination and descriptors. `to` is a sockaddr_un in user memory (0: none) with its length. fds holds nfds open files whose references are
 * handed to the message (also when the call fails: the callee puts them). On a stream the descriptors travel with the first byte of the data. */
ssize_t unix_send_msg(File *f, const void *buf, size_t n, int mflags, u64 to, u64 tolen, File **fds, int nfds);

/* What a receive found besides the bytes. The caller owns fds (references, kmalloc'd array) and must install or put them. */
typedef struct UnixCtl {
    bool   want_fds;                    /* set by the caller before the call: it takes the descriptors (otherwise they are dropped with the message) */
    int    nfds;
    File **fds;
    bool   have_cred;                   /* the sender's credentials, for a receiver that asked for them (SO_PASSCRED) */
    int    pid;
    u32    uid, gid;
    bool   passcred;                    /* the receiving socket has SO_PASSCRED set */
    u8     fromlen;                     /* the sender's name (datagrams), 0 if it has none */
    char   from[UNIX_PATH_MAX];
    size_t full;                        /* the length of the datagram, which may be more than was stored */
    bool   dgram;
} UnixCtl;
ssize_t unix_recv_msg(File *f, void *buf, size_t n, int mflags, UnixCtl *ctl);

/* The sockaddr_un of this end (peer == false) or the other end (peer == true), stored at uaddr with the length rules of getsockname(2): at most *ulenp bytes
 * are stored and *ulenp becomes the full length (2 for a socket without a name). -ENOTCONN for the peer of a socket that is not connected. */
long    unix_getname(File *f, bool peer, u64 uaddr, u64 ulenp);
long    unix_store_name(u64 uaddr, u64 ulenp, const char *nm, u8 nlen);       /* the same for a name already in hand */

/* getsockopt for a UNIX socket at the SOL_SOCKET level: fills out (16 bytes) and *olen. -ENOPROTOOPT for an option it does not know. */
long    unix_getsockopt(File *f, int level, int name, u8 out[16], size_t *olen);
long    unix_setsockopt(File *f, int level, int name, const u8 *val, size_t len);

#endif
