/* Lockup detection (docs/HARDENING.md). */
#include "kernel.h"

static u64 g_reports;
u64 watchdog_report_count(void) { return g_reports; }
