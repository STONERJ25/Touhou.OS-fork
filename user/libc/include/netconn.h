/* netconn: connecting to a name that has several addresses, of one or both families, without letting a broken one block the others.
 * A simplified "Happy Eyeballs" (RFC 8305): see user/libc/src/netconn.c for what is kept and what is left out. */
#ifndef NETCONN_H
#define NETCONN_H

#include <stddef.h>
#include <sys/socket.h>
#include <netdb.h>

#define NETCONN_ATTEMPT_MS  2000        /* one address gets this long to answer */
#define NETCONN_STAGGER_MS  250         /* the next address is started this long after the last one if nothing has answered yet (RFC 8305 section 5) */
#define NETCONN_MAX         32          /* addresses looked at; the rest are ignored */

/* Reorders v[0..n) as RFC 8305 section 4 asks: the family of the first address first, then the families alternate, each keeping its own order
 * (the order getaddrinfo gave, RFC 6724). Pure: no system calls. */
void netconn_interleave(const struct addrinfo **v, int n);

/* Tries the addresses of list (SOCK_STREAM entries; others are skipped) in interleaved order: an attempt is started, and the next one after
 * stagger_ms or at once when an attempt fails, each attempt given attempt_ms (once the last address has been started, the running ones get
 * up to total_ms if it is set); the first to connect wins and the others are closed.
 * total_ms bounds the whole (0: no bound but the attempts'). Returns a connected, blocking descriptor, or -1 with *err set to the reason
 * (ETIMEDOUT when only timeouts were seen, else the last error). *used (may be NULL) is the entry that won. */
int netconn_connect(const struct addrinfo *list, int attempt_ms, int stagger_ms, int total_ms, int *err, const struct addrinfo **used);

/* Resolves host (a name, an IPv4 or IPv6 literal, with or without [brackets]) for TCP port port with AI_ADDRCONFIG and connects as above.
 * Returns the descriptor, or -1: *gai is a getaddrinfo code when the name failed (0 otherwise), *err the errno of the connection.
 * ip (may be NULL) receives the text of the address that answered. */
int netconn_tcp(const char *host, int port, int attempt_ms, int total_ms, char *ip, size_t ipcap, int *gai, int *err);

#endif
