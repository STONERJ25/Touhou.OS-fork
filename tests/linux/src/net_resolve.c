/*
 * Name lookup through a statically linked C library: getaddrinfo and gethostbyname for "localhost" (the C library reads /etc/hosts and, for names it does not find
 * there, would ask the servers of /etc/resolv.conf over UDP), numeric addresses, the error for a name that does not exist, and getnameinfo for a numeric address.
 * Prints only what the manual pages promise, so a real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_net.py compares.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int main(void) {
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo("localhost", "8080", &hints, &res);
    int found = 0, port_ok = 0;
    for (p = res; rc == 0 && p; p = p->ai_next) {
        struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
        if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) found = 1;
        if (ntohs(sin->sin_port) == 8080) port_ok = 1;
    }
    printf("getaddrinfo(localhost, 8080): %d, 127.0.0.1 among the results %d, port 8080 %d, socktype stream %d\n", rc, found, port_ok,
           res && res->ai_socktype == SOCK_STREAM);
    if (res) freeaddrinfo(res);

    struct hostent *h = gethostbyname("localhost");
    printf("gethostbyname(localhost): found %d, first address 127.0.0.1 %d\n", h != NULL, h && h->h_addrtype == AF_INET && h->h_addr_list[0] &&
           memcmp(h->h_addr_list[0], "\x7f\x00\x00\x01", 4) == 0);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_NUMERICHOST;
    rc = getaddrinfo("192.0.2.33", "443", &hints, &res);
    printf("numeric host: %d", rc);
    if (rc == 0) {
        char text[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, text, sizeof text);
        printf(", %s port %d", text, ntohs(((struct sockaddr_in *)res->ai_addr)->sin_port));
        freeaddrinfo(res);
    }
    printf("\n");
    rc = getaddrinfo("not-an-address", "80", &hints, &res);
    printf("a name with AI_NUMERICHOST: %d (EAI_NONAME %d)\n", rc, EAI_NONAME);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(8080);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char host[64], serv[16];
    rc = getnameinfo((struct sockaddr *)&sin, sizeof sin, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
    printf("getnameinfo numeric: %d, %s:%s\n", rc, host, serv);
    return 0;
}
