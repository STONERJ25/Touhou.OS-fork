/* linux: runs a program built for Linux (docs/COMPAT.md). The kernel answers that program's system calls with the Linux personality. */
#include "kappabox.h"
#include <gensokyo.h>

/* linux PROGRAM [ARGUMENT...]   (PROGRAM is a path: no search through the path variable, as with a program started by ./) */
APPLET(linux) {
    if (argc < 2) { kb_error("usage: linux PROGRAM [ARGUMENT...]"); return 2; }
    if (gap_setabi(ABI_LINUX) < 0) { kb_error("cannot select the Linux personality: %s", strerror(errno)); return 1; }
    execv(argv[1], &argv[1]);
    kb_error("cannot run '%s': %s", argv[1], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

/* native PROGRAM [ARGUMENT...]: the other way round, for a native program started from inside a Linux one (a Linux process keeps its personality across exec). */
APPLET(native) {
    if (argc < 2) { kb_error("usage: native PROGRAM [ARGUMENT...]"); return 2; }
    if (gap_setabi(ABI_NATIVE) < 0) { kb_error("cannot select the native personality: %s", strerror(errno)); return 1; }
    execvp(argv[1], &argv[1]);
    kb_error("cannot run '%s': %s", argv[1], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
