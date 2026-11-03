#ifndef _GAP_THERMAL_H
#define _GAP_THERMAL_H
/*
 * CPU temperature, frequency and package power as the kernel reports them in /proc/thermal (docs/SENSORS.md). A panel or desktop widget needs
 * nothing but this call; the `sensors` command is built on it.
 */

struct gap_thermal {
    int  present;                       /* the CPU has a digital thermal sensor; when 0, `reason` says why not and the rest is zero */
    int  synthetic;                     /* the readings come from the test hook thermal.fake=, not from the CPU */
    int  tjmax;                         /* the temperature at which the CPU throttles itself, degrees Celsius; 0 when not known */
    int  core_valid, core_temp, core_margin;     /* core 0: degrees Celsius, and degrees below tjmax; core_valid is 0 when the sensor says its reading is not valid */
    int  pkg_valid, pkg_temp, pkg_margin;        /* the package as a whole; pkg_valid is 0 without a package sensor */
    char core_status[64], pkg_status[64];        /* "-", or the names of the limits that are active now ("hot prochot") */
    int  ratio;                         /* the multiplier the CPU runs at; 0 when not known */
    int  mhz;                           /* the multiplier times the reference clock; 0 when not known */
    long power_mw;                      /* package power over the kernel's last measuring window, milliwatts; -1 when not known */
    long power_window_ms;               /* how long that window was */
    char reason[160];                   /* when !present */
};

int gap_thermal_read(struct gap_thermal *t);     /* 0, or -1 with errno set when there is no /proc/thermal at all */

#endif
