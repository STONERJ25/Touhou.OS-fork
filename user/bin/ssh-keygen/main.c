/*
 * ssh-keygen: makes and examines ed25519 keys and looks after known_hosts.
 *
 *   ssh-keygen -t ed25519 [-f file] [-N passphrase] [-C comment] [-a rounds] [-q]     make a key pair (file, file.pub)
 *   ssh-keygen -l [-f file] [-E md5|sha256] [-v]                                      fingerprint of a key, of each key in a file of keys
 *   ssh-keygen -y [-f file] [-P passphrase]                                            print the public key of a private key file
 *   ssh-keygen -p [-f file] [-P old] [-N new]                                          change a key's passphrase
 *   ssh-keygen -c [-f file] -C comment [-P passphrase]                                 change a key's comment
 *   ssh-keygen -F host [-f known_hosts]                                                find a host in known_hosts
 *   ssh-keygen -R host [-f known_hosts]                                                remove a host from known_hosts
 *   ssh-keygen -H [-f known_hosts]                                                     hash the host names in known_hosts
 *
 * The private key file is OpenSSH's own format (openssh-key-v1), with a passphrase it is encrypted with aes256-ctr and bcrypt_pbkdf (-a rounds, 16 by default),
 * so OpenSSH reads what is written here and this reads what OpenSSH wrote (docs/SSH_CLIENT.md). Other key types are not made; the fingerprint and public-key
 * commands do read keys of any type.
 */
#include <sshc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sshcrypto.h>

static void usage(void) {
    fprintf(stderr,
            "usage: ssh-keygen [-q] [-a rounds] [-C comment] [-f output_keyfile] [-N new_passphrase] -t ed25519\n"
            "       ssh-keygen [-E md5|sha256] [-f input_keyfile] [-v] -l\n"
            "       ssh-keygen [-f input_keyfile] [-P passphrase] -y\n"
            "       ssh-keygen [-f keyfile] [-P old_passphrase] [-N new_passphrase] -p\n"
            "       ssh-keygen [-f keyfile] [-P passphrase] -C comment -c\n"
            "       ssh-keygen [-f known_hosts_file] -F hostname | -R hostname | -H\n");
    exit(1);
}

static int g_quiet;

static bool exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static char *default_key_path(void) {
    static char path[512];
    sshc_expand_path("~/.ssh/id_ed25519", path, sizeof path, NULL, NULL, 22);
    return path;
}

static void default_comment(char *out, size_t cap) {
    char host[128] = "";
    if (gethostname(host, sizeof host) != 0 || !host[0]) sshc_cpy(host, sizeof host, "localhost");
    snprintf(out, cap, "%.60s@%.60s", sshc_local_user(), host);
}

/* Asks twice for a new passphrase; "" means none. 0, or -1 when there is no one to ask. */
static int ask_new_passphrase(char *out, size_t cap) {
    char again[256];
    for (int tries = 0; tries < 3; tries++) {
        if (sshc_prompt("Enter passphrase (empty for no passphrase): ", false, out, cap) != 0) return -1;
        if (sshc_prompt("Enter same passphrase again: ", false, again, sizeof again) != 0) return -1;
        if (strcmp(out, again) != 0) { fprintf(stderr, "Passphrases do not match.  Try again.\n"); continue; }
        memset(again, 0, sizeof again);
        return 0;
    }
    return -1;
}

static int write_new_file(const char *path, const char *data, size_t len, mode_t mode, bool overwrite) {
    int fd = open(path, O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), mode);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    chmod(path, mode);                                          /* a file that existed keeps its old mode otherwise */
    return close(fd);
}

