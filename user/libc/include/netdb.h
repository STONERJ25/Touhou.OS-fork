#ifndef _NETDB_H
#define _NETDB_H
#include <sys/socket.h>
#include <netinet/in.h>

/* ---- the old interface ---- */
struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;                      /* AF_INET */
    int    h_length;                        /* 4 */
    char **h_addr_list;                     /* network byte order addresses, NULL terminated */
};
#define h_addr h_addr_list[0]

extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

struct hostent *gethostbyname(const char *name);
struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type);       /* /etc/hosts only */
const char *hstrerror(int err);

struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;                          /* network byte order */
    char  *s_proto;
};
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);                     /* port in network byte order */

/* ---- the new interface (IPv4 only) ---- */
struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_V4MAPPED    0x08
#define AI_ALL         0x10
#define AI_ADDRCONFIG  0x20
#define AI_NUMERICSERV 0x400

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

#define EAI_BADFLAGS  (-1)
#define EAI_NONAME    (-2)
#define EAI_AGAIN     (-3)
#define EAI_FAIL      (-4)
#define EAI_FAMILY    (-6)
#define EAI_SOCKTYPE  (-7)
#define EAI_SERVICE   (-8)
#define EAI_MEMORY    (-10)
#define EAI_SYSTEM    (-11)
#define EAI_OVERFLOW  (-12)

int  getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *ai);
const char *gai_strerror(int code);
int  getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);

/* ---- Gensokyo extensions: the resolver one level down (nslookup and friends) ---- */
#define DNS_A     1
#define DNS_NS    2
#define DNS_CNAME 5
#define DNS_SOA   6
#define DNS_PTR   12
#define DNS_MX    15
#define DNS_TXT   16
#define DNS_AAAA  28
#define DNS_ANY   255

/* The name servers of /etc/resolv.conf ("nameserver ADDR" or "nameserver ADDR#PORT"); returns how many were stored. */
int gap_dns_servers(struct sockaddr_in *out, int max);
/* Sends one query to one server (UDP, retried) and stores the raw answer; returns its length, or -1 with h_errno set. */
int gap_dns_query(const struct sockaddr_in *server, const char *name, int qtype, unsigned char *answer, int cap, int timeout_ms);
/* Walks a raw answer: calls fn for every resource record with its owner name, type, class, ttl and rdata (rdata is relative
 * to msg, so names inside it can be expanded with gap_dns_name). Returns the response code (0 = no error) or -1 if malformed. */
typedef void (*gap_dns_rr_fn)(void *ctx, int section, const char *name, int type, int cls, unsigned ttl, const unsigned char *msg, int msglen,
                              const unsigned char *rdata, int rdlen);
int gap_dns_walk(const unsigned char *msg, int len, gap_dns_rr_fn fn, void *ctx);
int gap_dns_name(const unsigned char *msg, int msglen, const unsigned char *at, char *out, int outlen);   /* expands a possibly compressed name */

#endif
