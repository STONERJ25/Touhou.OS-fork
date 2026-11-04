/* Test helper for tests/host/test_dmi.py: the size of DmiInfo (the Python side hands the C code a buffer of that size) and a way to
 * set what the kernel's dmi.c copies from BootInfo after the walk (the kind of entry point and its flags). */
#include "dmi.h"

size_t dmi_info_size(void) { return sizeof(DmiInfo); }

void dmi_info_entry(DmiInfo *d, unsigned char kind, unsigned char flags) {
    d->kind = kind;
    d->flags = flags;
}
