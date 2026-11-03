#ifndef K_KTIME_H
#define K_KTIME_H

#include "types.h"

void rtc_init(void);
u64  time_now_sec(void);                /* Unix time, UTC */
u64  time_now_ms(void);
void time_set_ms(u64 ms);               /* set the kernel's wall clock (the clock chip is untouched) */
u64  rtc_read_raw(void);                /* the clock chip's calendar fields, read as if they were UTC */
void rtc_write_raw(u64 epoch);          /* store the calendar fields of epoch in the clock chip */
void civil_from_epoch(u64 t, int *year, int *mon, int *day, int *hour, int *min, int *sec, int *wday);

/* In-memory copy of the kernel log (what /dev/kmsg and dmesg show). */
size_t klog_read(u64 off, void *buf, size_t n);
size_t klog_read_clamped(u64 *off, void *buf, size_t n);    /* moves *off up to the oldest byte still in the ring */
u64    klog_total(void);

#endif
