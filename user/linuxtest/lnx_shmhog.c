/* Maps shared anonymous memory until the kernel ends it. A test of this system, not a comparison with a real Linux (tools/linux-oracle.py never runs it): the Linux personality
 * gives a MAP_SHARED | MAP_ANONYMOUS mapping all its pages at the call (lnx_mmap in kernel/linux/l3.c, for multiprocessing's shared values and the like), so the pages are taken from the
 * page allocator at once. They have to be allocated as memory for a user process: out of reach of the kernel's reserve, and with the OOM killer behind a shortage (docs/HARDENING.md).
 * tests/case_hardening2.py runs this in a guest with 128 MiB: the program must be ended by SIGKILL (status 137), and its memory must come back. */
#include "lnx.h"

#define PROT_READ_WRITE 3
#define MAP_SHARED_ANON 0x21                         /* MAP_SHARED | MAP_ANONYMOUS */

void lnx_main(long *sp) {
    (void)sp;
    lnx_puts("shmhog: starts\n");
    long mib = 0;
    for (;;) {
        long p = lnx_sys6(L_mmap, 0, 1L << 20, PROT_READ_WRITE, MAP_SHARED_ANON, -1, 0);
        if (p >= 0 || p < -4095) {                   /* an address, not an errno: the megabyte is mapped (and every page of it is there) */
            mib++;
            if (mib % 16 == 0) { lnx_puts("shmhog: "); lnx_putl(mib); lnx_puts(" MiB\n"); }
        }
        /* an errno (ENOMEM): the shortage has been met; go on asking, as a program that wants all it can get does: the kernel's answer decides */
    }
}
