/* mcc -- Marisa's compiler, mld -- her linker, mar -- her library card index.  One program, three names:
 * the personality is chosen from argv[0] (or from --mld / --mar as the first argument).  See docs/MCC.md.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "mcc.h"
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "util.inc"
#include "type.inc"
#include "lex.inc"
#include "pp.inc"
#include "parse.inc"
#include "expr.inc"
#include "x86.inc"
#include "gen.inc"
#include "asm.inc"
#include "obj.inc"
#include "ld.inc"
#include "ar.inc"

#ifndef MCC_DEFAULT_ROOT
#define MCC_DEFAULT_ROOT "/ScarletDevilMansion"
#endif

static const char *mcc_root(void) {
    const char *r = getenv("MCC_ROOT");
    return r && *r ? r : MCC_DEFAULT_ROOT;
}

/* the argument of an option written either as "-Idir" or as "-I dir" */
static char *opt_arg(int argc, char **argv, int *i, int prefix_len) {
    char *a = argv[*i];
    if (a[prefix_len]) return a + prefix_len;
    if (*i + 1 >= argc) fatal("option '%s' needs an argument", a);
    return argv[++*i];
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
        else if (str_starts(a, "-L")) ldirs[nldirs++] = opt_arg(argc, argv, &i, 2);
        else if (str_starts(a, "-l")) {
            char *name = opt_arg(argc, argv, &i, 2);
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

/* ---- mcc */

typedef struct {
    char *name;
    char *value; /* NULL: -U */
} MacroOpt;

static MacroOpt *macro_opts;
static int n_macro_opts;
static char **include_files;
static int n_include_files;
static char **user_inc_dirs, **sys_inc_dirs;
static int n_user_inc, n_sys_inc;
static bool opt_nostdinc;

/* Forget everything the previous translation unit left behind. */
static void reset_frontend(void) {
    memset(&macros, 0, sizeof macros);
    memset(&pragma_once, 0, sizeof pragma_once);
    memset(&include_guards, 0, sizeof include_guards);
    cond_incl = NULL;
    memset(&global_scope, 0, sizeof global_scope);
    scope = &global_scope;
    locals = globals = NULL;
    gotos = labels = NULL;
    current_fn = NULL;
    current_switch = NULL;
    cur_brk_label = cur_cont_label = 0;
    in_param_decl = false;
    reset_output();
    nfixups = 0;
    depth = 0;
}

static void setup_includes(void) {
    n_include_paths = 0;
    for (int i = 0; i < n_user_inc; i++) pp_add_include_path(user_inc_dirs[i]);
    for (int i = 0; i < n_sys_inc; i++) pp_add_include_path(sys_inc_dirs[i]);
    if (!opt_nostdinc) {
        pp_add_include_path(join_path(mcc_root(), "include"));
        pp_add_include_path(join_path(mcc_root(), "lib/mcc/include"));
    }
}

static Token *front_end(const char *path, bool for_pp_only) {
    reset_frontend();
    setup_includes();
    init_macros();
    for (int i = 0; i < n_macro_opts; i++) {
        if (macro_opts[i].value) define_macro(macro_opts[i].name, macro_opts[i].value);
        else undef_macro(macro_opts[i].name);
    }
    pp_join_strings = !for_pp_only;
    Token *tok = tokenize_file((char *)path);
    if (!tok) fatal("cannot open '%s': %s -- Marisa: no such book, ze", path, strerror(errno));
    /* -include files are read before the source */
    for (int i = n_include_files - 1; i >= 0; i--) {
        Token *inc = tokenize_file(include_files[i]);
        if (!inc) fatal("cannot open '%s': %s", include_files[i], strerror(errno));
        tok = append(inc, tok);
    }
    return preprocess(tok);
}

static void usage(void) {
    printf("usage: mcc [-c] [-E] [-o out] [-I dir] [-D name[=value]] [-U name] [-l lib] [-L dir] file...\n");
    printf("  -c           compile to an object file, do not link\n");
    printf("  -E           preprocess only, write the tokens to stdout\n");
    printf("  -o FILE      name of the output\n");
    printf("  -I DIR       add a directory to the header search path (-isystem DIR: after those)\n");
    printf("  -D N[=V]     define a macro;  -U N undefines one;  -include FILE reads FILE first\n");
    printf("  -l NAME, -L DIR   link with libNAME.a (libgap is always linked)\n");
    printf("  -nostdinc    no default header directories;  -nostdlib  no crt0.o and no libgap\n");
    printf("  -s           strip the symbol table of the executable;  -M  print a link map\n");
    printf("  -fsyntax-only  parse and check, produce nothing;  -v / --version\n");
    printf("mcc compiles a C11 subset (no VLAs, no complex numbers) to static x86-64 TouhouOS programs.\n");
    printf("It is also mld (the linker) and mar (the archiver) when called by those names.\n");
}

static char *output_name_for(const char *src, const char *ext) {
    const char *b = base_of(src);
    const char *dot = strrchr(b, '.');
    size_t n = dot ? (size_t)(dot - b) : strlen(b);
    return xformat("%.*s%s", (int)n, b, ext);
}

static int mcc_main(int argc, char **argv) {
    bool opt_c = false, opt_E = false, opt_S = false, opt_syntax = false, opt_nostdlib = false, opt_nostartfiles = false;
    bool opt_strip = false, opt_map = false;
    char *out = NULL;
    char **inputs = xcalloc((size_t)(argc + 1) * sizeof(char *));
    int ninputs = 0;
    char **libdirs = xcalloc((size_t)(argc + 2) * sizeof(char *));
    int nlibdirs = 0;
    char **libs = xcalloc((size_t)(argc + 1) * sizeof(char *));
    int nlibs = 0;
    user_inc_dirs = xcalloc((size_t)(argc + 1) * sizeof(char *));
    sys_inc_dirs = xcalloc((size_t)(argc + 1) * sizeof(char *));
    macro_opts = xcalloc((size_t)(argc + 1) * sizeof(MacroOpt));
    include_files = xcalloc((size_t)(argc + 1) * sizeof(char *));

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (str_eq(a, "-c")) opt_c = true;
        else if (str_eq(a, "-E")) opt_E = true;
        else if (str_eq(a, "-S")) opt_S = true;
        else if (str_eq(a, "-fsyntax-only")) opt_syntax = true;
        else if (str_eq(a, "-o") || (str_starts(a, "-o") && a[2])) out = opt_arg(argc, argv, &i, 2);
        else if (str_starts(a, "-I")) user_inc_dirs[n_user_inc++] = opt_arg(argc, argv, &i, 2);
        else if (str_eq(a, "-isystem") && i + 1 < argc) sys_inc_dirs[n_sys_inc++] = argv[++i];
        else if (str_eq(a, "-include") && i + 1 < argc) include_files[n_include_files++] = argv[++i];
        else if (str_starts(a, "-D")) {
            char *d = opt_arg(argc, argv, &i, 2);
            char *eq = strchr(d, '=');
            macro_opts[n_macro_opts].name = eq ? xstrndup(d, (size_t)(eq - d)) : d;
            macro_opts[n_macro_opts].value = eq ? eq + 1 : (char *)"1";
            n_macro_opts++;
        } else if (str_starts(a, "-U")) {
            macro_opts[n_macro_opts].name = opt_arg(argc, argv, &i, 2);
            macro_opts[n_macro_opts].value = NULL;
            n_macro_opts++;
        } else if (str_starts(a, "-L")) {
            libdirs[nlibdirs++] = opt_arg(argc, argv, &i, 2);
        } else if (str_starts(a, "-l")) {
            libs[nlibs++] = opt_arg(argc, argv, &i, 2);
        } else if (str_eq(a, "-nostdinc")) {
            opt_nostdinc = true;
        } else if (str_eq(a, "-nostdlib")) {
            opt_nostdlib = true;
        } else if (str_eq(a, "-nostartfiles")) {
            opt_nostartfiles = true;
        } else if (str_eq(a, "-s")) {
            opt_strip = true;
        } else if (str_eq(a, "-M") || str_eq(a, "-Wl,-M")) {
            opt_map = true;
        } else if (str_eq(a, "-v") || str_eq(a, "--version") || str_eq(a, "-V")) {
            printf("mcc %s -- Marisa Kirisame's compiler, a C11 subset for x86-64 TouhouOS (ze!)\n", MCC_VERSION);
            return 0;
        } else if (str_eq(a, "--help") || str_eq(a, "-h")) {
            usage();
            return 0;
        } else if (str_eq(a, "-x") && i + 1 < argc) {
            i++;
        } else if (str_eq(a, "-shared")) {
            fatal("-shared is not supported: TouhouOS has no dynamic linking");
        } else if (a[0] == '-' && a[1]) {
            /* -O2, -g, -Wall, -std=c11, -static, -pthread, -m64, -fPIC, -fno-... are accepted and ignored */
        } else {
            inputs[ninputs++] = a;
        }
    }

    if (opt_S) fatal("-S is not supported: mcc writes machine code directly (use -c, then an object dumper)");
    if (!ninputs) fatal("no input files");
    if (out && ninputs > 1 && (opt_c || opt_E)) fatal("cannot specify -o with -c or -E and multiple input files");

    LdInput *objs = xcalloc((size_t)(ninputs + 1) * sizeof(LdInput));
    int nobjs = 0;

    for (int i = 0; i < ninputs; i++) {
        char *in = inputs[i];
        bool is_c = str_ends(in, ".c") || str_ends(in, ".i") || str_eq(in, "-");
        if (!is_c) {
            if (opt_c || opt_E || opt_syntax) fatal("%s: not a C source file", in);
            objs[nobjs].path = in;
            objs[nobjs].kind = 0;
            nobjs++;
            continue;
        }

        if (opt_E) {
            Token *tok = front_end(in, true);
            FILE *fp = stdout;
            if (out) {
                fp = fopen(out, "w");
                if (!fp) fatal("cannot write '%s': %s", out, strerror(errno));
            }
            print_tokens(fp, tok);
            if (out) fclose(fp);
            continue;
        }

        Token *tok = front_end(in, false);
        Obj *prog = parse(tok);
        if (opt_syntax) continue;
        codegen_program(prog);
        Buf obj = write_object(prog, in);

        if (opt_c) {
            char *dst = out ? out : output_name_for(in, ".o");
            if (write_whole_file(dst, obj.p, obj.len, false) != 0) fatal("cannot write '%s': %s", dst, strerror(errno));
            continue;
        }

        u8 *keep = xmalloc(obj.len + 16);
        memcpy(keep, obj.p, obj.len);
        objs[nobjs].path = output_name_for(in, ".o");
        objs[nobjs].kind = 1;
        objs[nobjs].data = keep;
        objs[nobjs].len = obj.len;
        nobjs++;
    }

    if (opt_c || opt_E || opt_syntax) return 0;

    /* link: crt0.o, the objects, the -l libraries, libgap */
    LdInput *all = xcalloc((size_t)(nobjs + nlibs + 8) * sizeof(LdInput));
    int nall = 0;
    char *libroot = join_path(mcc_root(), "lib");
    if (!opt_nostdlib && !opt_nostartfiles) {
        all[nall].path = join_path(libroot, "crt0.o");
        all[nall].kind = 0;
        nall++;
    }
    for (int i = 0; i < nobjs; i++) all[nall++] = objs[i];
    for (int i = 0; i < nlibs; i++) {
        char *found = NULL;
        for (int d = 0; d <= nlibdirs && !found; d++) {
            char *cand = xformat("%s/lib%s.a", d < nlibdirs ? libdirs[d] : libroot, libs[i]);
            if (file_exists(cand)) found = cand;
        }
        if (!found) {
            /* the libraries that live inside libgap */
            if (str_eq(libs[i], "m") || str_eq(libs[i], "c") || str_eq(libs[i], "pthread") || str_eq(libs[i], "gap") || str_eq(libs[i], "rt") ||
                str_eq(libs[i], "dl"))
                continue;
            fatal("cannot find -l%s -- Marisa: the shelf is empty, ze", libs[i]);
        }
        all[nall].path = found;
        all[nall].kind = 0;
        nall++;
    }
    if (!opt_nostdlib) {
        all[nall].path = join_path(libroot, "libgap.a");
        all[nall].kind = 0;
        nall++;
    }
    char *dst = out ? out : (char *)"a.out";
    return ld_link_mixed(all, nall, dst, opt_strip, opt_map, "_start");
}

static int real_main(int argc, char **argv) {
    const char *self = base_of(argv[0]);
    if (argc > 1 && str_eq(argv[1], "--mld")) {
        argv[1] = argv[0];
        return mld_main(argc - 1, argv + 1);
    }
    if (argc > 1 && str_eq(argv[1], "--mar")) {
        argv[1] = argv[0];
        return mar_main(argc - 1, argv + 1);
    }
    if (str_starts(self, "mld")) return mld_main(argc, argv);
    if (str_starts(self, "mar")) return mar_main(argc, argv);
    return mcc_main(argc, argv);
}

/* The compiler is recursive (a chain of 4000 additions is a tree 4000 levels deep), and the main stack of a TouhouOS
 * program is 8 MiB: the work is done in a thread with a big, lazily mapped stack. */
typedef struct {
    int argc;
    char **argv;
    int rc;
} MainArgs;

static void *big_stack_main(void *p) {
    MainArgs *a = p;
    a->rc = real_main(a->argc, a->argv);
    return NULL;
}

int main(int argc, char **argv) {
    MainArgs a;
    a.argc = argc;
    a.argv = argv;
    a.rc = 0;
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, (size_t)256 << 20) == 0 && pthread_create(&th, &attr, big_stack_main, &a) == 0) {
        pthread_join(th, NULL);
        return a.rc;
    }
    return real_main(argc, argv);
}
