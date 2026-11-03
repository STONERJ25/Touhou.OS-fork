/*
 * User authentication (RFC 4252), the client's side.
 *
 * The order is OpenSSH's: ask what the server accepts with the method "none", then public keys, keyboard-interactive (RFC 4256), password. A public key is first
 * OFFERED (a request without a signature, which the server answers "yes, I would take that key" or not at all), and only when the server would take it is the
 * passphrase of an encrypted key file asked for and the signature made: a person is never asked for the passphrase of a key the server does not know. The signed
 * data is the session id followed by the request itself (RFC 4252, section 7). Only ssh-ed25519 keys exist here.
 *
 * Passwords and answers are read with sshc_prompt() (the terminal, no echo) and never kept longer than the request; BatchMode refuses every question, so a script gets a
 * clean "Permission denied" instead of a hang.
 */
#include <sshc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sshcrypto.h>

enum { V_SUCCESS = 1, V_FAILURE = 2, V_OTHER = 3 };

typedef struct {
    char methods[160];                                  /* the server's list of methods that can continue, as it last said */
    bool partial;
} State;

static bool quiet(void) { return sshc_verbose < 0; }

/* The next message that is not a banner (which is shown). Returns its length, -1 on an error or disconnect (c->err says which). */
static int auth_recv(SshcConn *c, const uint8_t **pl) {
    for (;;) {
        int n = sshc_recv(c, pl, -1);
        if (n <= 0) {
            if (n == 0 && !c->err[0]) snprintf(c->err, sizeof c->err, "Connection closed by %s port %u", c->peer, c->peer_port);
            return -1;
        }
        if ((*pl)[0] == SSHC_MSG_USERAUTH_BANNER) {
            SshcRd r;
            sshc_rd_init(&r, *pl + 1, (size_t)n - 1);
            size_t ml;
            const uint8_t *msg = sshc_rd_str(&r, &ml);
            if (!r.bad && !quiet()) {
                char shown[2048];
                sshc_printable(shown, sizeof shown, msg, ml, true);    /* the server's words, minus anything that could move the cursor or retitle the window */
                fprintf(stderr, "%s%s", shown, ml && msg[ml - 1] == '\n' ? "" : "\n");
            }
            continue;
        }
        return n;
    }
}

/* Reads the server's verdict on a request: success, failure (the method list is stored), or some other message (left in *pl). */
static int verdict(SshcConn *c, State *st, const uint8_t **pl, int *n) {
    *n = auth_recv(c, pl);
    if (*n < 0) return -1;
    if ((*pl)[0] == SSHC_MSG_USERAUTH_SUCCESS) return V_SUCCESS;
    if ((*pl)[0] == SSHC_MSG_USERAUTH_FAILURE) {
        SshcRd r;
        sshc_rd_init(&r, *pl + 1, (size_t)*n - 1);
        size_t ml;
        const uint8_t *m = sshc_rd_str(&r, &ml);
        st->partial = sshc_rd_bool(&r);
        if (r.bad) { snprintf(c->err, sizeof c->err, "Malformed authentication failure from the server"); return -1; }
        sshc_printable(st->methods, sizeof st->methods, m, ml, false);
        sshc_debug(1, "Authentications that can continue: %s", st->methods);
        return V_FAILURE;
    }
    return V_OTHER;
}

static bool offers(const State *st, const char *method) {
    return sshc_namelist_has((const uint8_t *)st->methods, strlen(st->methods), method);
}

static void begin(SshcWr *w, const char *user, const char *method) {
    sshc_wr_u8(w, SSHC_MSG_USERAUTH_REQUEST);
    sshc_wr_cstr(w, user);
    sshc_wr_cstr(w, "ssh-connection");
    sshc_wr_cstr(w, method);
}

/* ------------------------------------------------------------------------------------------------ public keys */

