#ifndef K_KTIME_H
#define K_KTIME_H

#include "types.h"

void rtc_init(void);
u64  time_now_sec(void);                /* Unix time, UTC */
u64  time_now_ms(void);
void civil_from_epoch(u64 t, int *year, int *mon, int *day, int *hour, int *min, int *sec, int *wday);

/* In-memory copy of the kernel log (what /dev/kmsg and dmesg show). */
size_t klog_read(u64 off, void *buf, size_t n);
u64    klog_total(void);

#endif
