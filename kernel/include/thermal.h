/*
 * CPU temperature, frequency and package power from the processor's own registers (docs/SENSORS.md).
 *
 * kernel/arch/x86_64/thermal.c reads them (guarded: msrsafe.h) and shows them as /proc/thermal; thermal_core.c is the part that needs no
 * machine -- decoding a register value, the units, the test hook's table -- so that tests/host/test_thermal.py compiles it with gcc and
 * checks it against numbers worked out by hand from the Intel SDM (volume 3 chapter 14.7 and 15.10, volume 4: the MSR tables).
 */
#ifndef K_THERMAL_H
#define K_THERMAL_H

#include "types.h"

#define MSR_IA32_PERF_STATUS        0x198
#define MSR_IA32_THERM_STATUS       0x19C
#define MSR_TEMPERATURE_TARGET      0x1A2
#define MSR_IA32_PKG_THERM_STATUS   0x1B1
#define MSR_RAPL_POWER_UNIT         0x606
#define MSR_PKG_ENERGY_STATUS       0x611

/* IA32_THERM_STATUS and IA32_PACKAGE_THERM_STATUS: the same layout. The status bits are the even ones (0, 2, 4, ...), each followed by a
 * "log" bit that stays set until software clears it. */
typedef struct {
    bool valid;                 /* bit 31: the digital readout is valid */
    int  readout;               /* bits 22:16: degrees Celsius BELOW the temperature at which the processor throttles itself */
    int  resolution;            /* bits 30:27: degrees per step of the readout (1 on every part seen so far) */
    u32  status;                /* THERM_* bits that are set now */
    u32  log;                   /* THERM_* bits that were set since the log was last cleared */
} ThermalStatus;

enum {
    THERM_HOT       = 1u << 0,  /* thermal status: the sensor is at the throttling temperature */
    THERM_PROCHOT   = 1u << 1,  /* PROCHOT# or FORCEPR# asserted */
    THERM_CRITICAL  = 1u << 2,  /* the critical temperature: the processor is about to shut down */
    THERM_THRESHOLD1 = 1u << 3,
    THERM_THRESHOLD2 = 1u << 4,
    THERM_POWER     = 1u << 5,  /* power limitation */
    THERM_CURRENT   = 1u << 6,  /* current limitation */
    THERM_CROSS     = 1u << 7,  /* cross-domain limitation */
};

void   thermal_decode_status(u64 msr, ThermalStatus *out);
int    thermal_decode_tjmax(u64 msr_1a2, int *offset);        /* TjMax in degrees (bits 23:16); *offset gets the programmable offset (bits 29:24) when given */
size_t thermal_flag_names(u32 bits, char *buf, size_t cap);   /* "hot prochot", or "-" for none */
int    thermal_bus_khz(const char *vendor, u32 family, u32 model);   /* the reference clock the multiplier counts, 0 when this model is not known */
bool   thermal_rapl_units(u64 msr_606, int *esu);             /* energy unit 1/2^esu joule; false when the field is not credible */
u64    thermal_energy_uj(u32 raw, int esu);                   /* microjoules of a raw energy count */
u64    thermal_power_mw(u32 raw_from, u32 raw_to, int esu, u64 interval_ms);   /* the counter is 32 bits and wraps: the difference is taken modulo 2^32 */

/* The test hook: thermal.fake=MSR:VALUE[+STEP],MSR:VALUE,... (hexadecimal). The CPU is replaced by a table; a register that is not in it
 * behaves as if the CPU raised #GP. Every read of an entry with a STEP returns VALUE plus STEP times the number of earlier reads. */
typedef struct { u32 msr; u64 val, step; u64 reads; } ThermalFakeEntry;
typedef struct { ThermalFakeEntry e[16]; int n; } ThermalFake;
bool thermal_fake_parse(const char *spec, ThermalFake *out);
bool thermal_fake_read(ThermalFake *t, u32 msr, u64 *val);    /* false: not in the table */

#endif
