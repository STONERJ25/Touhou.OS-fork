/*
 * Memory type range registers: what the firmware set, a side-effect-free decision where one more write-combining range could
 * go, and the update sequence the Intel SDM prescribes for changing a variable range. The framebuffer code (gfx/fbtune.c) is
 * the only user: it falls back to an MTRR when the page-table route (PAT) leaves the screen slow.
 */
#ifndef K_MTRR_H
#define K_MTRR_H

#include "types.h"

#define MTRR_UC        0
#define MTRR_WC        1
#define MTRR_WT        4
#define MTRR_WP        5
#define MTRR_WB        6
#define MTRR_UNDEFINED (-2)         /* mtrr_type_at: overlapping ranges whose combination the SDM leaves undefined */

#define MTRR_MAX_VAR   16

typedef struct {
    bool present;                   /* CPUID.1:EDX[12] */
    bool enabled;                   /* IA32_MTRR_DEF_TYPE.E: with it clear the whole address space is uncached */
    bool wc_ok;                     /* IA32_MTRRCAP.WC: a variable range may be write-combining */
    u32  vcnt;                      /* variable ranges the CPU has (at most MTRR_MAX_VAR are looked at) */
    u8   def_type;                  /* the type of everything no variable range covers */
    u64  phys_mask;                 /* the physical address bits the CPU implements */
    u64  base[MTRR_MAX_VAR];        /* IA32_MTRR_PHYSBASEn: address | type */
    u64  mask[MTRR_MAX_VAR];        /* IA32_MTRR_PHYSMASKn: mask | bit 11 (valid) */
} MtrrState;

/* mtrr_plan_wc: the range cannot get a write-combining MTRR, or does not need one */
#define MTRR_PLAN_REFUSED  (-1)
#define MTRR_PLAN_ALREADY  (-2)

void        mtrr_read(MtrrState *s);                                    /* from the MSRs */
int         mtrr_type_at(const MtrrState *s, u64 addr);                 /* MTRR_UC.. or MTRR_UNDEFINED: variable ranges, else the default type */
const char *mtrr_type_name(int type);
u64         mtrr_range_mask(const MtrrState *s, u64 size);              /* the PHYSMASK value of a naturally aligned power-of-two range, valid bit set */
size_t      mtrr_describe(const MtrrState *s, char *buf, size_t cap);   /* "default UC; #0 0x0 +2 GiB WB; ..." for the log */

/* Where would a write-combining range for [base, base + size) go? Returns the variable range (slot) number to use, or
 * MTRR_PLAN_REFUSED / MTRR_PLAN_ALREADY with a reason in *why. Reads nothing and writes nothing: a pure function of `s`, so it
 * can be tested against made-up firmware layouts. A WC range may only be added where no other valid range overlaps: UC always
 * wins over it (so it would do nothing), and the SDM leaves WB/WT/WP overlapping WC undefined. */
int         mtrr_plan_wc(const MtrrState *s, u64 base, u64 size, const char **why);

/* Programs variable range `slot` (base register value, mask register value) with the SDM's update sequence: interrupts off, cache
 * disabled and flushed, ranges off while the pair is written, flushed again. Writing mask 0 clears the range. Returns false if
 * the CPU did not keep the values (read back and compared); the caller restores the old ones. */
bool        mtrr_write_var(u32 slot, u64 base, u64 mask);

#endif
