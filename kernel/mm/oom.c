/* Out-of-memory handling (docs/HARDENING.md). */
#include "kernel.h"

static u64 g_oom_kills;
u64 oom_kill_count(void) { return g_oom_kills; }
