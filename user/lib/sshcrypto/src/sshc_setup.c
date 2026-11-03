/*
 * From options to a connection description, once, for every client program: where the config files are, which names the options go by, what the defaults are.
 * Defaults are OpenSSH's: port 22, the local user's name, StrictHostKeyChecking ask, ~/.ssh/known_hosts then /etc/ssh/ssh_known_hosts, the identity
 * ~/.ssh/id_ed25519 (the only key type this client has), public key then keyboard-interactive then password, three password tries.
 */
#include <sshc.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int sshc_load_configs(SshcConfig *cfg, const char *config_file, const char *host, char *err, size_t errcap) {
    const char *user = sshc_local_user();
    if (config_file) {
        if (!strcmp(config_file, "none") || !strcmp(config_file, "/dev/null")) return 0;
        return sshc_cfg_load(cfg, config_file, host, user, true, err, errcap);
    }
    char path[512];
    if (sshc_expand_path("~/.ssh/config", path, sizeof path, host, NULL, 22) && sshc_cfg_load(cfg, path, host, user, false, err, errcap) != 0) return -1;
    return sshc_cfg_load(cfg, "/etc/ssh/ssh_config", host, user, false, err, errcap);
}

char *sshc_split_userhost(char *s, const char **user) {
    *user = NULL;
    char *at = strrchr(s, '@');
    if (!at) return s;
    *at = 0;
    *user = s;
    return at + 1;
}

static int parse_loglevel(const char *s) {
    if (!strcasecmp(s, "QUIET") || !strcasecmp(s, "FATAL") || !strcasecmp(s, "ERROR")) return -1;
    if (!strcasecmp(s, "INFO")) return 0;
    if (!strcasecmp(s, "VERBOSE") || !strcasecmp(s, "DEBUG") || !strcasecmp(s, "DEBUG1")) return 1;
    if (!strcasecmp(s, "DEBUG2")) return 2;
    if (!strcasecmp(s, "DEBUG3")) return 3;
    return 99;
}

