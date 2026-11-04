/*
 * stubs.c: placeholders for the parts of the engine that are not written yet (each one is replaced by its real
 * module as it lands; this file shrinks to nothing).
 */
#include "jsint.h"

#define STUB_INIT(name) int name(JSContext *ctx) { (void)ctx; return 0; }
STUB_INIT(kz_init_intl)
