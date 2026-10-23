/* hakurei-session: starts the window server and the desktop shell, and ends when either of them does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <gensokyo.h>

static pid_t spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* hakurei-session            the desktop: window server + hakurei-shell
 * hakurei-session --greeter  the login screen: window server + hakurei-greeter (run by init as root) */
int main(int argc, char **argv) {
    const char *front = "/bin/hakurei-shell";
    if (argc > 1 && !strcmp(argv[1], "--greeter")) front = "/bin/hakurei-greeter";
    gap_setname("session");
    pid_t server = spawn("/bin/fairyd");
    if (server < 0) { perror("hakurei-session: fork"); return 1; }
    struct stat st;
    int waited = 0;
    while (stat("/tmp/fairy/fairyd.sock", &st) < 0 && waited < 100) {          /* up to 5 s for the window server to listen */
        int status;
        if (waitpid(server, &status, WNOHANG) == server) { fprintf(stderr, "hakurei-session: the window server did not start\n"); return 2; }
        usleep(50000);
        waited++;
    }
    pid_t shell = spawn(front);
    int rc = 0;
    for (;;) {
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) break;
        if (pid == shell) { rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1; break; }
        if (pid == server) { rc = 3; break; }
    }
    if (rc != 3) kill(server, SIGTERM);
    kill(shell, SIGTERM);
    for (int i = 0; i < 40; i++) {                                              /* give them a moment, then insist */
        int status;
        pid_t a = waitpid(server, &status, WNOHANG), b = waitpid(shell, &status, WNOHANG);
        (void)a; (void)b;
        if (kill(server, 0) < 0 && kill(shell, 0) < 0) break;
        usleep(50000);
    }
    kill(server, SIGKILL);
    kill(shell, SIGKILL);
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
    return rc;
}
