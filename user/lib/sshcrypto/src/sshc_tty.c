/*
 * Asking the person at the keyboard a question: a passphrase, a password, "Are you sure you want to continue connecting (yes/no)?". The question goes to the
 * controlling terminal (/dev/tty), not to standard output, so it works while the program's output is a pipe or a file, and the answer is read from it; with no
 * /dev/tty, standard input is used when that is a terminal. Where there is no terminal the answer is "no one to ask" and the caller decides what that means
 * (a refused host key, a skipped method). Secrets are read with the terminal's echo switched off, and the terminal is put back whatever happens: a signal that arrives
 * while the question is open restores the settings first and is then passed on to the program's default action.
 */
#include <sshc.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

bool sshc_batch;

static volatile int g_sig;                                /* (sig_atomic_t, which this libc does not name, is an int) */
static void on_signal(int sig) { g_sig = sig; }

int sshc_prompt(const char *prompt, bool echo, char *buf, size_t cap) {
    if (sshc_batch || cap < 2) return -1;
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    bool own = fd >= 0;
    if (fd < 0) {
        if (!isatty(STDIN_FILENO)) return -1;
        fd = STDIN_FILENO;
    }
    struct termios old, quiet;
    bool changed = false;
    if (!echo && tcgetattr(fd, &old) == 0) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        changed = tcsetattr(fd, TCSANOW, &quiet) == 0;
    }
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP };
    struct sigaction sa, saved[sizeof sigs / sizeof sigs[0]];
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;                                /* no SA_RESTART: a signal ends the read */
    g_sig = 0;
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, &saved[i]);

    ssize_t w = write(fd, prompt, strlen(prompt));
    (void)w;
    size_t n = 0;
    int rc = -1;
    for (;;) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r < 0 && errno == EINTR) { if (g_sig) break; continue; }
        if (r <= 0) { if (n) rc = 0; break; }                 /* end of file ends the answer; with nothing typed it is "no answer" */
        if (ch == '\n' || ch == '\r') { rc = 0; break; }
        if (n + 1 < cap) buf[n++] = ch;
    }
    buf[n] = 0;
    if (!echo && rc == 0) { w = write(fd, "\n", 1); (void)w; }
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &saved[i], NULL);
    if (changed) tcsetattr(fd, TCSANOW, &old);
    if (own) close(fd);
    if (g_sig) {                                              /* the terminal is back as it was: now let the signal do what it would have done */
        int s = g_sig;
        g_sig = 0;
        signal(s, SIG_DFL);
        raise(s);
        return -1;
    }
    return rc;
}
