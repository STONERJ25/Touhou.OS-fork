/* Helpers shared by the network applets (net_*.c). Addresses are in host byte order unless the name says otherwise. */
#ifndef KAPPABOX_NETUTIL_H
#define KAPPABOX_NETUTIL_H

#include "kappabox.h"
#include <stdint.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

/* "10.0.2.15" in one of eight rotating static buffers */
char *nu_ip(uint32_t ip);
/* "a.b.c.d" or "a.b.c.d/len": returns 1 and fills ip (and prefix, -1 when absent); 0 if it is not an address */
int nu_parse_cidr(const char *s, uint32_t *ip, int *prefix);
uint32_t nu_prefix_mask(int len);
int nu_mask_len(uint32_t mask);                 /* -1 if the mask is not contiguous */
/* resolves a host name or dotted address to a host-order IPv4 address; prints an error and returns 0 on failure */
int nu_resolve(const char *host, uint32_t *ip);
/* IPv6: "fe80::1" in one of eight rotating static buffers (the shortest form) */
char *nu_ip6(const uint8_t a[16]);
/* "ADDR" or "ADDR/LEN" with a colon in it: returns 1 and fills addr (and prefix, -1 when absent); 0 if it is not an IPv6 address */
int nu_parse_cidr6(const char *s, uint8_t addr[16], int *prefix);
/* "global", "link", "host" or "site": the scope of an address as /proc/net/if_inet6 and netctl give it (G6S_*) */
const char *nu_scope6(unsigned scope);
/* resolves a host name or an address of either family to a socket address. family: AF_INET, AF_INET6, or AF_UNSPEC (IPv4 first, IPv6 if the name only has that or is one);
 * prints an error and returns 0 on failure */
int nu_resolve_sa(const char *host, int family, struct sockaddr_storage *out, socklen_t *len);
/* the numeric text of a socket address ("10.0.2.2", "fe80::1%eth0") */
const char *nu_sa_str(const struct sockaddr *sa, char *buf, size_t n);
/* milliseconds on the monotonic clock */
long nu_now_ms(void);
/* "52:54:00:12:34:56" */
char *nu_mac(const uint8_t *mac);

#endif
