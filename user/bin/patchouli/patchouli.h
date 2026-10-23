/*
 * patchouli: the manual viewer ("man" is an alias). Pages are .pw files (see docs/TOOLS.md and pw(5)) under
 * /ScarletDevilMansion/share/patchouli/<section>/<name>.pw, or under the directories listed in $PATCHOULI_PATH.
 *
 *   index.c   finds pages and builds the index used by -l, -k and lookups
 *   pw.c      parses the page format, renders it for a terminal or as plain text, and checks pages
 *   main.c    command line, pager hand-off
 */
#ifndef PATCHOULI_H
#define PATCHOULI_H

#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "../ptlib/pt_term.h"

#define DEFAULT_ROOT "/ScarletDevilMansion/share/patchouli"

/* ---- index ---- */

typedef struct {
    char *name;
    char *sec;
    char *desc;                 /* one-line description (for an alias: "alias for NAME(SEC)") */
    char *path;
    char *target;               /* alias entries: the page they point at */
    int is_alias;
} Entry;

typedef struct { Entry *e; int n, cap; char **roots; int nroots; } Index;

void   index_load(Index *ix);
Entry *index_find(Index *ix, const char *section, const char *name);
int    index_cmp_name(const void *a, const void *b);
char  *read_file(const char *path, size_t *len);        /* malloc'd, NUL terminated; NULL on error */

/* ---- pages ---- */

enum { BK_HEADING, BK_SUBHEADING, BK_PARA, BK_SYN, BK_VERB, BK_ITEM, BK_BULLET };

typedef struct {
    int kind;
    int in_item;                /* part of the description of an item */
    int indent;                 /* BK_VERB: extra indentation */
    int line;                   /* source line, for messages */
    char *text;                 /* heading / paragraph / term text, markup unparsed */
    char **lines;               /* BK_VERB */
    int nlines;
} Block;

typedef struct { int line; char *msg; } PageError;

typedef struct {
    char *name, *sec, *desc;
    char **aliases;
    int naliases;
    Block *b;
    int nb, cap;
    PageError *err;
    int nerr, errcap;
    int header_line;
} Page;

void pw_parse(Page *pg, const char *src, size_t len);
void pw_free(Page *pg);
void pw_render(Page *pg, PtBuf *out, int width, int ansi);
int  pw_check(Page *pg, const char *path, const char *dir_section, const char *file_base, Index *ix, FILE *report);

#endif
