#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>

#define INET_ADDRSTRLEN 16

in_addr_t   inet_addr(const char *s);                            /* INADDR_NONE on error */
int         inet_aton(const char *s, struct in_addr *out);       /* 1 if valid; accepts a.b.c.d, a.b.c, a.b and a */
char       *inet_ntoa(struct in_addr in);                        /* static buffer */
int         inet_pton(int af, const char *src, void *dst);       /* 1 ok, 0 not an address, -1 unsupported family */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#endif