int sshc_login_setup(SshcSetup *s, const SshcConfig *cfg, const char *host, int cli_verbose) {
    memset(s, 0, sizeof *s);
    SshcLogin *lg = &s->lg;

    int level = cli_verbose;
    if (!level && sshc_cfg_str(cfg, SSHC_O_LOGLEVEL)) {
        level = parse_loglevel(sshc_cfg_str(cfg, SSHC_O_LOGLEVEL));
        if (level == 99) { snprintf(s->err, sizeof s->err, "Bad LogLevel '%s'", sshc_cfg_str(cfg, SSHC_O_LOGLEVEL)); return -1; }
    }
    sshc_verbose = level;
    sshc_batch = sshc_cfg_bool(cfg, SSHC_O_BATCHMODE, 0) != 0;

    const char *pj = sshc_cfg_str(cfg, SSHC_O_PROXYJUMP);
    if (pj && strcasecmp(pj, "none")) { snprintf(s->err, sizeof s->err, "ProxyJump is not supported"); return -1; }
    const char *af = sshc_cfg_str(cfg, SSHC_O_ADDRESSFAMILY);
    if (af && !strcasecmp(af, "inet6")) { snprintf(s->err, sizeof s->err, "IPv6 is not available on this system"); return -1; }

    const char *hostname = sshc_cfg_str(cfg, SSHC_O_HOSTNAME);
    if (hostname) {
        if (!sshc_expand_path(hostname, s->hostname, sizeof s->hostname, host, NULL, 22)) { snprintf(s->err, sizeof s->err, "bad HostName '%s'", hostname); return -1; }
    } else {
        sshc_cpy(s->hostname, sizeof s->hostname, host);
    }
    const char *user = sshc_cfg_str(cfg, SSHC_O_USER);
    if (!user) user = sshc_local_user();
    long port = sshc_cfg_int(cfg, SSHC_O_PORT, 22);
    if (port < 1 || port > 65535) { snprintf(s->err, sizeof s->err, "Bad port '%s'", sshc_cfg_str(cfg, SSHC_O_PORT)); return -1; }
    const char *alias = sshc_cfg_str(cfg, SSHC_O_HOSTKEYALIAS);

    lg->host = s->hostname;
    lg->name = alias ? alias : s->hostname;
    lg->port = (int)port;
    lg->user = user;
    long ct = sshc_cfg_str(cfg, SSHC_O_CONNECTTIMEOUT) ? sshc_parse_time(sshc_cfg_str(cfg, SSHC_O_CONNECTTIMEOUT)) : 0;
    if (ct < 0) { snprintf(s->err, sizeof s->err, "Bad ConnectTimeout '%s'", sshc_cfg_str(cfg, SSHC_O_CONNECTTIMEOUT)); return -1; }
    lg->connect_timeout_s = (int)ct;
    lg->strict = sshc_cfg_str(cfg, SSHC_O_STRICT) ? sshc_cfg_str(cfg, SSHC_O_STRICT) : "ask";
    if (!strcasecmp(lg->strict, "true")) lg->strict = "yes";
    else if (!strcasecmp(lg->strict, "false")) lg->strict = "no";
    else if (strcasecmp(lg->strict, "ask") && strcasecmp(lg->strict, "yes") && strcasecmp(lg->strict, "no") && strcasecmp(lg->strict, "off") &&
             strcasecmp(lg->strict, "accept-new")) {
        snprintf(s->err, sizeof s->err, "Bad StrictHostKeyChecking '%s'", lg->strict);
        return -1;
    }
    lg->hash_known_hosts = sshc_cfg_bool(cfg, SSHC_O_HASHKNOWNHOSTS, 0) != 0;

    const char *ukh = sshc_cfg_str(cfg, SSHC_O_USERKNOWNHOSTS);
    char kbuf[1024], *kw[3];
    int nk = ukh ? sshc_words(ukh, kbuf, sizeof kbuf, kw, 3) : 0;
    if (nk == 0) { kw[0] = (char *)"~/.ssh/known_hosts"; nk = 1; }
    for (int k = 0; k < nk && lg->n_known_hosts < 3; k++) {
        if (!strcasecmp(kw[k], "none")) continue;
        if (!sshc_expand_path(kw[k], s->kh[lg->n_known_hosts], sizeof s->kh[0], host, user, (int)port)) { snprintf(s->err, sizeof s->err, "bad UserKnownHostsFile '%s'", kw[k]); return -1; }
        lg->known_hosts[lg->n_known_hosts] = s->kh[lg->n_known_hosts];
        lg->n_known_hosts++;
    }
    const char *gkh = sshc_cfg_str(cfg, SSHC_O_GLOBALKNOWNHOSTS) ? sshc_cfg_str(cfg, SSHC_O_GLOBALKNOWNHOSTS) : "/etc/ssh/ssh_known_hosts";
    if (strcasecmp(gkh, "none") && lg->n_known_hosts < 4) {
        if (!sshc_expand_path(gkh, s->kh[lg->n_known_hosts], sizeof s->kh[0], host, user, (int)port)) { snprintf(s->err, sizeof s->err, "bad GlobalKnownHostsFile '%s'", gkh); return -1; }
        lg->known_hosts[lg->n_known_hosts] = s->kh[lg->n_known_hosts];
        lg->n_known_hosts++;
    }

    int nid = 0;
    for (int k = 0; k < cfg->n[SSHC_O_IDENTITYFILE] && nid < 8; k++) {
        if (!sshc_expand_path(cfg->vals[SSHC_O_IDENTITYFILE][k], s->idfiles[nid], sizeof s->idfiles[0], s->hostname, user, (int)port)) {
            snprintf(s->err, sizeof s->err, "bad IdentityFile '%s'", cfg->vals[SSHC_O_IDENTITYFILE][k]);
            return -1;
        }
        s->idptr[nid] = s->idfiles[nid];
        nid++;
    }
    if (nid == 0) {
        sshc_expand_path("~/.ssh/id_ed25519", s->idfiles[0], sizeof s->idfiles[0], s->hostname, user, (int)port);
        s->idptr[nid++] = s->idfiles[0];
    }
    lg->auth.identity_files = s->idptr;
    lg->auth.n_identities = nid;
    lg->auth.pubkey = sshc_cfg_bool(cfg, SSHC_O_PUBKEYAUTH, 1) != 0;
    lg->auth.password = sshc_cfg_bool(cfg, SSHC_O_PASSWORDAUTH, 1) != 0;
    lg->auth.kbdint = sshc_cfg_bool(cfg, SSHC_O_KBDAUTH, 1) != 0;
    lg->auth.batch = sshc_batch;
    lg->auth.max_prompts = (int)sshc_cfg_int(cfg, SSHC_O_NUMPROMPTS, 3);
    const char *pref = sshc_cfg_str(cfg, SSHC_O_PREFERREDAUTH);
    if (pref) {                                                         /* only the methods named, whatever the other keywords say */
        size_t pl = strlen(pref);
        lg->auth.pubkey = lg->auth.pubkey && sshc_namelist_has((const uint8_t *)pref, pl, "publickey");
        lg->auth.password = lg->auth.password && sshc_namelist_has((const uint8_t *)pref, pl, "password");
        lg->auth.kbdint = lg->auth.kbdint && sshc_namelist_has((const uint8_t *)pref, pl, "keyboard-interactive");
    }
    const char *rl = sshc_cfg_str(cfg, SSHC_O_REKEYLIMIT);
    if (rl && strcasecmp(rl, "none")) {
        char rbuf[128], *rw[2];
        int nr = sshc_words(rl, rbuf, sizeof rbuf, rw, 2);
        if (nr > 0 && strcasecmp(rw[0], "default")) {
            long long bytes = sshc_parse_size(rw[0]);
            if (bytes <= 0) { snprintf(s->err, sizeof s->err, "Bad RekeyLimit '%s'", rl); return -1; }
            lg->rekey_bytes = (uint64_t)bytes;
        }
        if (nr > 1) { long t = sshc_parse_time(rw[1]); if (t > 0) lg->rekey_ms = t * 1000; }
    }
    return 0;
}
