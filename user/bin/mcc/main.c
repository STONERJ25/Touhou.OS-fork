/* mcc -- Marisa's compiler, mld -- her linker, mar -- her library card index.  One program, three names:
 * the personality is chosen from argv[0] (or from --mld / --mar as the first argument).  See docs/MCC.md.
 */
#include "mcc.h"
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "util.inc"
#include "ld.inc"

#ifndef MCC_DEFAULT_ROOT
#define MCC_DEFAULT_ROOT "/ScarletDevilMansion"
#endif

static const char *mcc_root(void) {
    const char *r = getenv("MCC_ROOT");
    return r && *r ? r : MCC_DEFAULT_ROOT;
}

/* ---- mld */

static int mld_main(int argc, char **argv) {
    const char *out = "a.out", *entry = "_start";
    bool strip = false, map = false;
    char **files = xcalloc((size_t)(argc + 1) * sizeof(char *));
    int nfiles = 0;
    char **ldirs = xcalloc((size_t)(argc + 2) * sizeof(char *));
    int nldirs = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (str_eq(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (str_starts(a, "-o") && a[2]) out = a + 2;
        else if (str_eq(a, "-e") && i + 1 < argc) entry = argv[++i];
        else if (str_eq(a, "-s") || str_eq(a, "--strip-all")) strip = true;
        else if (str_eq(a, "-M") || str_eq(a, "--map")) map = true;
        else if (str_eq(a, "-L") && i + 1 < argc) ldirs[nldirs++] = argv[++i];
        else if (str_starts(a, "-L")) ldirs[nldirs++] = a + 2;
        else if (str_starts(a, "-l")) {
            char *name = (a[2] || i + 1 >= argc) ? a + 2 : argv[++i];
            char *found = NULL;
            ldirs[nldirs] = join_path(mcc_root(), "lib");
            for (int d = 0; d <= nldirs && !found; d++) {
                char *cand = xformat("%s/lib%s.a", ldirs[d], name);
                if (file_exists(cand)) found = cand;
            }
            if (!found) fatal("mld: cannot find -l%s -- Marisa: the shelf is empty, ze", name);
            files[nfiles++] = found;
        } else if (str_eq(a, "-static") || str_eq(a, "-nostdlib") || str_eq(a, "--gc-sections") || str_eq(a, "-n") || str_eq(a, "-N")) {
        } else if ((str_eq(a, "-z") || str_eq(a, "-m") || str_eq(a, "-T")) && i + 1 < argc) {
            i++;
        } else if (str_starts(a, "-z") || str_starts(a, "-m") || str_starts(a, "--")) {
        } else if (a[0] == '-' && a[1]) {
            fatal("mld: unknown option '%s'", a);
        } else {
            files[nfiles++] = a;
        }
    }
    if (!nfiles) fatal("mld: no input files");
    return ld_link_files(files, nfiles, out, strip, map, entry);
}

int main(int argc, char **argv) {
    const char *self = base_of(argv[0]);
    if (str_starts(self, "mld") || (argc > 1 && str_eq(argv[1], "--mld"))) {
        if (argc > 1 && str_eq(argv[1], "--mld")) {
            argv[1] = argv[0];
            return mld_main(argc - 1, argv + 1);
        }
        return mld_main(argc, argv);
    }
    return mld_main(argc, argv);
}
