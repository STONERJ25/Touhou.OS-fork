/* su: become another user (root unless named) in a login shell. Installed setuid root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>

int main(int argc, char **argv) {
    const char *target = "root";
    bool cmd = false;
    char *command = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l")) continue;
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { cmd = true; command = argv[++i]; continue; }
        target = argv[i];
    }
    if (geteuid() != 0) { fprintf(stderr, "su: this program must be installed setuid root\n"); return 1; }
    struct passwd *tp = getpwnam(target);
    if (!tp) { fprintf(stderr, "su: user %s does not exist\n", target); return 1; }
    if (getuid() != 0) {
        char hash[160];
        bool open_account = gap_shadow_lookup(target, hash, sizeof hash) == 0 && hash[0] == 0;   /* empty hash: no password needed */
        if (!open_account) {
            bool ok = false;
            for (int tries = 0; tries < 3 && !ok; tries++) {
                ok = gap_authenticate(target, getpass("Password: "));
                if (!ok) { sleep(2); fputs("su: Authentication failure\n", stderr); }
            }
            if (!ok) return 1;
        }
    }
    initgroups(target, tp->pw_gid);
    setgid(tp->pw_gid);
    if (setuid(tp->pw_uid) < 0) { perror("su: setuid"); return 1; }
    setenv("HOME", tp->pw_dir, 1);
    setenv("USER", target, 1);
    setenv("LOGNAME", target, 1);
    setenv("SHELL", tp->pw_shell, 1);
    chdir(tp->pw_dir);
    if (cmd) execl(tp->pw_shell, tp->pw_shell, "-c", command, (char *)NULL);
    else execl(tp->pw_shell, tp->pw_shell, "-l", (char *)NULL);
    perror("su: cannot start the shell");
    return 127;
}
