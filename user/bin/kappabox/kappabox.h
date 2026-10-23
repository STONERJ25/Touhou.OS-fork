/*
 * kappabox: Nitori's toolbox, one binary with many applets (BusyBox style).
 * An applet is any function declared with APPLET(name) in a .c file next to this header; tools/build.py finds them,
 * writes the dispatch table and installs a /bin/<name> symlink to kappabox for each one.
 */
#ifndef KAPPABOX_H
#define KAPPABOX_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <gensokyo.h>

#define APPLET(name) int applet_##name(int argc, char **argv)

/* the applet currently running (argv[0] basename), for error messages */
extern const char *kb_name;

void  kb_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  kb_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
const char *kb_user_name(unsigned uid);
const char *kb_group_name(unsigned gid);
int   kb_user_id(const char *name);               /* -1 if unknown */
int   kb_group_id(const char *name);
char *kb_join(const char *dir, const char *name);  /* malloc'd "dir/name" */
int   kb_term_cols(void);
int   kb_copy_fd(int in, int out);
int   kb_copy_file(const char *src, const char *dst, int preserve_mode);
int   kb_remove_tree(const char *path, int force, int verbose);
char *kb_read_all(int fd, size_t *len);           /* malloc'd, NUL terminated */
char *kb_format_size(unsigned long long bytes, char *buf, size_t n);   /* "1.4M" */
void  kb_mode_string(unsigned mode, char *out);   /* "drwxr-xr-x" */
extern int kb_color;                               /* stdout is a terminal */

/* Runs fn on each file named in argv[first..] (stdin when there are none or for "-"). Returns 1 if any file failed. */
int   kb_each_input(int argc, char **argv, int first, int (*fn)(FILE *f, const char *name, void *ctx), void *ctx);
/* Reads every line of f into a malloc'd array of malloc'd strings (newline stripped); returns the count. */
size_t kb_read_lines(FILE *f, char ***out);
void  kb_free_lines(char **lines, size_t n);
/* Parses "1,3-5,7-" style lists; returns the number of ranges stored in lo[]/hi[] (hi = INT_MAX for open ends). */
int   kb_parse_ranges(const char *spec, int *lo, int *hi, int max);
int   kb_in_ranges(int pos, const int *lo, const int *hi, int n);

#endif
