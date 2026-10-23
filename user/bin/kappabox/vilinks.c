/*
 * vim and view start the vi editor (user/bin/vi). The program looks at argv[0], so `view` opens read-only.
 */
#include "kappabox.h"

static int start_vi(int argc, char **argv) {
    char **av = calloc((size_t)argc + 1, sizeof *av);
    if (!av) kb_die("out of memory");
    for (int i = 0; i < argc; i++) av[i] = argv[i];
    execv("/bin/vi", av);
    kb_error("cannot start /bin/vi: %s", strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

APPLET(vim) { return start_vi(argc, argv); }
APPLET(view) { return start_vi(argc, argv); }
