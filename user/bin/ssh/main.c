/*
 * ssh [-46NnqsTtVv] [-D [bind:]port] [-e char] [-F file] [-i file] [-L [bind:]port:host:hostport] [-l user] [-o option] [-p port]
 *     [-R [bind:]port:host:hostport] [user@]host [command [argument ...]]
 *
 * The command line and the config files say the same things in the same words, so every option here becomes an ssh_config keyword (-p 2222 is "Port 2222") and
 * the first value for a keyword wins: the command line is read first, then ~/.ssh/config (or -F file), then /etc/ssh/ssh_config.
 *
 * Exit status: the remote command's; 255 when ssh itself failed (nothing to connect to, host key refused, login refused, connection lost).
 * Not supported, and said so rather than ignored when asked for: agent and X11 forwarding (-A -X -Y), jump hosts and proxy commands (-J, ProxyCommand), -f,
 * IPv6 (-6), multiplexing.
 */
#include "ssh.h"
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr,
            "usage: ssh [-4NnqsTtVv] [-D [bind_address:]port] [-e escape_char] [-F configfile] [-i identity_file]\n"
            "           [-L [bind_address:]port:host:hostport] [-l login_name] [-o option] [-p port]\n"
            "           [-R [bind_address:]port:host:hostport] [user@]hostname [command [argument ...]]\n");
    exit(255);
}

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("ssh: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(255);
}

static void set_or_die(SshcConfig *cfg, const char *key, const char *val) {
    char err[200];
    if (sshc_cfg_set(cfg, key, val, err, sizeof err) != 0) die("%s", err);
}

