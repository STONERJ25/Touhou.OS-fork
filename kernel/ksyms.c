/* Lookup in the symbol table the build embeds after linking (tools/build.py writes build/gen/ksyms.bin). */
#include "arch.h"
#include "kernel.h"

typedef struct PACKED {
    u32 count;
    u32 reserved;
} KsymHeader;

typedef struct PACKED {
    u64 addr;
    u32 name_off;
    u32 pad;
} KsymEntry;

extern const char ksyms_start[];
extern char __text_start[], __text_end[];

const char *ksym_lookup(u64 addr, u64 *offset) {
    const KsymHeader *h = (const KsymHeader *)ksyms_start;
    if (!h->count || addr < (u64)__text_start || addr >= (u64)__text_end) return NULL;
    const KsymEntry *e = (const KsymEntry *)(h + 1);
    const char *names = (const char *)(e + h->count);
    if (addr < e[0].addr) return NULL;
    u32 lo = 0, hi = h->count;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (e[mid].addr <= addr) lo = mid;
        else hi = mid;
    }
    if (offset) *offset = addr - e[lo].addr;
    return names + e[lo].name_off;
}
