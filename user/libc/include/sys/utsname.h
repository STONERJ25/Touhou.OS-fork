#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
#include <abi.h>
struct utsname { char sysname[32], nodename[32], release[32], version[64], machine[32], codename[48]; };
int uname(struct utsname *u);
#endif
