/*
 * pwcheck: is this the password of the user who runs me? Reads one line from standard input and exits 0 if it is.
 *
 * Installed setuid root so that a program started by an ordinary user (the screen lock) can ask the question without being
 * able to read /etc/shadow itself. It only ever checks the caller's own account, so it is no use for guessing anyone
 * else's password.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>

int main(void) {
    struct passwd *p = getpwuid(getuid());
    if (!p) return 2;
    char name[64];
    snprintf(name, sizeof name, "%s", p->pw_name);
    char line[128];
    if (!fgets(line, sizeof line, stdin)) line[0] = 0;
    line[strcspn(line, "\r\n")] = 0;
    return gap_authenticate(name, line) ? 0 : 1;
}
