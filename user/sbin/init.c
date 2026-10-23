/*
 * init: the shrine maiden. PID 1 sets up the day, lets a resident of the Human Village in, and puts a new one at the
 * door whenever the previous one leaves; it also adopts and reaps every orphan.
 *
 * Kernel command line:   text            no desktop, a text shell on the console
 *                        nogui           same
 *                        login           ask who is at the door (login screen, or "login:" in text mode)
 *                        autologin=NAME  skip the question and open NAME's session (default reimu; "none" = ask)
 *                        user=root       legacy spelling of autologin=root
 *                        utest           run the user-space test suite and power off
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <gensokyo.h>

#define TAG "\x1b[1;31m[Hakurei]\x1b[0m"

static char g_cmdline[512];

static void read_cmdline(void) {
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, g_cmdline, sizeof g_cmdline - 1);
    close(fd);
    if (n > 0) g_cmdline[n] = 0;
}

/* The command line is a list of space-separated words; "login" must not match "autologin=reimu". */
static bool cmdline_has(const char *word) {
    size_t len = strlen(word);
    for (const char *p = g_cmdline; *p;) {
        while (*p == ' ' || *p == '\n') p++;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\n') end++;
        if ((size_t)(end - p) == len && !strncmp(p, word, len)) return true;
        p = end;
    }
    return false;
}

static bool cmdline_get(const char *prefix, char *out, size_t n) {
    size_t len = strlen(prefix);
    for (const char *p = g_cmdline; *p;) {
        while (*p == ' ' || *p == '\n') p++;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\n') end++;
        if ((size_t)(end - p) > len && !strncmp(p, prefix, len)) {
            size_t v = (size_t)(end - p) - len;
            if (v >= n) v = n - 1;
            memcpy(out, p + len, v);
            out[v] = 0;
            return true;
        }
        p = end;
    }
    return false;
}

typedef struct { char name[32]; int uid, gid; char home[96], shell[64]; } Account;

static bool find_account(const char *name, Account *a) {
    struct passwd *p = getpwnam(name);
    if (!p) return false;
    snprintf(a->name, sizeof a->name, "%s", p->pw_name);
    a->uid = (int)p->pw_uid;
    a->gid = (int)p->pw_gid;
    snprintf(a->home, sizeof a->home, "%s", p->pw_dir);
    snprintf(a->shell, sizeof a->shell, "%s", p->pw_shell);
    return true;
}

static void base_environment(const char *user, const char *home, const char *shell) {
    chdir(home);
    setenv("HOME", home, 1);
    setenv("USER", user, 1);
    setenv("LOGNAME", user, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", "/bin:/sbin:/usr/bin", 1);
    setenv("HOSTNAME", "gensokyo", 1);
    setenv("TERM", "xterm", 1);
}

/* Starts the shell, or the desktop session, as the resident on the console. */
static pid_t spawn_session(const Account *a, bool desktop) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setpgid(0, 0);
    tcsetpgrp(0, getpid());
    signal(SIGINT, SIG_DFL);
    if (a->uid) initgroups(a->name, (gid_t)a->gid);
    if (a->gid) setgid((gid_t)a->gid);
    if (a->uid) setuid((uid_t)a->uid);
    base_environment(a->name, a->home, a->shell);
    if (desktop) {
        execl("/bin/hakurei-session", "hakurei-session", (char *)NULL);
        perror("init: cannot start the desktop");
        _exit(127);
    }
    execl(a->shell, a->shell, "-l", (char *)NULL);
    perror("init: cannot start the shell");
    _exit(127);
}

/* The text-mode door: login asks for a name and a password, then becomes that resident's shell. */
static pid_t spawn_text_login(void) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    setpgid(0, 0);
    tcsetpgrp(0, getpid());
    signal(SIGINT, SIG_DFL);
    base_environment("root", "/root", "/bin/gapsh");
    execl("/sbin/login", "login", (char *)NULL);
    perror("init: cannot start login");
    _exit(127);
}

/* The graphical door: a private window server and the greeter, as root. The greeter writes the name of whoever it let
 * in to the pipe's write end, which the child sees as fd 3. */
static pid_t spawn_greeter(int *read_end) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (fds[1] != 3) { dup2(fds[1], 3); close(fds[1]); }
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        base_environment("root", "/root", "/bin/gapsh");
        setenv("GREETER_FD", "3", 1);
        execl("/bin/hakurei-session", "hakurei-session", "--greeter", (char *)NULL);
        perror("init: cannot start the login screen");
        _exit(127);
    }
    close(fds[1]);
    *read_end = fds[0];
    return pid;
}

static bool read_greeted(int fd, char *name, size_t n) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (fd < 0 || poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN)) return false;
    ssize_t got = read(fd, name, n - 1);
    if (got <= 0) return false;
    name[got] = 0;
    name[strcspn(name, "\r\n")] = 0;
    return name[0] != 0;
}

/* Time zone: tz=... on the command line wins over /etc/timezone (which libc reads by itself). If the clock chip keeps local
 * time (rtc=local: a PC that also runs Windows does that) the kernel read it as UTC, so correct the system clock. */
