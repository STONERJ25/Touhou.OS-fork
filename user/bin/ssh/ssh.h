/*
 * ssh: the SSH client (docs/SSH_CLIENT.md).
 *
 *   main.c      the command line, ssh_config, logging in
 *   session.c   the session channel: the terminal in raw mode, the escape character, window changes, the exit status, the poll loop
 *   forward.c   port forwarding: -L, -D (direct-tcpip), -R (tcpip-forward)
 *
 * The protocol itself (transport, authentication, channels, known_hosts, key files, ssh_config) is in the library, user/lib/sshcrypto/src/sshc_*.c, and is the same
 * code ssh-keygen, ssh-keyscan, scp and sftp use.
 */
#ifndef SSH_H
#define SSH_H

#include <poll.h>
#include <sshc.h>

typedef struct {
    bool pty;                                   /* ask for a pseudo-terminal */
    bool force_pty;                             /* -tt: ask even when standard input is not a terminal */
    bool no_stdin;                              /* -n: standard input is not read; the remote side sees end of file */
    bool subsystem;                             /* -s: `command` names a subsystem */
    bool no_session;                            /* -N: no command at all, only the forwardings */
    const char *command;                        /* NULL: the login shell */
    int escape;                                 /* the escape character (default '~'); -1: none */
    const char *host;                           /* for messages */
    int alive_interval_s, alive_count_max;      /* ServerAliveInterval / ServerAliveCountMax; interval 0: never ask */
    bool exit_on_forward_failure;
    int nlocal, nremote, ndynamic;
    const char *const *local, *const *remote, *const *dynamic;     /* the specifications, as given: "[bind:]port:host:hostport", "[bind:]port" */
} SessionOpts;

/* Runs the session until it ends; returns ssh's exit status (the remote command's, or 255). */
int session_run(SshcConn *c, const SessionOpts *o);

/* forward.c: the forwardings of the session. forwards_init() sets them up (opening listeners, asking the server for -R ports); forwards_fds() adds the descriptors
 * the poll loop must watch; forwards_service() acts on what poll reported. forwards_list() prints them for the ~# escape. */
typedef struct Forwards Forwards;
Forwards *forwards_new(SshcConn *c, const SessionOpts *o);
int  forwards_start(Forwards *f, SshcConn *c, const SessionOpts *o);           /* 0, or -1 when ExitOnForwardFailure applies and one failed */
int  forwards_fds(Forwards *f, struct pollfd *fds, int max);                   /* fills fds (and remembers which is which); returns how many */
void forwards_service(Forwards *f, SshcConn *c, const struct pollfd *fds, int n);
void forwards_list(Forwards *f, char *out, size_t cap);
bool forwards_busy(const Forwards *f);                                          /* is a forwarded connection open */
void forwards_free(Forwards *f);

#endif
