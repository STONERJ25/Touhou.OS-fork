/*
 * init: the shrine maiden. PID 1 sets up the day, starts a shell for the resident of the Human Village and
 * respawns it when it leaves; it also adopts and reaps every orphan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <grp.h>
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

/* Starts the login shell, or the desktop session, as `user` on the console. */
static pid_t spawn_login(const char *user, int uid, int gid, const char *home, bool desktop) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setpgid(0, 0);
    tcsetpgrp(0, getpid());
    signal(SIGINT, SIG_DFL);
    if (uid) initgroups(user, (gid_t)gid);
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
    if (desktop) {
        execl("/bin/hakurei-session", "hakurei-session", (char *)NULL);
        perror("init: cannot start the desktop");
        _exit(127);
    }
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

    /* Boot into the desktop when there is a screen to draw on, unless "text" is on the command line. A desktop that dies
     * (rather than being logged out of) drops back to the text shell so the machine is never left without a console. */
    bool want_desktop = access("/dev/fb0", F_OK) == 0 && access("/bin/hakurei-session", X_OK) == 0 && !cmdline_has("text") && !cmdline_has("nogui");
    bool in_desktop = want_desktop;
    pid_t shell = spawn_login(user, uid, gid, home, in_desktop);
    if (in_desktop) printf("\x1b[1;31m[Hakurei]\x1b[0m opening the desktop for %s\n", user);
    for (;;) {
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) { sleep(1); continue; }
        if (pid == shell) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
            if (in_desktop && code != 0) {
                printf("\x1b[1;31m[Hakurei]\x1b[0m the desktop ended (status %d): here is the text shell; type 'desktop' to try again\n", code);
                in_desktop = false;
            } else if (!in_desktop) {
                printf("\x1b[1;31m[Hakurei]\x1b[0m %s left the shrine (status %d); a new maiden steps in\n", user, code);
            }
            shell = spawn_login(user, uid, gid, home, in_desktop);
        }
    }
}
