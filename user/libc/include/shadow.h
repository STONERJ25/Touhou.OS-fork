#ifndef _SHADOW_H
#define _SHADOW_H
#include <stddef.h>

/* /etc/shadow: one "name:hash:..." line per account. hash is "$gap$salt$hex", empty (no password needed) or "!" (locked). */
int  gap_shadow_lookup(const char *user, char *hash, size_t n);     /* 0 and the hash, or -1 when there is no entry */
int  gap_shadow_set(const char *user, const char *hash);            /* rewrites the file; needs root. 0 or -1 */
/* 1 if `password` is acceptable for `user`: an empty hash accepts an empty password, "!" and "*" accept nothing. */
int  gap_authenticate(const char *user, const char *password);
char *getpass(const char *prompt);                                   /* reads a line from /dev/tty without echo */

#endif
