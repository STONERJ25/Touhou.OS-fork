/* login: ask who is at the door, check the shadow file, and hand the shrine over to that user's shell. Run by init as root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <gensokyo.h>

int main(int argc, char **argv) {
    char user[64] = "";
    if (argc > 1) snprintf(user, sizeof user, "%s", argv[1]);
    for (int tries = 0; tries < 5; tries++) {
        if (!user[0]) {
            fputs("gensokyo login: ", stdout);
            fflush(stdout);
            if (!fgets(user, sizeof user, stdin)) return 1;
            user[strcspn(user, "\r\n")] = 0;
            if (!user[0]) continue;
        }
        char *pw = getpass("Password: ");
        struct passwd *p = getpwnam(user);
        if (p && gap_authenticate(user, pw)) {
            if (initgroups(user, p->pw_gid) < 0) perror("login: initgroups");
            setgid(p->pw_gid);
            setuid(p->pw_uid);
            chdir(p->pw_dir);
            setenv("HOME", p->pw_dir, 1);
            setenv("USER", user, 1);
            setenv("LOGNAME", user, 1);
            setenv("SHELL", p->pw_shell, 1);
            setenv("PATH", "/bin:/sbin:/usr/bin", 1);
            setenv("HOSTNAME", "gensokyo", 1);
            setenv("TERM", "xterm", 1);
            execl(p->pw_shell, p->pw_shell, "-l", (char *)NULL);
            perror("login: cannot start the shell");
            return 127;
        }
        sleep(2);
        puts("Login incorrect");
        user[0] = 0;
    }
    return 1;
}
