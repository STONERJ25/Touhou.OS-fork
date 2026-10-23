/*
 * init: the shrine maiden. PID 1 sets up the day, starts a shell for the resident of the Human Village and
 * respawns it when it leaves; it also adopts and reaps every orphan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <gensokyo.h>

static int cmdline_has(const char *word) {
    char buf[512];
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, word) != NULL;
}

static pid_t spawn_shell(const char *user, int uid, int gid, const char *home) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setpgid(0, 0);
    tcsetpgrp(0, getpid());
    signal(SIGINT, SIG_DFL);
    if (gid) setgid((gid_t)gid);
    if (uid) setuid((uid_t)uid);
    chdir(home);
    setenv("HOME", home, 1);
    setenv("USER", user, 1);
    setenv("LOGNAME", user, 1);
    setenv("SHELL", "/bin/gapsh", 1);
    setenv("PATH", "/bin:/sbin:/usr/bin", 1);
    setenv("HOSTNAME", "gensokyo", 1);
    setenv("TERM", "xterm", 1);
    execl("/bin/gapsh", "gapsh", "-l", (char *)NULL);
    perror("init: cannot start gapsh");
    _exit(127);
}

int main(void) {
    gap_setname("init");
    signal(SIGINT, SIG_IGN);
    setsid();

    if (cmdline_has("utest")) {
        printf("[Hakurei] running the user-space test suite\n");
        pid_t pid = fork();
        if (pid == 0) { execl("/bin/utest", "utest", (char *)NULL); _exit(127); }
        int status = 0;
        waitpid(pid, &status, 0);
        gap_reboot(0x100 | (WIFEXITED(status) ? WEXITSTATUS(status) : 99));
    }

    printf("\x1b[1;31m[Hakurei]\x1b[0m the shrine opens for the day (init, pid %d)\n", getpid());
    const char *user = "reimu";
    int uid = 1000, gid = 1000;
    const char *home = "/HumanVillage/reimu";
    if (cmdline_has("user=root")) { user = "root"; uid = 0; gid = 0; home = "/root"; }

    pid_t shell = spawn_shell(user, uid, gid, home);
    for (;;) {
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) { sleep(1); continue; }
        if (pid == shell) {
            printf("\x1b[1;31m[Hakurei]\x1b[0m %s left the shrine (status %d); a new maiden steps in\n", user,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status));
            shell = spawn_shell(user, uid, gid, home);
        }
    }
}
