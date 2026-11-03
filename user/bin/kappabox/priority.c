/* nice and renice: scheduling priorities (docs/JOBCONTROL.md). */
#include "kappabox.h"
#include <sys/resource.h>

static int parse_int(const char *s, int *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno) return -1;
    *out = (int)v;
    return 0;
}

/* nice [-n N | -N | --N] [command [argument...]] */
APPLET(nice) {
    int inc = 10, i = 1;                            /* the default increment is 10, as in every nice */
    if (i < argc && !strcmp(argv[i], "-n")) {
        if (i + 1 >= argc || parse_int(argv[i + 1], &inc)) { kb_error("usage: nice [-n INCREMENT] [command [argument...]]"); return 125; }
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] == '-' && isdigit((unsigned char)argv[i][2])) {
        if (parse_int(argv[i] + 1, &inc)) { kb_error("invalid increment '%s'", argv[i]); return 125; }     /* --10 is -10 */
        i++;
    } else if (i < argc && argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
        if (parse_int(argv[i] + 1, &inc)) { kb_error("invalid increment '%s'", argv[i]); return 125; }     /* -10 is +10 */
        i++;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) {
        kb_error("invalid option '%s'", argv[i]);
        return 125;
    } else if (i < argc && !strcmp(argv[i], "--")) {
        i++;
        inc = 10;
    }
    if (i >= argc) {                                /* without a command: show the current niceness */
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, 0);
        if (cur == -1 && errno) { kb_error("cannot get niceness: %s", strerror(errno)); return 125; }
        printf("%d\n", cur);
        return 0;
    }
    if (nice(inc) == -1 && errno) kb_error("cannot set niceness: %s", strerror(errno));      /* a warning: the command still runs */
    execvp(argv[i], &argv[i]);
    kb_error("cannot run '%s': %s", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

/* renice [-n] PRIORITY [-p] PID... [-g] PGRP... [-u] USER...  (PRIORITY is the new nice value, not an increment) */
APPLET(renice) {
    int i = 1, prio;
    if (i < argc && !strcmp(argv[i], "-n")) i++;
    if (i >= argc || parse_int(argv[i], &prio)) { kb_error("usage: renice [-n] PRIORITY [-g|-p|-u] ID..."); return 1; }
    i++;
    if (i >= argc) { kb_error("usage: renice [-n] PRIORITY [-g|-p|-u] ID..."); return 1; }
    int which = PRIO_PROCESS, rc = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { which = PRIO_PROCESS; continue; }
        if (!strcmp(argv[i], "-g")) { which = PRIO_PGRP; continue; }
        if (!strcmp(argv[i], "-u")) { which = PRIO_USER; continue; }
        int id;
        if (parse_int(argv[i], &id)) {
            int uid = which == PRIO_USER ? kb_user_id(argv[i]) : -1;
            if (uid < 0) { kb_error("bad %s '%s'", which == PRIO_USER ? "user" : "ID", argv[i]); rc = 1; continue; }
            id = uid;
        }
        const char *what = which == PRIO_PROCESS ? "process ID" : which == PRIO_PGRP ? "process group ID" : "user ID";
        errno = 0;
        int old = getpriority(which, (id_t)id);
        if (old == -1 && errno) { kb_error("failed to get priority for %d (%s): %s", id, what, strerror(errno)); rc = 1; continue; }
        if (setpriority(which, (id_t)id, prio) < 0) {
            kb_error("failed to set priority for %d (%s): %s", id, what, strerror(errno));
            rc = 1;
            continue;
        }
        int now = getpriority(which, (id_t)id);
        printf("%d (%s) old priority %d, new priority %d\n", id, what, old, now);
    }
    return rc;
}
