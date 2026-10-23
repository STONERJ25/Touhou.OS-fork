#ifndef _PTY_H
#define _PTY_H
#include <stddef.h>
#include <termios.h>

/* Creates a pseudo-terminal pair. ptyopen() returns the master descriptor and stores the slave path ("/dev/pts/N"). */
int ptyopen(char *slave_path, size_t size);
int openpty(int *master, int *slave, char *name, const struct termios *term, const struct winsize *ws);

#endif