static int cmd_generate(const char *type, const char *file, const char *pass, bool have_pass, const char *comment, unsigned rounds) {
    if (type && strcasecmp(type, "ed25519") != 0) {
        fprintf(stderr, "ssh-keygen: only ed25519 keys can be made here (-t ed25519)\n");
        return 1;
    }
    char path[512];
    if (!g_quiet) printf("Generating public/private ed25519 key pair.\n");
    if (file) sshc_cpy(path, sizeof path, file);
    else {
        char prompt[600], answer[512];
        snprintf(prompt, sizeof prompt, "Enter file in which to save the key (%s): ", default_key_path());
        if (sshc_prompt(prompt, true, answer, sizeof answer) == 0 && answer[0]) sshc_cpy(path, sizeof path, answer);
        else sshc_cpy(path, sizeof path, default_key_path());
    }
    bool overwrite = false;
    if (exists(path)) {
        char answer[16];
        fprintf(stderr, "%s already exists.\n", path);
        if (sshc_prompt("Overwrite (y/n)? ", true, answer, sizeof answer) != 0 || (answer[0] != 'y' && answer[0] != 'Y')) return 1;
        overwrite = true;
    }
    char dir[512];
    sshc_cpy(dir, sizeof dir, path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (sshc_mkdir_p(dir) != 0) { fprintf(stderr, "Could not create directory '%s'.\n", dir); return 1; }
        if (!g_quiet && !file) printf("Created directory '%s'.\n", dir);
    }
    char pw[256] = "";
    if (have_pass) sshc_cpy(pw, sizeof pw, pass);
    else if (ask_new_passphrase(pw, sizeof pw) != 0) { fprintf(stderr, "ssh-keygen: no passphrase could be asked for (give one with -N, or -N '' for none)\n"); return 1; }
    if (pw[0] && strlen(pw) < 5) { fprintf(stderr, "Passphrase too short: have %zu characters, need at least 5.\n", strlen(pw)); return 1; }

    SshcEd25519 key;
    memset(&key, 0, sizeof key);
    sshc_random(key.seed, 32);
    ed25519_pubkey(key.pk, key.seed);
    if (comment) sshc_cpy(key.comment, sizeof key.comment, comment);
    else default_comment(key.comment, sizeof key.comment);
    char *text = NULL;
    if (sshc_keyfile_make(&key, pw[0] ? pw : NULL, rounds, &text) != 0) { fprintf(stderr, "ssh-keygen: could not make the key file\n"); return 1; }
    if (write_new_file(path, text, strlen(text), 0600, overwrite) != 0) {
        fprintf(stderr, "Saving key \"%s\" failed: %s\n", path, strerror(errno));
        memset(text, 0, strlen(text)); free(text); memset(&key, 0, sizeof key);
        return 1;
    }
    memset(text, 0, strlen(text));
    free(text);
    SshcPub pub;
    uint8_t blob[51];
    sshc_ed25519_blob(key.pk, blob);
    sshc_pub_from_blob(blob, sizeof blob, &pub);
    char line[400], pubpath[560], fp[64];
    sshc_pub_to_line(&pub, key.comment, line, sizeof line);
    strcat(line, "\n");
    snprintf(pubpath, sizeof pubpath, "%s.pub", path);
    if (write_new_file(pubpath, line, strlen(line), 0644, true) != 0) { fprintf(stderr, "Saving key \"%s\" failed: %s\n", pubpath, strerror(errno)); return 1; }
    sshc_fingerprint(blob, sizeof blob, fp);
    if (!g_quiet) {
        char art[1024];
        sshc_randomart(blob, sizeof blob, "ED25519", 256, art, sizeof art);
        printf("Your identification has been saved in %s\nYour public key has been saved in %s\nThe key fingerprint is:\n%s %s\nThe key's randomart image is:\n%s", path, pubpath, fp, key.comment, art);
    }
    memset(&key, 0, sizeof key);
    return 0;
}

/* The public key of a file that is a private key (read from its clear part) or a public key line. comment may stay empty. */
static int load_public(const char *path, SshcPub *pub, char *comment, size_t ccap, bool *was_private) {
    size_t len;
    const char *why = NULL;
    char *text = sshc_read_file(path, 1u << 20, &len, false, &why);
    if (!text) { fprintf(stderr, "%s: %s\n", path, why); return -1; }
    comment[0] = 0;
    *was_private = false;
    SshcKeyFile kf;
    const char *err;
    int rc = -1;
    if (strstr(text, "-----BEGIN OPENSSH PRIVATE KEY-----") && sshc_keyfile_parse(text, len, &kf, &err) == 0) {
        *pub = kf.pub;
        *was_private = true;
        SshcEd25519 key;
        if (!kf.encrypted && sshc_keyfile_unlock(&kf, NULL, &key, &err) == 0) sshc_cpy(comment, ccap, key.comment);
        else {                                                   /* an encrypted key keeps its comment inside: the .pub next to it has it too */
            char pp[600];
            snprintf(pp, sizeof pp, "%s.pub", path);
            size_t pl;
            char *ptext = sshc_read_file(pp, 1u << 16, &pl, false, &why);
            SshcPub tmp;
            if (ptext) { sshc_pub_parse(ptext, &tmp, comment, ccap); free(ptext); }
        }
        sshc_keyfile_free(&kf);
        rc = 0;
    } else if (sshc_pub_parse(text, pub, comment, ccap) == 0) {
        rc = 0;
    } else if (!strstr(text, "-----BEGIN")) {
        fprintf(stderr, "%s is not a public key file.\n", path);
    } else {
        fprintf(stderr, "%s: this key format is not supported (OpenSSH-format ed25519 keys only)\n", path);
    }
    free(text);
    return rc;
}

