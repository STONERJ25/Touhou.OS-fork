/*
 * ssh_config (the file format ssh_config(5) describes) and the -o options: "Keyword value" or "Keyword=value", `Host` and `Match` sections, `Include`.
 *
 * As in OpenSSH, the first value obtained for an option wins: the command line is read first, then the user's file, then the system's, and within a file the
 * earlier lines come before the later. A `Host` line starts a section whose lines apply only when the name the user typed matches its patterns (* and ?, and a
 * leading ! to exclude); a `Match` line does the same with conditions (all, host, originalhost, user, localuser). IdentityFile, LocalForward, RemoteForward,
 * DynamicForward and SendEnv collect every value.
 *
 * Options OpenSSH has that make no difference here (compression, agent socket, cipher lists, control sockets ...) are accepted and ignored, so that a config file
 * shared with other machines does not stop working; an option that would change what is trusted or where the connection goes and cannot be honoured
 * (ProxyCommand) is an error, not a silent no-op.
 */
#include <sshc.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct { const char *name; SshcOpt id; bool list; } KEYWORDS[] = {
    { "hostname", SSHC_O_HOSTNAME, false }, { "user", SSHC_O_USER, false }, { "port", SSHC_O_PORT, false },
    { "identityfile", SSHC_O_IDENTITYFILE, true }, { "identitiesonly", SSHC_O_IDENTITIESONLY, false },
    { "stricthostkeychecking", SSHC_O_STRICT, false }, { "userknownhostsfile", SSHC_O_USERKNOWNHOSTS, false },
    { "globalknownhostsfile", SSHC_O_GLOBALKNOWNHOSTS, false }, { "connecttimeout", SSHC_O_CONNECTTIMEOUT, false },
    { "serveraliveinterval", SSHC_O_SERVERALIVEINTERVAL, false }, { "serveralivecountmax", SSHC_O_SERVERALIVECOUNTMAX, false },
    { "batchmode", SSHC_O_BATCHMODE, false }, { "passwordauthentication", SSHC_O_PASSWORDAUTH, false },
    { "pubkeyauthentication", SSHC_O_PUBKEYAUTH, false }, { "kbdinteractiveauthentication", SSHC_O_KBDAUTH, false },
    { "challengeresponseauthentication", SSHC_O_KBDAUTH, false }, { "numberofpasswordprompts", SSHC_O_NUMPROMPTS, false },
    { "hashknownhosts", SSHC_O_HASHKNOWNHOSTS, false }, { "requesttty", SSHC_O_REQUESTTTY, false }, { "loglevel", SSHC_O_LOGLEVEL, false },
    { "escapechar", SSHC_O_ESCAPECHAR, false }, { "hostkeyalias", SSHC_O_HOSTKEYALIAS, false }, { "rekeylimit", SSHC_O_REKEYLIMIT, false },
    { "localforward", SSHC_O_LOCALFORWARD, true }, { "remoteforward", SSHC_O_REMOTEFORWARD, true }, { "dynamicforward", SSHC_O_DYNAMICFORWARD, true },
    { "exitonforwardfailure", SSHC_O_EXITONFORWARDFAILURE, false }, { "proxyjump", SSHC_O_PROXYJUMP, false },
    { "preferredauthentications", SSHC_O_PREFERREDAUTH, false }, { "addressfamily", SSHC_O_ADDRESSFAMILY, false },
    { "connectionattempts", SSHC_O_CONNECTIONATTEMPTS, false }, { "forwardagent", SSHC_O_FORWARDAGENT, false }, { "forwardx11", SSHC_O_FORWARDX11, false },
    { "sendenv", SSHC_O_SENDENV, true },
};

static const char *const IGNORED[] = {
    "compression", "compressionlevel", "tcpkeepalive", "ipqos", "controlmaster", "controlpath", "controlpersist", "addkeystoagent", "usekeychain",
    "hostkeyalgorithms", "kexalgorithms", "ciphers", "macs", "pubkeyacceptedalgorithms", "pubkeyacceptedkeytypes", "updatehostkeys", "visualhostkey",
    "verifyhostkeydns", "checkhostip", "canonicalizehostname", "canonicalizedomains", "canonicalizemaxdots", "canonicalizefallbacklocal", "forwardx11trusted",
    "xauthlocation", "gssapiauthentication", "gssapidelegatecredentials", "hostbasedauthentication", "permitlocalcommand", "localcommand", "setenv",
    "streamlocalbindunlink", "identityagent", "nohostauthenticationforlocalhost", "tunnel", "tunneldevice", "clearallforwardings", "gatewayports",
    "fingerprinthash", "logverbose", "syslogfacility", "revokedhostkeys", "protocol", "enableescapecommandline", "requiredrsasize", "obscurekeystroketiming",
    "bindaddress", "bindinterface", "hostbasedacceptedalgorithms", "ignoreunknown", "pkcs11provider", "securitykeyprovider", "proxyusefdpass",
};

