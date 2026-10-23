/* passwd: change an account password in /etc/shadow. Installed setuid root. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>
#include <crypto.h>

int main(int argc, char **argv) {
    if (geteuid() != 0) { fprintf(stderr, "passwd: this program must be installed setuid root\n"); return 1; }
    struct passwd *me = getpwuid(getuid());
    char user[64];                                  /* a copy: pw lookups reuse one static buffer */
    snprintf(user, sizeof user, "%s", argc > 1 ? argv[1] : (me ? me->pw_name : ""));
    if (!user[0]) { fprintf(stderr, "passwd: who are you?\n"); return 1; }
    if (getuid() != 0 && (!me || strcmp(user, me->pw_name))) { fprintf(stderr, "passwd: you may only change your own password\n"); return 1; }
    if (!getpwnam(user)) { fprintf(stderr, "passwd: unknown user '%s'\n", user); return 1; }

    if (getuid() != 0) {
        char hash[160];
        if (gap_shadow_lookup(user, hash, sizeof hash) == 0 && hash[0] != 0 && !gap_authenticate(user, getpass("Current password: "))) {
            sleep(2);
            fprintf(stderr, "passwd: Authentication failure\n");
            return 1;
        }
    }
    printf("Changing the password for %s (leave it empty to remove the password).\n", user);
    char first[128], second[128];
    snprintf(first, sizeof first, "%s", getpass("New password: "));
    snprintf(second, sizeof second, "%s", getpass("Retype new password: "));
    if (strcmp(first, second)) { fprintf(stderr, "passwd: the passwords do not match; nothing changed\n"); return 1; }
    char hash[160] = "";
    if (first[0]) {
        char salt[12];
        gap_make_salt(salt, sizeof salt);
        gap_hash_password(first, salt, hash, sizeof hash);
    }
    if (gap_shadow_set(user, hash) < 0) { perror("passwd: cannot write /etc/shadow"); return 1; }
    printf("passwd: password updated for %s\n", user);
    return 0;
}