static void print_fingerprint_line(const SshcPub *pub, const char *comment, bool md5, bool visual) {
    char fp[64];
    if (md5) sshc_fingerprint_md5(pub->blob, pub->blob_len, fp);
    else sshc_fingerprint(pub->blob, pub->blob_len, fp);
    printf("%d %s %s (%s)\n", sshc_pub_bits(pub), fp, comment[0] ? comment : "no comment", sshc_pub_label(pub));
    if (visual) {
        char art[1024];
        sshc_randomart(pub->blob, pub->blob_len, sshc_pub_label(pub), sshc_pub_bits(pub), art, sizeof art);
        fputs(art, stdout);
    }
}

/* -l on a file of several keys (authorized_keys, known_hosts): the key is the first field that names a key type. */
static int fingerprint_lines(const char *text, bool md5, bool visual) {
    int count = 0;
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        char line[2048];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        p = eol ? eol + 1 : p + ll;
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '#') continue;
        for (int field = 0; field < 3 && *q; field++) {
            SshcPub pub;
            char comment[200];
            if (sshc_pub_parse(q, &pub, comment, sizeof comment) == 0) {
                print_fingerprint_line(&pub, comment, md5, visual);
                count++;
                break;
            }
            while (*q && *q != ' ' && *q != '\t') q++;
            while (*q == ' ' || *q == '\t') q++;
        }
    }
    return count;
}

static int cmd_fingerprint(const char *file, bool md5, bool visual) {
    char path[512];
    sshc_cpy(path, sizeof path, file ? file : default_key_path());
    size_t len;
    const char *why = NULL;
    char *text = sshc_read_file(path, 4u << 20, &len, false, &why);
    if (!text) { fprintf(stderr, "%s: %s\n", path, why); return 1; }
    int rc = 1;
    if (strstr(text, "-----BEGIN")) {
        SshcPub pub;
        char comment[200];
        bool priv;
        if (load_public(path, &pub, comment, sizeof comment, &priv) == 0) { print_fingerprint_line(&pub, comment, md5, visual); rc = 0; }
    } else if (fingerprint_lines(text, md5, visual) > 0) {
        rc = 0;
    } else {
        fprintf(stderr, "%s is not a public key file.\n", path);
    }
    free(text);
    return rc;
}

/* Loads and unlocks a private key, asking for the passphrase when needed. */
static int load_private(const char *path, const char *pass, bool have_pass, SshcEd25519 *key) {
    size_t len;
    const char *why = NULL, *err = NULL;
    char *text = sshc_read_file(path, 1u << 20, &len, true, &why);
    if (!text) { fprintf(stderr, "Load key \"%s\": %s\n", path, why); return -1; }
    SshcKeyFile kf;
    if (sshc_keyfile_parse(text, len, &kf, &err) != 0) { fprintf(stderr, "Load key \"%s\": %s\n", path, err); free(text); return -1; }
    free(text);
    int rc = sshc_keyfile_unlock(&kf, have_pass ? pass : NULL, key, &err);
    for (int tries = 0; rc == -2 && !have_pass && tries < 3; tries++) {
        char pw[256], prompt[600];
        snprintf(prompt, sizeof prompt, "Enter passphrase for \"%s\": ", path);
        if (sshc_prompt(prompt, false, pw, sizeof pw) != 0) break;
        rc = sshc_keyfile_unlock(&kf, pw, key, &err);
        memset(pw, 0, sizeof pw);
    }
    if (rc != 0) fprintf(stderr, "Load key \"%s\": %s\n", path, err ? err : "failed");
    sshc_keyfile_free(&kf);
    return rc == 0 ? 0 : -1;
}