void sshc_cfg_free(SshcConfig *cfg) {
    for (int i = 0; i < SSHC_O_COUNT; i++) {
        for (int k = 0; k < cfg->n[i]; k++) free(cfg->vals[i][k]);
        free(cfg->vals[i]);
    }
    memset(cfg, 0, sizeof *cfg);
}

int sshc_cfg_set(SshcConfig *cfg, const char *keyword, const char *value, char *err, size_t errcap) {
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (strcasecmp(keyword, KEYWORDS[i].name) != 0) continue;
        SshcOpt id = KEYWORDS[i].id;
        if (!KEYWORDS[i].list && cfg->n[id] > 0) return 0;                  /* the first value wins */
        char **nv = realloc(cfg->vals[id], (size_t)(cfg->n[id] + 1) * sizeof *nv);
        char *dup = strdup(value);
        if (!nv || !dup) { free(dup); if (err) snprintf(err, errcap, "out of memory"); return -1; }
        cfg->vals[id] = nv;
        nv[cfg->n[id]++] = dup;
        return 0;
    }
    for (size_t i = 0; i < sizeof IGNORED / sizeof IGNORED[0]; i++)
        if (!strcasecmp(keyword, IGNORED[i])) { sshc_debug(2, "ssh_config: %s is accepted and ignored here", keyword); return 0; }
    if (!strcasecmp(keyword, "proxycommand")) {
        if (err) snprintf(err, errcap, "ProxyCommand is not supported");
        return -1;
    }
    if (err) snprintf(err, errcap, "Bad configuration option: %s", keyword);
    return -1;
}

/* Splits "Keyword value", "Keyword=value" or "Keyword = value" at the first blank or '='. *value points into `line` (which is modified). */
static bool split_keyword(char *line, char **keyword, char **value) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '\r' || *line == '\n') return false;
    *keyword = line;
    while (*line && *line != ' ' && *line != '\t' && *line != '=' && *line != '\r' && *line != '\n') line++;
    if (*line) *line++ = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '=') { line++; while (*line == ' ' || *line == '\t') line++; }
    *value = line;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
    return true;
}

/* A value with one pair of double quotes around all of it loses them. */
static char *unquote(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"' && !memchr(v + 1, '"', n - 2)) { v[n - 1] = 0; return v + 1; }
    return v;
}

int sshc_cfg_set_line(SshcConfig *cfg, const char *text, char *err, size_t errcap) {
    char line[1024], *k, *v;
    sshc_cpy(line, sizeof line, text);
    if (!split_keyword(line, &k, &v) || !*v) {
        if (err) snprintf(err, errcap, "missing argument");
        return -1;
    }
    return sshc_cfg_set(cfg, k, unquote(v), err, errcap);
}

int sshc_words(const char *s, char *buf, size_t cap, char **out, int max) {
    int n = 0;
    size_t o = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        out[n++] = buf + o;
        bool in_q = false;
        while (*s && (in_q || (*s != ' ' && *s != '\t'))) {
            if (*s == '"') { in_q = !in_q; s++; continue; }
            if (o + 1 < cap) buf[o++] = *s;
            s++;
        }
        if (o < cap) buf[o++] = 0;
    }
    return n;
}

/* Does the name match a list of patterns, where !pattern excludes? */
static bool host_matches(const char *patterns, const char *host) {
    char buf[512], *w[32];
    int n = sshc_words(patterns, buf, sizeof buf, w, 32);
    bool hit = false;
    for (int i = 0; i < n; i++) {
        bool neg = w[i][0] == '!';
        if (sshc_pattern_match(w[i] + (neg ? 1 : 0), host)) {
            if (neg) return false;
            hit = true;
        }
    }
    return hit;
}

static bool match_conditions(const char *args, const char *host, const char *user, const SshcConfig *cfg) {
    char buf[512], *w[32];
    int n = sshc_words(args, buf, sizeof buf, w, 32);
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(w[i], "all")) continue;
        if (!strcasecmp(w[i], "canonical") || !strcasecmp(w[i], "final") || !strcasecmp(w[i], "exec") || !strcasecmp(w[i], "tagged")) {
            if (!strcasecmp(w[i], "exec") || !strcasecmp(w[i], "tagged")) i++;
            return false;                                               /* names are not canonicalised here, and no command is run to decide */
        }
        if (i + 1 >= n) return false;
        const char *crit = w[i], *arg = w[i + 1];
        i++;
        const char *subject = NULL;
        if (!strcasecmp(crit, "host") || !strcasecmp(crit, "originalhost")) subject = host;
        else if (!strcasecmp(crit, "user")) subject = sshc_cfg_str(cfg, SSHC_O_USER) ? sshc_cfg_str(cfg, SSHC_O_USER) : user;
        else if (!strcasecmp(crit, "localuser")) subject = sshc_local_user();
        else return false;
        if (!subject || !host_matches(arg, subject)) return false;
    }
    return true;
}

