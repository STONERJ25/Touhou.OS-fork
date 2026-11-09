/*
 * Starting other programs from a dynamically linked program (L2 of docs/COMPAT.md): fork and execve of another dynamic program (the kernel loads its interpreter again, in the
 * root of the parent), posix_spawn (clone with CLONE_VM | CLONE_VFORK), system(), popen() and a pipeline through the shell. The programs it starts are dyn_hello (next to this
 * program) and the shell and tools of the Linux root (/bin/sh, tr). Every line is a fact the manual pages promise, so a real Linux prints the same.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void status(const char *what, int st) {
    if (WIFEXITED(st)) printf("%s: exited with %d\n", what, WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) printf("%s: killed by signal %d\n", what, WTERMSIG(st));
    else printf("%s: status %#x\n", what, st);
}

int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IOLBF, 0);                   /* a terminal and a pipe must give the same order of lines between this program's and its children's */
    char dir[4096], hello[4200];
    strncpy(dir, argv[0], sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    snprintf(hello, sizeof hello, "%s/dyn_hello", dirname(dir));
    fflush(stdout);

    pid_t p = fork();
    if (p == 0) {
        char *args[] = { hello, "from", "fork", NULL };
        execv(hello, args);
        printf("execv failed: %s\n", strerror(errno));
        _exit(99);
    }
    int st = 0;
    waitpid(p, &st, 0);
    status("fork and exec of a dynamic program", st);

    pid_t sp;
    char *sargs[] = { hello, "from", "posix_spawn", NULL };
    int rc = posix_spawn(&sp, hello, NULL, NULL, sargs, environ);
    printf("posix_spawn returned %d\n", rc);
    waitpid(sp, &st, 0);
    status("posix_spawn of a dynamic program", st);

    fflush(stdout);
    st = system("echo from system; exit 5");
    status("system()", st);

    FILE *f = popen("echo piped through the shell | tr a-z A-Z", "r");
    char line[200];
    while (f && fgets(line, sizeof line, f)) printf("popen read: %s", line);
    if (f) status("popen", pclose(f));

    fflush(stdout);
    st = system("echo one two three | wc -w | tr -d ' ' ; test -d /proc/self && echo proc is there");
    status("a pipeline in the shell", st);
    return 0;
}
