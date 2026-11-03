#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgroups(int n, gid_t list[]);
int setgroups(size_t n, const gid_t *list);
int initgroups(const char *user, gid_t gid);        /* supplementary groups from /etc/group; root only */

#endif
