/*
 * Connecting and logging in, with the host-key policy of ssh(1).
 *
 * The question "is this the machine I meant to reach" is answered from known_hosts:
 *   known and the same  -> go on;
 *   known but DIFFERENT -> refused, always, whatever StrictHostKeyChecking says (OpenSSH's "no" would go on with a warning; here the answer to a key that changed is
 *                          never "carry on", because a password typed after that goes to whoever answered);
 *   not known           -> StrictHostKeyChecking: "yes" refuses, "accept-new" and "no" remember the key and go on, "ask" (the default) shows the fingerprint and
 *                          asks, on the terminal, and refuses when there is no terminal to ask on (BatchMode, a script).
 * A key marked @revoked is refused wherever it turns up. The warning for a changed key follows the shape of OpenSSH's, so that people who know it recognise it.
 */
#include <sshc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct { const SshcLogin *lg; } HkCtx;

static bool quiet(void) { return sshc_verbose < 0; }

static void shown_name(const SshcLogin *lg, char *out, size_t cap) { sshc_hk_name(lg->name, lg->port, out, cap); }

static void warn_changed(const SshcLogin *lg, const char *fp, const SshcHkResult *r) {
    char name[320];
    shown_name(lg, name, sizeof name);
    fprintf(stderr,
            "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
            "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
            "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
            "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
            "Someone could be eavesdropping on you right now (man-in-the-middle attack)!\n"
            "It is also possible that a host key has just been changed.\n"
            "The fingerprint for the ED25519 key sent by the remote host is\n"
            "%s.\n"
            "Please contact your system administrator.\n"
            "Add correct host key in %s to get rid of this message.\n"
            "Offending ED25519 key in %s:%d\n"
            "Host key for %s has changed and you have requested strict checking.\n",
            fp, lg->known_hosts[0], r->file, r->line, name);
}

static int hostkey_check(void *vctx, SshcConn *c, const uint8_t *blob, size_t n) {
    const SshcLogin *lg = ((HkCtx *)vctx)->lg;
    SshcPub key;
    if (sshc_pub_from_blob(blob, n, &key) != 0) return 1;
    char fp[64], name[320];
    sshc_fingerprint(blob, n, fp);
    shown_name(lg, name, sizeof name);
    sshc_debug(1, "Server host key: ssh-ed25519 %s", fp);
    SshcHkResult r = sshc_hk_check(lg->known_hosts, lg->n_known_hosts, lg->name, lg->port, blob, n);
    if (r.status == SSHC_HK_OK) {
        sshc_debug(1, "Host '%s' is known and matches the ED25519 host key (%s:%d)", name, r.file, r.line);
        return 0;
    }
    if (r.status == SSHC_HK_REVOKED) {
        fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                        "@       WARNING: REVOKED HOST KEY DETECTED!               @\n"
                        "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                        "Host key %s is marked as revoked (%s:%d).\n", fp, r.file, r.line);
        return 1;
    }
    if (r.status == SSHC_HK_CHANGED) {
        warn_changed(lg, fp, &r);
        return 1;
    }
    /* not known */
    const char *strict = lg->strict ? lg->strict : "ask";
    bool add = false;
    if (!strcmp(strict, "yes")) {
        fprintf(stderr, "No ED25519 host key is known for %s and you have requested strict checking.\n", name);
        return 1;
    } else if (!strcmp(strict, "no") || !strcmp(strict, "off") || !strcmp(strict, "accept-new")) {
        add = true;
    } else {
        fprintf(stderr, "The authenticity of host '%s (%s)' can't be established.\nED25519 key fingerprint is %s.\n", name, c->peer, fp);
        if (r.other_types) fprintf(stderr, "This host is known by other key types, which this client cannot use.\n");
        else fprintf(stderr, "This key is not known by any other names.\n");
        for (;;) {
            char answer[256];
            if (sshc_prompt("Are you sure you want to continue connecting (yes/no/[fingerprint])? ", true, answer, sizeof answer) != 0) {
                fprintf(stderr, "No terminal to ask on. Add the key to %s (ssh-keyscan %s) or use -o StrictHostKeyChecking=accept-new.\n",
                        lg->known_hosts[0], lg->name);
                return 1;
            }
            char *a = answer;
            while (*a == ' ' || *a == '\t') a++;
            size_t al = strlen(a);
            while (al && (a[al - 1] == ' ' || a[al - 1] == '\t')) a[--al] = 0;
            if (!strcasecmp(a, "yes")) { add = true; break; }
            if (!strcasecmp(a, "no")) return 1;
            if (al > 7 && !strcmp(a, fp)) { add = true; break; }
            fprintf(stderr, "Please type 'yes', 'no' or the fingerprint: ");
            fflush(stderr);
        }
    }
    if (add) {
        if (lg->n_known_hosts > 0 && sshc_hk_add(lg->known_hosts[0], lg->name, lg->port, lg->hash_known_hosts, &key) == 0) {
            if (!quiet()) fprintf(stderr, "Warning: Permanently added '%s' (ED25519) to the list of known hosts.\n", name);
        } else if (!quiet()) {
            fprintf(stderr, "Failed to add the host to the list of known hosts (%s).\n", lg->n_known_hosts ? lg->known_hosts[0] : "no file");
        }
    }
    return 0;
}

SshcConn *sshc_login(const SshcLogin *lg, char *why, size_t cap) {
    char ip[48] = "", err[300] = "";
    int fd = sshc_tcp_connect(lg->host, lg->port, lg->connect_timeout_s * 1000, err, sizeof err, ip, sizeof ip);
    if (fd < 0) {
        snprintf(why, cap, "ssh: %s", err);
        return NULL;
    }
    SshcConn *c = sshc_conn_new(fd);
    if (!c) {
        close(fd);
        snprintf(why, cap, "ssh: out of memory");
        return NULL;
    }
    snprintf(c->peer, sizeof c->peer, "%s", ip[0] ? ip : lg->host);
    c->peer_port = (unsigned)lg->port;
    HkCtx ctx = { lg };
    c->hostkey_fn = hostkey_check;
    c->hostkey_ctx = &ctx;
    c->rekey_bytes = lg->rekey_bytes;
    c->rekey_ms = lg->rekey_ms;
    c->deadline_ms = lg->connect_timeout_s > 0 ? sshc_mono_ms() + (long)lg->connect_timeout_s * 1000 : 0;
    sshc_debug(1, "Connecting to %s [%s] port %d.", lg->host, c->peer, lg->port);
    if (sshc_handshake(c) < 0) {
        snprintf(why, cap, "%s", c->err);
        sshc_conn_free(c);
        return NULL;
    }
    c->deadline_ms = 0;
    SshcAuthOpts a = lg->auth;
    a.user = lg->user;
    a.host = lg->name;
    if (sshc_authenticate(c, &a) < 0) {
        snprintf(why, cap, "%s", c->err);
        sshc_conn_free(c);
        return NULL;
    }
    sshc_debug(1, "Authentication succeeded for %s", lg->user);
    return c;
}
