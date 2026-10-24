/* compiled by mcc ON the guest: fork, pipes, wait, exec of a shell command, signals, qsort and the heap */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int got_signal;
static void on_usr1(int sig) { got_signal = sig; }

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    int fds[2];
    pipe(fds);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        const char *msg = "child says hi";
        write(fds[1], msg, strlen(msg));
        close(fds[1]);
        _exit(7);
    }
    close(fds[1]);
    char buf[64] = {0};
    read(fds[0], buf, sizeof buf - 1);
    int status = 0;
    waitpid(pid, &status, 0);
    printf("child: [%s] exited=%d status=%d\n", buf, WIFEXITED(status), WEXITSTATUS(status));

    pid = fork();
    if (pid == 0) {
        execl("/bin/echo", "echo", "exec works", (char *)NULL);
        _exit(99);
    }
    waitpid(pid, &status, 0);
    printf("exec: status=%d\n", WEXITSTATUS(status));

    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    printf("signal: got=%d\n", got_signal == SIGUSR1);

    int a[] = {9, 4, 7, 1, 8, 2};
    qsort(a, 6, sizeof a[0], cmp);
    for (int i = 0; i < 6; i++) printf("%d ", a[i]);
    printf("\n");
    char *big = malloc(1 << 20);
    memset(big, 'x', 1 << 20);
    big[(1 << 20) - 1] = 0;
    printf("1 MiB malloc: %d\n", (int)strlen(big));
    free(big);
    return 0;
}
