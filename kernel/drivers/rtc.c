/* CMOS real-time clock (Sakuya's pocket watch) and the wall clock derived from it. */
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "vfs.h"

static u64 g_boot_epoch;

static u8 cmos(u8 reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static u8 bcd(u8 v) { return (u8)((v & 0x0F) + (v >> 4) * 10); }

static s64 days_from_civil(s64 y, unsigned m, unsigned d) {
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

void civil_from_epoch(u64 t, int *year, int *mon, int *day, int *hour, int *min, int *sec, int *wday) {
    s64 days = (s64)(t / 86400);
    u64 rem = t % 86400;
    *hour = (int)(rem / 3600);
    *min = (int)(rem % 3600 / 60);
    *sec = (int)(rem % 60);
    *wday = (int)((days + 4) % 7);          /* 1970-01-01 was a Thursday */
    s64 z = days + 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    s64 y = (s64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = (int)(y + (m <= 2));
    *mon = (int)m;
    *day = (int)d;
}

static u64 read_rtc_epoch(void) {
    u8 sec, min, hour, day, mon, year, century = 0, b;
    for (int tries = 0; tries < 5; tries++) {
        for (int i = 0; i < 1000000 && (cmos(0x0A) & 0x80); i++) { }
        sec = cmos(0x00); min = cmos(0x02); hour = cmos(0x04);
        day = cmos(0x07); mon = cmos(0x08); year = cmos(0x09); century = cmos(0x32);
        b = cmos(0x0B);
        if (sec == cmos(0x00) && min == cmos(0x02) && hour == cmos(0x04)) break;   /* stable across an update */
    }
    bool pm = hour & 0x80;
    hour &= 0x7F;
    if (!(b & 4)) {                             /* BCD */
        sec = bcd(sec); min = bcd(min); hour = bcd(hour); day = bcd(day); mon = bcd(mon); year = bcd(year);
        if (century) century = bcd(century);
    }
    if (!(b & 2)) {                             /* 12-hour clock */
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }
    int full = (century >= 19 && century <= 21 ? century : 20) * 100 + year;
    return (u64)(days_from_civil(full, mon, day) * 86400 + hour * 3600 + min * 60 + sec);
}

void rtc_init(void) {
    g_boot_epoch = read_rtc_epoch() - uptime_ms() / 1000;
    int y, mo, d, h, mi, s, wd;
    civil_from_epoch(time_now_sec(), &y, &mo, &d, &h, &mi, &s, &wd);
    klog("Sakuya", "wall clock %04d-%02d-%02d %02d:%02d:%02d UTC (time is only stopped when she says so)", y, mo, d, h, mi, s);
}

u64 time_now_sec(void) {
    return g_boot_epoch + uptime_ms() / 1000;
}

u64 time_now_ms(void) {
    return g_boot_epoch * 1000 + uptime_ms();
}
