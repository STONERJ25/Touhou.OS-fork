#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>
#include <abi.h>
struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);
#endif
