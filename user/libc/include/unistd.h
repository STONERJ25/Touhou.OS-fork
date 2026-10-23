#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>
#include <abi.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern char **environ;

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup(int fd);
int     dup2(int old, int new_);
int     pipe(int fds[2]);
pid_t   fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
int     execl(const char *path, const char *arg, ...);
int     execlp(const char *file, const char *arg, ...);
pid_t   getpid(void);
pid_t   getppid(void);
pid_t   getpgid(pid_t pid);
pid_t   getpgrp(void);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   setsid(void);
int     tcsetpgrp(int fd, pid_t pgid);
pid_t   tcgetpgrp(int fd);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t n);
int     unlink(const char *path);
int     rmdir(const char *path);
int     access(const char *path, int mode);
int     isatty(int fd);
int     link(const char *a, const char *b);
int     symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t n);
int     truncate(const char *path, off_t len);
int     ftruncate(int fd, off_t len);
int     chown(const char *path, uid_t u, gid_t g);
unsigned sleep(unsigned sec);
int     usleep(useconds_t us);
unsigned alarm(unsigned sec);
int     pause(void);
void   *sbrk(long inc);
int     brk(void *addr);
int     sync(void);
int     gethostname(char *buf, size_t n);
long    sysconf(int name);
char   *ttyname(int fd);
__attribute__((noreturn)) void _exit(int status);

#define _SC_PAGESIZE 30
#define _SC_NPROCESSORS_ONLN 84
#define _SC_CLK_TCK 2

extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *opts);

#endif
