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
/* milliseconds on the monotonic clock */
long nu_now_ms(void);
/* "52:54:00:12:34:56" */
char *nu_mac(const uint8_t *mac);

#endif