static int load_file(SshcConfig *cfg, const char *path, const char *host, const char *user, bool must_exist, char *err, size_t errcap, int depth) {
    const char *why = NULL;
    size_t len;
    char *text = sshc_read_file(path, 1u << 20, &len, false, &why);
    if (!text) {
        if (!must_exist && why && !strcmp(why, "no such file")) return 0;
        if (err) snprintf(err, errcap, "%s: %s", path, why ? why : "cannot be read");
        return -1;
    }
    bool active = true;
    int lineno = 0, rc = 0;
    for (char *line = text; *line && rc == 0;) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        lineno++;
        char *k, *v;
        if (split_keyword(line, &k, &v)) {
            if (!*v) {                                                       /* a keyword without a value is a mistake wherever it stands */
                if (err) snprintf(err, errcap, "%s: line %d: missing argument", path, lineno);
                rc = -1;
            } else if (!strcasecmp(k, "host")) active = host_matches(unquote(v), host);
            else if (!strcasecmp(k, "match")) active = match_conditions(v, host, user, cfg);
            else if (active && !strcasecmp(k, "include")) {
                char buf[512], *w[8];
                int n = sshc_words(v, buf, sizeof buf, w, 8);
                for (int i = 0; i < n && rc == 0 && depth < 4; i++) {
                    char full[512], expanded[512];
                    if (!sshc_expand_path(w[i], expanded, sizeof expanded, host, user, 22)) continue;
                    if (expanded[0] == '/') sshc_cpy(full, sizeof full, expanded);
                    else {                                                 /* relative: to the directory of the file that includes it */
                        sshc_cpy(full, sizeof full, path);
                        char *slash = strrchr(full, '/');
                        size_t keep = slash ? (size_t)(slash - full + 1) : 0;
                        full[keep] = 0;
                        strncat(full, expanded, sizeof full - keep - 1);
                    }
                    rc = load_file(cfg, full, host, user, false, err, errcap, depth + 1);
                }
            } else if (active) {
                if (!*v) { if (err) snprintf(err, errcap, "%s: line %d: missing argument", path, lineno); rc = -1; }
                else {
                    char e2[200];
                    if (sshc_cfg_set(cfg, k, unquote(v), e2, sizeof e2) != 0) {
                        if (err) snprintf(err, errcap, "%s: line %d: %s", path, lineno, e2);
                        rc = -1;
                    }
                }
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    memset(text, 0, len);
    free(text);
    return rc;
}

int sshc_cfg_load(SshcConfig *cfg, const char *path, const char *host, const char *user, bool must_exist, char *err, size_t errcap) {
    return load_file(cfg, path, host, user, must_exist, err, errcap, 0);
}

const char *sshc_cfg_str(const SshcConfig *cfg, SshcOpt o) { return cfg->n[o] > 0 ? cfg->vals[o][0] : NULL; }

int sshc_cfg_bool(const SshcConfig *cfg, SshcOpt o, int dflt) {
    const char *v = sshc_cfg_str(cfg, o);
    if (!v) return dflt;
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on")) return 1;
    if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off")) return 0;
    return dflt;
}

long sshc_cfg_int(const SshcConfig *cfg, SshcOpt o, long dflt) {
    const char *v = sshc_cfg_str(cfg, o);
    if (!v) return dflt;
    char *end;
    long n = strtol(v, &end, 10);
    return end == v ? dflt : n;
}

long sshc_parse_time(const char *s) {
    if (!s || !*s) return -1;
    long total = 0;
    while (*s) {
        char *end;
        long n = strtol(s, &end, 10);
        if (end == s || n < 0) return -1;
        long unit = 1;
        switch (tolower((unsigned char)*end)) {
        case 0: case 's': unit = 1; break;
        case 'm': unit = 60; break;
        case 'h': unit = 3600; break;
        case 'd': unit = 86400; break;
        case 'w': unit = 604800; break;
        default: return -1;
        }
        total += n * unit;
        s = *end ? end + 1 : end;
    }
    return total;
}

long long sshc_parse_size(const char *s) {
    if (!s || !*s) return -1;
    char *end;
    long long n = strtoll(s, &end, 10);
    if (end == s || n < 0) return -1;
    switch (tolower((unsigned char)*end)) {
    case 0: return n;
    case 'k': n <<= 10; break;
    case 'm': n <<= 20; break;
    case 'g': n <<= 30; break;
    default: return -1;
    }
    return end[1] ? -1 : n;
}