/* "^A" is control-A, "none" switches the escape off, one character is itself. */
static int parse_escape(const char *s) {
    if (!strcasecmp(s, "none")) return -1;
    if (s[0] == '^' && s[1] && !s[2]) return s[1] == '?' ? 127 : (s[1] & 0x1f);
    if (s[0] && !s[1]) return (unsigned char)s[0];
    die("Bad escape character '%s'.", s);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    SshcConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    SessionOpts so;
    memset(&so, 0, sizeof so);
    so.escape = '~';
    int verbose = 0, tty_opt = 0;
    bool quiet = false, escape_set = false;
    const char *config_file = NULL, *host_arg = NULL;
    int i = 1;

    static const char ARGOPTS[] = "BbcDEeFIiJLlmOopQRSWw";
    while (i < argc) {
        char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--")) { i++; if (!host_arg && i < argc) host_arg = argv[i++]; break; }
            for (char *p = a + 1; *p; p++) {
                char opt = *p;
                if (strchr(ARGOPTS, opt)) {
                    const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!val) { fprintf(stderr, "ssh: option requires an argument -- %c\n", opt); usage(); }
                    switch (opt) {
                    case 'p': set_or_die(&cfg, "Port", val); break;
                    case 'l': set_or_die(&cfg, "User", val); break;
                    case 'i': set_or_die(&cfg, "IdentityFile", val); break;
                    case 'L': set_or_die(&cfg, "LocalForward", val); break;
                    case 'R': set_or_die(&cfg, "RemoteForward", val); break;
                    case 'D': set_or_die(&cfg, "DynamicForward", val); break;
                    case 'F': config_file = val; break;
                    case 'e': so.escape = parse_escape(val); escape_set = true; break;
                    case 'o': {
                        char err[200];
                        if (sshc_cfg_set_line(&cfg, val, err, sizeof err) != 0) die("command-line: %s", err);
                        break;
                    }
                    case 'J': die("jump hosts (-J) are not supported");
                    case 'c': case 'm': case 'b': case 'B': case 'I': case 'Q': case 'E': case 'S': case 'O': case 'w': case 'W':
                        die("option -%c is not supported by this client", opt);
                    default: usage();
                    }
                    break;
                }
                switch (opt) {
                case 'v': if (verbose < 3) verbose++; break;
                case 'q': quiet = true; break;
                case 'N': so.no_session = true; break;
                case 'n': so.no_stdin = true; break;
                case 's': so.subsystem = true; break;
                case 'T': tty_opt = -1; break;
                case 't': if (tty_opt < 2) tty_opt = tty_opt < 0 ? 1 : tty_opt + 1; break;
                case 'V': printf("TouhouOS_ssh_1 (SSH-2.0: curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com)\n"); return 0;
                case '4': set_or_die(&cfg, "AddressFamily", "inet"); break;
                case '6': die("IPv6 is not available on this system");
                case 'A': case 'X': case 'Y': die("agent and X11 forwarding (-%c) are not supported", opt);
                case 'a': case 'x': case 'k': case 'K': case 'C': case 'g': case 'y': case 'G': break;      /* off already, or without meaning here */
                case 'f': die("-f (going to the background) is not supported");
                case 'M': die("connection multiplexing (-M) is not supported");
                default: fprintf(stderr, "ssh: unknown option -- %c\n", opt); usage();
                }
            }
            i++;
        } else if (!host_arg) {
            host_arg = a;
            i++;
        } else {
            break;
        }
    }
    if (!host_arg) usage();

    /* the remote command: the rest of the arguments, joined with blanks as OpenSSH does (the remote shell reads them as one line) */
    char command[4096];
    command[0] = 0;
    for (int k = i; k < argc; k++) {
        if (strlen(command) + strlen(argv[k]) + 2 >= sizeof command) die("the command is too long");
        if (k > i) strcat(command, " ");
        strcat(command, argv[k]);
    }
    so.command = i < argc ? command : NULL;
    if (so.no_session && so.command) die("-N and a command do not go together");

    /* [user@]host, or ssh://[user@]host[:port] */
    char hostbuf[256];
    snprintf(hostbuf, sizeof hostbuf, "%s", host_arg);
    char *host = hostbuf;
    if (!strncmp(host, "ssh://", 6)) {
        host += 6;
        char *slash = strchr(host, '/');
        if (slash) *slash = 0;
        char *colon = strrchr(host, ':');
        if (colon && !strchr(colon, '@')) { *colon++ = 0; set_or_die(&cfg, "Port", colon); }
    }
    char *at = strrchr(host, '@');
    if (at) {
        *at = 0;
        if (!*host) die("empty user name in '%s'", host_arg);
        set_or_die(&cfg, "User", host);
        host = at + 1;
    }
    if (!*host) usage();
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    so.host = host;

    /* the config files, and what the options come to */
    char err[300];
    if (sshc_load_configs(&cfg, config_file, host, err, sizeof err) != 0) die("%s", err);
    SshcSetup setup;
    if (sshc_login_setup(&setup, &cfg, host, quiet ? -1 : verbose) != 0) die("%s", setup.err);
    SshcLogin lg = setup.lg;
    if (sshc_cfg_bool(&cfg, SSHC_O_FORWARDAGENT, 0) || sshc_cfg_bool(&cfg, SSHC_O_FORWARDX11, 0))
        fprintf(stderr, "ssh: ForwardAgent and ForwardX11 are not supported and stay off\n");

    /* terminal or not */
    const char *rt = sshc_cfg_str(&cfg, SSHC_O_REQUESTTTY);
    if (tty_opt == 0 && rt) tty_opt = !strcasecmp(rt, "no") ? -1 : !strcasecmp(rt, "force") ? 2 : !strcasecmp(rt, "yes") ? 1 : 0;
    bool stdin_tty = isatty(STDIN_FILENO);
    if (so.subsystem || so.no_session) so.pty = false;
    else if (tty_opt < 0) so.pty = false;
    else if (so.command) so.pty = tty_opt > 0;
    else so.pty = tty_opt > 0 || stdin_tty;
    if (so.pty && !stdin_tty && tty_opt < 2) {
        if (sshc_verbose >= 0) fprintf(stderr, "Pseudo-terminal will not be allocated because stdin is not a terminal.\n");
        so.pty = false;
    }
    if (!escape_set && sshc_cfg_str(&cfg, SSHC_O_ESCAPECHAR)) so.escape = parse_escape(sshc_cfg_str(&cfg, SSHC_O_ESCAPECHAR));
    if (so.command && so.subsystem == false && !*so.command) so.command = NULL;
    so.alive_interval_s = (int)sshc_cfg_int(&cfg, SSHC_O_SERVERALIVEINTERVAL, 0);
    so.alive_count_max = (int)sshc_cfg_int(&cfg, SSHC_O_SERVERALIVECOUNTMAX, 3);
    so.exit_on_forward_failure = sshc_cfg_bool(&cfg, SSHC_O_EXITONFORWARDFAILURE, 0) != 0;
    so.nlocal = cfg.n[SSHC_O_LOCALFORWARD];
    so.local = (const char *const *)cfg.vals[SSHC_O_LOCALFORWARD];
    so.nremote = cfg.n[SSHC_O_REMOTEFORWARD];
    so.remote = (const char *const *)cfg.vals[SSHC_O_REMOTEFORWARD];
    so.ndynamic = cfg.n[SSHC_O_DYNAMICFORWARD];
    so.dynamic = (const char *const *)cfg.vals[SSHC_O_DYNAMICFORWARD];
    if (so.no_session && !(so.nlocal + so.nremote + so.ndynamic) && sshc_verbose >= 0) fprintf(stderr, "Warning: -N with nothing to forward\n");

    char why[400];
    SshcConn *c = sshc_login(&lg, why, sizeof why);
    if (!c) {
        fprintf(stderr, "%s\n", why);
        return 255;
    }
    int status = session_run(c, &so);
    sshc_conn_free(c);
    sshc_cfg_free(&cfg);
    return status;
}
