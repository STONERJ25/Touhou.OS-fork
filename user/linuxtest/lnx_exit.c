/* Exits with the status given as the first argument (default 42): the status word the parent sees must be status << 8 (exit_group, wait4). */
#include "lnx.h"

void lnx_main(long *sp) {
    int code = 42;
    if (sp[0] > 1) {
        const char *a = (const char *)sp[2];
        code = 0;
        while (*a >= '0' && *a <= '9') code = code * 10 + (*a++ - '0');
    }
    lnx_exit(code);
}
