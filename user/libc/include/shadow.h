#ifndef _SHADOW_H
#define _SHADOW_H
#include <stddef.h>

/* /etc/shadow: one "name:hash:..." line per account. hash is "$gap$salt$hex", empty (no password needed) or "!" (locked). */
int  gap_shadow_lookup(const char *user, char *hash, size_t n);     /* 0 and the hash, or -1 when there is no entry */
/* Rewrites the file through etc_commit() (etcstore.h): needs root. 0 done, 1 changed in /etc but the HUMANVILLAGE volume could not
 * keep it (etc_note() says why), -1 nothing changed. */
int  gap_shadow_set(const char *user, const char *hash);
/* 1 if `password` is acceptable for `user`: an empty hash accepts an empty password, "!" and "*" accept nothing. */
int  gap_authenticate(const char *user, const char *password);
char *getpass(const char *prompt);                                   /* reads a line from /dev/tty without echo */

#endif