static int cmd_public(const char *file, const char *pass, bool have_pass) {
    char path[512];
    sshc_cpy(path, sizeof path, file ? file : default_key_path());
    SshcPub pub;
    char comment[200];
    bool priv;
    if (load_public(path, &pub, comment, sizeof comment, &priv) != 0) return 1;
    if (priv) {                                                  /* the comment of an encrypted key needs the passphrase */
        SshcEd25519 key;
        if (!comment[0] && load_private(path, pass, have_pass, &key) == 0) sshc_cpy(comment, sizeof comment, key.comment);
    }
    char line[2048];
    sshc_pub_to_line(&pub, comment, line, sizeof line);
    puts(line);
    return 0;
}

static int cmd_change(const char *file, const char *oldpass, bool have_old, const char *newpass, bool have_new, const char *new_comment, bool comment_only, unsigned rounds) {
    char path[512];
    sshc_cpy(path, sizeof path, file ? file : default_key_path());
    SshcEd25519 key;
    if (load_private(path, oldpass, have_old, &key) != 0) return 1;
    char npw[256] = "";
    const char *use = NULL;
    if (comment_only) {
        if (!new_comment) { fprintf(stderr, "ssh-keygen: -c needs -C comment\n"); return 1; }
        sshc_cpy(key.comment, sizeof key.comment, new_comment);
        /* keep the key's current protection: it was unlocked with `oldpass` (or typed), which is what it is saved with again */
        if (have_old) use = oldpass;
        else {
            size_t len;
            const char *why = NULL, *err = NULL;
            char *text = sshc_read_file(path, 1u << 20, &len, true, &why);
            SshcKeyFile kf;
            bool enc = text && sshc_keyfile_parse(text, len, &kf, &err) == 0 && kf.encrypted;
            if (text && enc) sshc_keyfile_free(&kf);
            free(text);
            if (enc) {
                if (sshc_prompt("Enter the passphrase again to keep the key protected: ", false, npw, sizeof npw) != 0) return 1;
                use = npw;
            }
        }
    } else if (have_new) {
        sshc_cpy(npw, sizeof npw, newpass);
        use = npw[0] ? npw : NULL;
    } else {
        if (ask_new_passphrase(npw, sizeof npw) != 0) { fprintf(stderr, "ssh-keygen: no passphrase could be asked for\n"); return 1; }
        use = npw[0] ? npw : NULL;
    }
    if (use && strlen(use) < 5) { fprintf(stderr, "Passphrase too short: have %zu characters, need at least 5.\n", strlen(use)); return 1; }
    char *text;
    if (sshc_keyfile_make(&key, use, rounds, &text) != 0) { fprintf(stderr, "ssh-keygen: could not make the key file\n"); return 1; }
    if (write_new_file(path, text, strlen(text), 0600, true) != 0) { fprintf(stderr, "Saving key \"%s\" failed: %s\n", path, strerror(errno)); return 1; }
    memset(text, 0, strlen(text));
    free(text);
    char pubpath[560], line[400];
    uint8_t blob[51];
    SshcPub pub;
    sshc_ed25519_blob(key.pk, blob);
    sshc_pub_from_blob(blob, sizeof blob, &pub);
    sshc_pub_to_line(&pub, key.comment, line, sizeof line);
    strcat(line, "\n");
    snprintf(pubpath, sizeof pubpath, "%s.pub", path);
    write_new_file(pubpath, line, strlen(line), 0644, true);
    if (!g_quiet) printf(comment_only ? "Comment '%s' applied\n" : "Your identification has been saved with the new passphrase.\n", key.comment);
    memset(&key, 0, sizeof key);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ known_hosts maintenance */

static int cmd_find_host(const char *file, const char *host, bool remove_it) {
    char path[512];
    if (file) sshc_cpy(path, sizeof path, file);
    else sshc_expand_path("~/.ssh/known_hosts", path, sizeof path, NULL, NULL, 22);
    size_t len;
    const char *why = NULL;
    char *text = sshc_read_file(path, 8u << 20, &len, false, &why);
    if (!text) { fprintf(stderr, "%s: %s\n", path, why); return 1; }
    /* the host as typed: "name", "[name]:port" or "name:port" */
    char name[320], bare[300];
    int port = 22;
    sshc_cpy(bare, sizeof bare, host);
    if (bare[0] == '[') {
        char *rb = strchr(bare, ']');
        if (rb && rb[1] == ':') { port = atoi(rb + 2); *rb = 0; memmove(bare, bare + 1, strlen(bare)); }
    }
    sshc_hk_name(bare, port, name, sizeof name);
    int lineno = 0, found = 0;
    size_t out_cap = len + 1, out_len = 0;
    char *out = malloc(out_cap);
    if (!out) { free(text); return 1; }
    for (char *p = text; *p;) {
        char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        lineno++;
        char line[4096];
        size_t cl = ll < sizeof line - 1 ? ll : sizeof line - 1;
        memcpy(line, p, cl);
        line[cl] = 0;
        bool hit = false;
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q && *q != '#') {
            if (*q == '@') { while (*q && *q != ' ' && *q != '\t') q++; while (*q == ' ' || *q == '\t') q++; }
            char *hosts_end = q;
            while (*hosts_end && *hosts_end != ' ' && *hosts_end != '\t') hosts_end++;
            char saved = *hosts_end;
            *hosts_end = 0;
            char *fields = q;
            if (!strncmp(fields, "|1|", 3)) {
                char *salt = fields + 3, *bar = strchr(salt, '|');
                uint8_t s[64], want[40];
                if (bar) {
                    *bar = 0;
                    int sl = sshc_b64_decode(salt, strlen(salt), s, sizeof s), wl = sshc_b64_decode(bar + 1, strlen(bar + 1), want, sizeof want);
                    uint8_t got[20];
                    if (sl > 0 && wl == 20) {
                        sshc_hmac_sha1(got, s, (size_t)sl, name, strlen(name));
                        hit = memcmp(got, want, 20) == 0;
                    }
                }
            } else {
                char *tok = fields;
                for (;;) {
                    char *comma = strchr(tok, ',');
                    if (comma) *comma = 0;
                    if (tok[0] != '!' && sshc_pattern_match(tok, name)) hit = true;
                    if (!comma) break;
                    tok = comma + 1;
                }
            }
            *hosts_end = saved;
        }
        if (hit) {
            found++;
            if (!remove_it) { printf("# Host %s found: line %d\n%.*s\n", host, lineno, (int)ll, p); }
        } else {
            memcpy(out + out_len, p, ll);
            out_len += ll;
            if (eol) out[out_len++] = '\n';
        }
        p = eol ? eol + 1 : p + ll;
    }
    int rc = 0;
    if (remove_it) {
        if (found) {
            char tmp[600], old[600];
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            snprintf(old, sizeof old, "%s.old", path);
            if (write_new_file(tmp, out, out_len, 0600, true) != 0) { fprintf(stderr, "could not write %s\n", tmp); rc = 1; }
            else {
                unlink(old);
                if (rename(path, old) != 0 || rename(tmp, path) != 0) { fprintf(stderr, "could not replace %s\n", path); rc = 1; }
                else printf("# Host %s found: %d entr%s removed from %s\n%s updated.\nOriginal contents retained as %s\n", host, found, found == 1 ? "y" : "ies", path, path, old);
            }
        } else {
            rc = 1;
        }
    } else if (!found) {
        rc = 1;
    }
    free(out);
    free(text);
    return rc;
}

