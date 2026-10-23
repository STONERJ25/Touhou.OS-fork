/* Gensokyo-specific system interfaces: system information, power, mounts, logging. */
#ifndef _GENSOKYO_H
#define _GENSOKYO_H
#include <abi.h>
#include <syscall.h>
#include <keys.h>

int  gap_sysinfo(struct gsysinfo *si);
int  gap_reboot(int how);                 /* REBOOT_RESTART or REBOOT_POWEROFF */
int  gap_mount(const char *src, const char *target, const char *fstype, unsigned long flags);
int  gap_mount_opts(const char *src, const char *target, const char *fstype, unsigned long flags, const char *opts);
int  gap_umount(const char *target);
void gap_setname(const char *name);
void gap_log(const char *msg);
long gap_time_ms(void);
int  gap_getrandom(void *buf, unsigned long n);
#endif