/* 1 logged in, 0 no key worked, -1 the connection is lost. */
static int try_publickey(SshcConn *c, const SshcAuthOpts *o, State *st) {
    for (int i = 0; i < o->n_identities; i++) {
        const char *path = o->identity_files[i];
        const char *why = NULL;
        size_t len;
        char *text = sshc_read_file(path, 1u << 20, &len, true, &why);
        if (!text) {
            if (why && strstr(why, "too open")) {
                if (!quiet())
                    fprintf(stderr, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                                    "@         WARNING: UNPROTECTED PRIVATE KEY FILE!          @\n"
                                    "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                                    "Permissions of '%s' are too open.\n"
                                    "It is required that your private key files are NOT accessible by others.\n"
                                    "This private key will be ignored.\n", path);
            } else {
                sshc_debug(1, "no identity at %s (%s)", path, why ? why : "unreadable");
            }
            continue;
        }
        SshcKeyFile kf;
        const char *err = NULL;
        int pr = sshc_keyfile_parse(text, len, &kf, &err);
        memset(text, 0, len);
        free(text);
        if (pr != 0) {
            if (!quiet()) fprintf(stderr, "Load key \"%s\": %s\n", path, err);
            continue;
        }
        if (strcmp(kf.pub.type, "ssh-ed25519") != 0) {
            sshc_debug(1, "skipping %s: a %s key (only ssh-ed25519 is supported)", path, kf.pub.type);
            sshc_keyfile_free(&kf);
            continue;
        }
        sshc_debug(1, "Offering public key: %s ED25519", path);
        uint8_t req[SSHC_BLOB_MAX + 256];
        SshcWr w;
        sshc_wr_init(&w, req, sizeof req);
        begin(&w, o->user, "publickey");
        sshc_wr_bool(&w, false);
        sshc_wr_cstr(&w, "ssh-ed25519");
        sshc_wr_str(&w, kf.pub.blob, kf.pub.blob_len);
        if (w.bad || sshc_send(c, req, w.n) < 0) { sshc_keyfile_free(&kf); return -1; }
        const uint8_t *pl;
        int n;
        int v = verdict(c, st, &pl, &n);
        if (v < 0) { sshc_keyfile_free(&kf); return -1; }
        if (v == V_SUCCESS) { sshc_keyfile_free(&kf); return 1; }
        if (v == V_FAILURE) {
            sshc_keyfile_free(&kf);
            if (!offers(st, "publickey")) return 0;
            continue;
        }
        if (pl[0] != SSHC_MSG_USERAUTH_PK_OK) {
            snprintf(c->err, sizeof c->err, "Unexpected message %u during public key authentication", pl[0]);
            sshc_keyfile_free(&kf);
            return -1;
        }
        /* the server would take this key: now the passphrase, if there is one */
        SshcEd25519 key;
        const char *uerr = NULL;
        int rc = sshc_keyfile_unlock(&kf, NULL, &key, &uerr);
        for (int attempt = 0; rc == -2 && attempt < 3; attempt++) {
            char pass[256], prompt[300];
            snprintf(prompt, sizeof prompt, "Enter passphrase for key '%s': ", path);
            if (sshc_prompt(prompt, false, pass, sizeof pass) != 0) { uerr = "no passphrase given"; rc = -1; break; }
            rc = sshc_keyfile_unlock(&kf, pass, &key, &uerr);
            memset(pass, 0, sizeof pass);
            if (rc == -2 && !quiet() && attempt < 2) fprintf(stderr, "Bad passphrase, try again for %s\n", path);
        }
        sshc_keyfile_free(&kf);
        if (rc != 0) {
            if (!quiet()) fprintf(stderr, "Load key \"%s\": %s\n", path, uerr ? uerr : "cannot use the key");
            continue;
        }
        uint8_t blob[51], signed_data[512], sig[64];
        sshc_ed25519_blob(key.pk, blob);
        SshcWr s;
        sshc_wr_init(&s, signed_data, sizeof signed_data);
        sshc_wr_str(&s, c->session_id, 32);
        begin(&s, o->user, "publickey");
        sshc_wr_bool(&s, true);
        sshc_wr_cstr(&s, "ssh-ed25519");
        sshc_wr_str(&s, blob, sizeof blob);
        if (s.bad) { memset(&key, 0, sizeof key); return -1; }
        ed25519_sign(sig, signed_data, s.n, key.seed, key.pk);
        memset(&key, 0, sizeof key);
        uint8_t sigblob[4 + 11 + 4 + 64];
        SshcWr sb;
        sshc_wr_init(&sb, sigblob, sizeof sigblob);
        sshc_wr_cstr(&sb, "ssh-ed25519");
        sshc_wr_str(&sb, sig, sizeof sig);
        sshc_wr_init(&w, req, sizeof req);
        begin(&w, o->user, "publickey");
        sshc_wr_bool(&w, true);
        sshc_wr_cstr(&w, "ssh-ed25519");
        sshc_wr_str(&w, blob, sizeof blob);
        sshc_wr_str(&w, sigblob, sb.n);
        if (w.bad || sshc_send(c, req, w.n) < 0) return -1;
        v = verdict(c, st, &pl, &n);
        if (v < 0) return -1;
        if (v == V_SUCCESS) return 1;
        sshc_debug(1, "the server refused the signature of %s", path);
        if (v == V_FAILURE && !offers(st, "publickey")) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ keyboard-interactive and password */

static int try_kbdint(SshcConn *c, const SshcAuthOpts *o, State *st) {
    uint8_t req[256];
    SshcWr w;
    sshc_wr_init(&w, req, sizeof req);
    begin(&w, o->user, "keyboard-interactive");
    sshc_wr_cstr(&w, "");
    sshc_wr_cstr(&w, "");
    if (w.bad || sshc_send(c, req, w.n) < 0) return -1;
    for (int round = 0; round < 8; round++) {
        const uint8_t *pl;
        int n;
        int v = verdict(c, st, &pl, &n);
        if (v < 0) return -1;
        if (v == V_SUCCESS) return 1;
        if (v == V_FAILURE) return 0;
        if (pl[0] != SSHC_MSG_USERAUTH_PK_OK) {              /* 60 again: here it is SSH_MSG_USERAUTH_INFO_REQUEST */
            snprintf(c->err, sizeof c->err, "Unexpected message %u during keyboard-interactive authentication", pl[0]);
            return -1;
        }
        SshcRd r;
        sshc_rd_init(&r, pl + 1, (size_t)n - 1);
        size_t nl, il, ll;
        const uint8_t *name = sshc_rd_str(&r, &nl), *instr = sshc_rd_str(&r, &il);
        sshc_rd_str(&r, &ll);
        uint32_t count = sshc_rd_u32(&r);
        if (r.bad || count > 16) { snprintf(c->err, sizeof c->err, "Malformed keyboard-interactive request"); return -1; }
        if (!quiet() && (nl || il)) {
            char shown[600];
            if (nl) { sshc_printable(shown, sizeof shown, name, nl, true); fprintf(stderr, "%s\n", shown); }
            if (il) { sshc_printable(shown, sizeof shown, instr, il, true); fprintf(stderr, "%s\n", shown); }
        }
        uint8_t resp[2048];
        SshcWr rw;
        sshc_wr_init(&rw, resp, sizeof resp);
        sshc_wr_u8(&rw, SSHC_MSG_USERAUTH_INFO_RESPONSE);
        sshc_wr_u32(&rw, count);
        for (uint32_t i = 0; i < count; i++) {
            size_t pl2;
            const uint8_t *p = sshc_rd_str(&r, &pl2);
            bool echo = sshc_rd_bool(&r);
            if (r.bad) { snprintf(c->err, sizeof c->err, "Malformed keyboard-interactive prompt"); return -1; }
            char prompt[300], answer[256];
            sshc_printable(prompt, sizeof prompt, p, pl2, false);
            if (sshc_prompt(prompt, echo, answer, sizeof answer) != 0) {
                sshc_debug(1, "no answer for the keyboard-interactive prompt: giving up on the method");
                return 0;
            }
            sshc_wr_cstr(&rw, answer);
            memset(answer, 0, sizeof answer);
        }
        if (rw.bad || sshc_send(c, resp, rw.n) < 0) return -1;
        memset(resp, 0, sizeof resp);
    }
    return 0;
}

static int try_password(SshcConn *c, const SshcAuthOpts *o, State *st) {
    int tries = o->max_prompts > 0 ? o->max_prompts : 3;
    for (int i = 0; i < tries; i++) {
        char prompt[300], pw[256];
        snprintf(prompt, sizeof prompt, "%s@%s's password: ", o->user, o->host ? o->host : c->peer);
        if (sshc_prompt(prompt, false, pw, sizeof pw) != 0) return 0;
        uint8_t req[512];
        SshcWr w;
        sshc_wr_init(&w, req, sizeof req);
        begin(&w, o->user, "password");
        sshc_wr_bool(&w, false);
        sshc_wr_cstr(&w, pw);
        memset(pw, 0, sizeof pw);
        if (w.bad) return 0;
        int rc = sshc_send(c, req, w.n);
        memset(req, 0, sizeof req);
        if (rc < 0) return -1;
        const uint8_t *pl;
        int n;
        int v = verdict(c, st, &pl, &n);
        if (v < 0) return -1;
        if (v == V_SUCCESS) return 1;
        if (v == V_OTHER) {
            if (pl[0] == SSHC_MSG_USERAUTH_PK_OK) {          /* SSH_MSG_USERAUTH_PASSWD_CHANGEREQ: the password has expired */
                if (!quiet()) fprintf(stderr, "The password has expired and must be changed; this client cannot change it.\n");
                return 0;
            }
            snprintf(c->err, sizeof c->err, "Unexpected message %u during password authentication", pl[0]);
            return -1;
        }
        if (!offers(st, "password")) return 0;
        if (i + 1 < tries && !quiet()) fprintf(stderr, "Permission denied, please try again.\n");
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the whole thing */

int sshc_authenticate(SshcConn *c, const SshcAuthOpts *o) {
    uint8_t m[64];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_SERVICE_REQUEST);
    sshc_wr_cstr(&w, "ssh-userauth");
    if (sshc_send(c, m, w.n) < 0) return -1;
    const uint8_t *pl;
    int n = auth_recv(c, &pl);
    if (n < 0) return -1;
    if (pl[0] != SSHC_MSG_SERVICE_ACCEPT) {
        snprintf(c->err, sizeof c->err, "The server did not accept the ssh-userauth service (message %u)", pl[0]);
        return -1;
    }
    State st;
    memset(&st, 0, sizeof st);
    uint8_t req[256];
    sshc_wr_init(&w, req, sizeof req);
    begin(&w, o->user, "none");
    if (sshc_send(c, req, w.n) < 0) return -1;
    int v = verdict(c, &st, &pl, &n);
    if (v < 0) return -1;
    if (v == V_SUCCESS) return 0;
    if (v == V_OTHER) {
        snprintf(c->err, sizeof c->err, "Unexpected message %u in reply to the first authentication request", pl[0]);
        return -1;
    }
    bool done_pk = false, done_kbd = false, done_pw = false;
    for (int round = 0; round < 6; round++) {
        int rc = 0;
        if (o->pubkey && !done_pk && offers(&st, "publickey")) {
            done_pk = true;
            rc = try_publickey(c, o, &st);
        } else if (o->kbdint && !o->batch && !done_kbd && offers(&st, "keyboard-interactive")) {
            done_kbd = true;
            rc = try_kbdint(c, o, &st);
        } else if (o->password && !o->batch && !done_pw && offers(&st, "password")) {
            done_pw = true;
            rc = try_password(c, o, &st);
        } else {
            break;
        }
        if (rc < 0) return -1;
        if (rc == 1) return 0;
        if (st.partial) done_pk = done_kbd = done_pw = false;       /* multi-step logins: what failed before may be wanted again */
    }
    snprintf(c->err, sizeof c->err, "%s@%s: Permission denied (%s).", o->user, o->host ? o->host : c->peer, st.methods[0] ? st.methods : "none");
    return -1;
}