static int cmd_hash(const char *file) {
    char path[512];
    if (file) sshc_cpy(path, sizeof path, file);
    else sshc_expand_path("~/.ssh/known_hosts", path, sizeof path, NULL, NULL, 22);
    size_t len;
    const char *why = NULL;
    char *text = sshc_read_file(path, 8u << 20, &len, false, &why);
    if (!text) { fprintf(stderr, "%s: %s\n", path, why); return 1; }
    size_t cap = len * 3 + 4096, n = 0;
    char *out = malloc(cap);
    if (!out) { free(text); return 1; }
    int hashed = 0;
    for (char *p = text; *p;) {
        char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        char line[4096];
        size_t cl = ll < sizeof line - 1 ? ll : sizeof line - 1;
        memcpy(line, p, cl);
        line[cl] = 0;
        p = eol ? eol + 1 : p + ll;
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        char *marker_end = q;
        if (*q == '@') { while (*marker_end && *marker_end != ' ' && *marker_end != '\t') marker_end++; while (*marker_end == ' ' || *marker_end == '\t') marker_end++; }
        char *hosts = marker_end, *hosts_end = hosts;
        while (*hosts_end && *hosts_end != ' ' && *hosts_end != '\t') hosts_end++;
        bool plain = *q && *q != '#' && strncmp(hosts, "|1|", 3) != 0 && !strpbrk(hosts, "*?!");
        if (!plain) { n += (size_t)snprintf(out + n, cap - n, "%s\n", line); continue; }
        char saved = *hosts_end;
        *hosts_end = 0;
        char *rest = saved ? hosts_end + 1 : hosts_end;
        for (char *tok = hosts; tok;) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = 0;
            uint8_t salt[20], mac[20];
            char sb[40], mb[40];
            sshc_random(salt, sizeof salt);
            sshc_hmac_sha1(mac, salt, sizeof salt, tok, strlen(tok));
            sshc_b64_encode(salt, sizeof salt, sb, true);
            sshc_b64_encode(mac, sizeof mac, mb, true);
            n += (size_t)snprintf(out + n, cap - n, "%.*s|1|%s|%s %s\n", (int)(marker_end - q), q, sb, mb, rest);
            hashed++;
            tok = comma ? comma + 1 : NULL;
        }
    }
    char tmp[600], old[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(old, sizeof old, "%s.old", path);
    int rc = 0;
    if (write_new_file(tmp, out, n, 0600, true) != 0) rc = 1;
    else {
        unlink(old);
        if (rename(path, old) != 0 || rename(tmp, path) != 0) rc = 1;
        else printf("%s updated.\nOriginal contents retained as %s\n", path, old);
    }
    if (rc) fprintf(stderr, "ssh-keygen: could not rewrite %s\n", path);
    (void)hashed;
    free(out);
    free(text);
    return rc;
}

