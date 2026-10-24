/* compiled by mcc ON the guest: a TCP echo on the loopback address and a socketpair */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int listen_fd;

static void *server(void *arg) {
    (void)arg;
    int c = accept(listen_fd, NULL, NULL);
    char buf[64];
    ssize_t n;
    while ((n = read(c, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        write(c, buf, (size_t)n);
    }
    close(c);
    return NULL;
}

int main(void) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 4) < 0) {
        printf("bind/listen failed\n");
        return 1;
    }
    socklen_t len = sizeof a;
    getsockname(listen_fd, (struct sockaddr *)&a, &len);
    printf("listening on loopback: port assigned=%d\n", ntohs(a.sin_port) != 0);

    pthread_t th;
    pthread_create(&th, NULL, server, NULL);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("connect failed\n");
        return 1;
    }
    const char *msgs[] = {"hello marisa", "master spark", "danmaku is power"};
    for (int i = 0; i < 3; i++) {
        char reply[64] = {0};
        write(c, msgs[i], strlen(msgs[i]));
        size_t got = 0;
        while (got < strlen(msgs[i])) {
            ssize_t n = read(c, reply + got, sizeof reply - 1 - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        printf("sent [%s] got [%s]\n", msgs[i], reply);
    }
    close(c);
    pthread_join(th, NULL);
    close(listen_fd);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[0], "ping", 4);
    char buf[8] = {0};
    read(sv[1], buf, 4);
    write(sv[1], "pong", 4);
    char buf2[8] = {0};
    read(sv[0], buf2, 4);
    printf("socketpair: %s %s\n", buf, buf2);
    printf("htons/htonl: %04x %08x\n", htons(0x1234), htonl(0x11223344));
    return 0;
}
