#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
#include <abi.h>
#include <syscall.h>
typedef void (*sighandler_t)(int);
typedef unsigned int sigset_t;
#define SIG_ERR ((sighandler_t)-1)
#undef SIG_DFL
#undef SIG_IGN
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    sigset_t sa_mask;
};
#define SA_RESTART 0x10000000
/* SA_NOCLDSTOP (from abi.h): no SIGCHLD when a child stops or continues */
sighandler_t signal(int sig, sighandler_t h);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);       /* the mask belongs to the calling thread */
int sigsuspend(const sigset_t *mask);
int sigwait(const sigset_t *set, int *sig);
struct timespec;
int sigtimedwait(const sigset_t *set, void *info, const struct timespec *timeout);
int sigpending(sigset_t *set);
int sigemptyset(sigset_t *s);
int sigfillset(sigset_t *s);
int sigaddset(sigset_t *s, int sig);
int sigdelset(sigset_t *s, int sig);
int kill(pid_t pid, int sig);
int raise(int sig);
#endif