int main(int argc, char **argv) {
    const char *type = NULL, *file = NULL, *newpass = NULL, *oldpass = NULL, *comment = NULL, *find_host = NULL, *remove_host = NULL, *hash_alg = "sha256";
    bool want_l = false, want_y = false, want_p = false, want_c = false, want_H = false, visual = false;
    unsigned rounds = 16;
    int opt_index = 1;
    for (; opt_index < argc; opt_index++) {
        char *a = argv[opt_index];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { opt_index++; break; }
        for (char *p = a + 1; *p; p++) {
            const char *val = NULL;
            if (strchr("tfNPCaEFR", *p)) {
                val = p[1] ? p + 1 : (opt_index + 1 < argc ? argv[++opt_index] : NULL);
                if (!val) { fprintf(stderr, "ssh-keygen: option requires an argument -- %c\n", *p); usage(); }
            }
            switch (*p) {
            case 't': type = val; break;
            case 'f': file = val; break;
            case 'N': newpass = val; break;
            case 'P': oldpass = val; break;
            case 'C': comment = val; break;
            case 'a': rounds = (unsigned)atoi(val); if (rounds < 1 || rounds > 1000) { fprintf(stderr, "Invalid number of rounds\n"); return 1; } break;
            case 'E': hash_alg = val; break;
            case 'F': find_host = val; break;
            case 'R': remove_host = val; break;
            case 'q': g_quiet = 1; break;
            case 'l': want_l = true; break;
            case 'y': want_y = true; break;
            case 'p': want_p = true; break;
            case 'c': want_c = true; break;
            case 'H': want_H = true; break;
            case 'v': visual = true; break;
            default: usage();
            }
            if (val && p[1]) break;
            if (val) break;
        }
    }
    if (opt_index < argc) usage();
    if (strcasecmp(hash_alg, "sha256") && strcasecmp(hash_alg, "md5")) { fprintf(stderr, "ssh-keygen: unknown fingerprint hash '%s'\n", hash_alg); return 1; }
    if (want_l) return cmd_fingerprint(file, !strcasecmp(hash_alg, "md5"), visual);
    if (want_y) return cmd_public(file, oldpass, oldpass != NULL);
    if (want_p) return cmd_change(file, oldpass, oldpass != NULL, newpass, newpass != NULL, NULL, false, rounds);
    if (want_c) return cmd_change(file, oldpass, oldpass != NULL, NULL, false, comment, true, rounds);
    if (find_host) return cmd_find_host(file, find_host, false);
    if (remove_host) return cmd_find_host(file, remove_host, true);
    if (want_H) return cmd_hash(file);
    return cmd_generate(type, file, newpass, newpass != NULL, comment, rounds);           /* no other command asked for: make a key (ed25519, the default) */
}