static void setup_clock(void) {
    char tz[96];
    if (cmdline_get("tz=", tz, sizeof tz) && tz[0]) setenv("TZ", tz, 1);
    tzset();
    if (cmdline_has("rtc=local")) {
        time_t raw = time(NULL);
        struct tm tm;
        gmtime_r(&raw, &tm);
        gap_settime((long)mktime(&tm) * 1000, 0);
    }
}

/* After a logout nothing of the previous resident should linger on the machine. */
static void send_home(int uid) {
    if (uid == 0) return;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 1) continue;
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        char *u = strstr(buf, "Uid:");
        if (u && atoi(u + 4) == uid) kill(pid, SIGKILL);
    }
    closedir(d);
}

int main(void) {
    gap_setname("init");
    signal(SIGINT, SIG_IGN);
    setsid();
    read_cmdline();

    if (cmdline_has("utest")) {
        printf("[Hakurei] running the user-space test suite\n");
        pid_t pid = fork();
        if (pid == 0) { execl("/bin/utest", "utest", (char *)NULL); _exit(127); }
        int status = 0;
        waitpid(pid, &status, 0);
        gap_reboot(0x100 | (WIFEXITED(status) ? WEXITSTATUS(status) : 99));
    }

    setup_clock();
    printf(TAG " the shrine opens for the day (init, pid %d)\n", getpid());

    char autologin[32] = "reimu";
    if (cmdline_has("user=root")) snprintf(autologin, sizeof autologin, "root");
    cmdline_get("autologin=", autologin, sizeof autologin);
    bool ask = cmdline_has("login") || !strcmp(autologin, "none");
    Account cur;
    memset(&cur, 0, sizeof cur);
    if (!ask && !find_account(autologin, &cur)) {
        printf(TAG " there is no resident called '%s'; asking at the door instead\n", autologin);
        ask = true;
    }

    /* Boot into the desktop when there is a screen to draw on, unless "text" is on the command line. A desktop that dies
     * (rather than being logged out of) drops back to the text shell so the machine is never left without a console. */
    bool in_desktop = access("/dev/fb0", F_OK) == 0 && access("/bin/hakurei-session", X_OK) == 0 && !cmdline_has("text") && !cmdline_has("nogui");
    bool have_greeter = in_desktop && access("/bin/hakurei-greeter", X_OK) == 0;
    bool text_login = cmdline_has("login") || !strcmp(autologin, "none");       /* the text door stays open for later arrivals */
    int greeter_failures = 0;
    bool text_door_once = false;                                         /* the login screen's "Text console" button */

    enum { ST_SESSION, ST_GREETER, ST_LOGIN } state = ST_SESSION;
    int greeter_fd = -1;
    pid_t child = 0;
    long spawned_ms = 0;

    for (;;) {
        if (child == 0) {
            if (ask) {
                if (in_desktop && have_greeter && greeter_failures < 3 && !text_door_once) {
                    printf(TAG " opening the login screen\n");
                    child = spawn_greeter(&greeter_fd);
                    state = ST_GREETER;
                } else {
                    child = spawn_text_login();
                    state = ST_LOGIN;
                    text_door_once = false;
                }
            } else {
                child = spawn_session(&cur, in_desktop);
                state = ST_SESSION;
                if (in_desktop) printf(TAG " opening the desktop for %s\n", cur.name);
            }
        }
        if (!spawned_ms) spawned_ms = gap_time_ms();
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0) { sleep(1); continue; }
        if (pid != child) continue;                                      /* an adopted orphan */
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        child = 0;
        if (gap_time_ms() - spawned_ms < 1000) sleep(1);                 /* something that dies at once must not spin the machine */
        spawned_ms = 0;

        if (state == ST_GREETER) {
            char who[32] = "";
            bool let_in = code == 0 && read_greeted(greeter_fd, who, sizeof who) && find_account(who, &cur);
            if (greeter_fd >= 0) close(greeter_fd);
            greeter_fd = -1;
            if (let_in) {
                greeter_failures = 0;
                ask = false;
            } else if (code == 10) {                                     /* "Text console" */
                text_door_once = true;
                printf(TAG " the text door is open; the login screen returns once it closes\n");
            } else {
                greeter_failures++;
                printf(TAG " the login screen ended (status %d)%s\n", code,
                       greeter_failures >= 3 ? "; the text door will do instead" : "; trying again");
            }
        } else if (state == ST_LOGIN) {
            ask = true;                                                  /* whoever it was has left: back to the door */
        } else if (in_desktop && code != 0) {
            /* A desktop that dies (or is shot down with Ctrl+Alt+Backspace) must not hand out a shell for free: the screen lock
             * may have been up. The text door asks who is there; when that session ends the login screen tries again. */
            printf(TAG " the desktop ended (status %d): the text door is open, then the login screen returns\n", code);
            send_home(cur.uid);
            text_door_once = true;
            ask = true;
        } else if (in_desktop) {
            printf(TAG " %s left the shrine; the gate closes behind them\n", cur.name);
            send_home(cur.uid);
            if (have_greeter) ask = true;
        } else {
            printf(TAG " %s left the shrine (status %d); a new maiden steps in\n", cur.name, code);
            send_home(cur.uid);
            if (text_login) ask = true;
        }
    }
}
