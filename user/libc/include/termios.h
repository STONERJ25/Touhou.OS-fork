#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <abi.h>
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_cc[NCCS]; };
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
int ioctl(int fd, unsigned long req, ...);
#endif
