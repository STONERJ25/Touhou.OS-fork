/* eiki (sudo): the judge decides who may borrow root's power. Installed setuid root; `sudo` is a link to it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <gensokyo.h>

/* /etc/sudoers: "user", "user NOPASSWD", "%group", "%group NOPASSWD"; # starts a comment */
static bool listed(const char *user, gid_t gid, bool *nopasswd) {
    FILE *f = fopen("/etc/sudoers", "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    gid_t groups[16];
    int ng = getgroups(16, groups);
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *name = strtok(line, " \t\r\n");
        if (!name) continue;
        char *opt = strtok(NULL, " \t\r\n");
        bool match = false;
        if (name[0] == '%') {
            struct group *g = getgrnam(name + 1);
            if (g) {
                if (g->gr_gid == gid) match = true;
                for (int i = 0; i < ng; i++) if (groups[i] == g->gr_gid) match = true;
            }
        } else match = !strcmp(name, user) || !strcmp(name, "ALL");
        if (match) { ok = true; if (opt && !strcmp(opt, "NOPASSWD")) *nopasswd = true; }
    }
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    const char *target = "root";
    int i = 1;
    bool login_shell = false;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "-s")) login_shell = true;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "usage: eiki [-u user] [-i] command [args...]\n"); return 2; }
    }
    if (i >= argc && !login_shell) { fprintf(stderr, "usage: eiki [-u user] [-i] command [args...]\n"); return 2; }
    if (geteuid() != 0) { fprintf(stderr, "eiki: this program must be installed setuid root (Eiki has no authority here)\n"); return 1; }

    uid_t uid = getuid();
    struct passwd *me = getpwuid(uid);
    char who[64];                                   /* a copy: the next pw lookup overwrites the static entry */
    snprintf(who, sizeof who, "%s", me ? me->pw_name : "?");
    bool nopasswd = false;
    if (uid != 0 && !listed(who, getgid(), &nopasswd)) {
        fprintf(stderr, "%s is not in the sudoers file. This incident will be reported to Eiki.\n", who);
        char note[96];
        snprintf(note, sizeof note, "%s tried to borrow root's power and was refused", who);
        gap_setname("Eiki");                    /* the klog tag is the process name */
        gap_log(note);
        return 1;
    }
    char hash[160];
    if (uid != 0 && !nopasswd && gap_shadow_lookup(who, hash, sizeof hash) == 0 && hash[0] != 0) {
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            char prompt[96];
            snprintf(prompt, sizeof prompt, "[eiki] password for %s: ", who);
            ok = gap_authenticate(who, getpass(prompt));
            if (!ok) { sleep(1); fputs("Sorry, try again.\n", stderr); }
        }
        if (!ok) { fputs("eiki: three incorrect passwords\n", stderr); return 1; }
    }

    struct passwd *tp = getpwnam(target);
    if (!tp) { fprintf(stderr, "eiki: unknown user '%s'\n", target); return 1; }
    if (initgroups(target, tp->pw_gid) < 0) perror("eiki: initgroups");
    setgid(tp->pw_gid);
    if (setuid(tp->pw_uid) < 0) { perror("eiki: setuid"); return 1; }
    setenv("HOME", tp->pw_dir, 1);
    setenv("USER", target, 1);
    setenv("LOGNAME", target, 1);
    setenv("SUDO_USER", who, 1);
    if (login_shell || i >= argc) {
        chdir(tp->pw_dir);
        execl(tp->pw_shell, tp->pw_shell, "-l", (char *)NULL);
        perror("eiki: cannot start the shell");
        return 127;
    }
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "eiki: %s: command not found\n", argv[i]);
    return 127;
}
