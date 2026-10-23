/*
 * Doors into Patchouli's library: less and more start the pager, man starts the manual viewer.
 * The real programs live in user/bin/patchouli-pager and user/bin/patchouli; these applets only exist so that the
 * familiar names work (applet names cannot contain a dash, and build.py only links applets to kappabox).
 */
#include "kappabox.h"

static int hand_over(const char *program, const char *as, int argc, char **argv) {
    char **av = calloc((size_t)argc + 1, sizeof *av);
    if (!av) kb_die("out of memory");
    av[0] = (char *)as;
    for (int i = 1; i < argc; i++) av[i] = argv[i];
    execv(program, av);
    kb_error("cannot start %s: %s", program, strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

APPLET(less) { return hand_over("/bin/patchouli-pager", "less", argc, argv); }
APPLET(more) { return hand_over("/bin/patchouli-pager", "more", argc, argv); }
APPLET(man) { return hand_over("/bin/patchouli", "patchouli", argc, argv); }
