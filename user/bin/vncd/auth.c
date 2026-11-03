/*
 * The VNC password and the throttle on wrong ones.
 *
 * VNC Authentication needs the password itself (the server encrypts the challenge with it), so /etc/vnc.passwd holds it as
 * text: one line of 1 to 8 printable ASCII characters (no spaces), the file root's and 0600. A file that others may read or
 * write, that is not a plain file, or that does not hold a password is not used, and the server then turns every viewer away
 * (failing closed: a damaged password file must not become "no password"). `vncd -passwd` writes it, and a copy on the
 * HUMANVILLAGE volume (/HumanVillage/.system/etc/vnc.passwd, root's like everything under .system) that vncd puts back into /etc
 * when it starts on a fresh /etc, the way init restores Keine's records (docs/PERSISTENT_ETC.md).
 *
 * The throttle: every wrong password is answered only after a pause (1 s), and an address that gives `throttle_fails` wrong
 * passwords within a minute is not served for the next minute (one line in the log, not one per attempt).
 */
#include "vncd.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define THROTTLE_WINDOW_MS 60000
#define THROTTLE_BLOCK_MS  60000

static bool valid_password(const char *p, size_t n, char *why, size_t whylen) {
    if (n == 0) { snprintf(why, whylen, "the password is empty"); return false; }
    if (n > 8) { snprintf(why, whylen, "VNC passwords have at most 8 characters (viewers send only the first 8)"); return false; }
    for (size_t i = 0; i < n; i++)
        if (p[i] <= 0x20 || p[i] >= 0x7F) { snprintf(why, whylen, "only printable ASCII characters without spaces"); return false; }
    return true;
}

/* Reads a password file: 1 and the password, 0 no such file, -1 unusable (reason in why). */
static int read_file(const char *path, char *out, size_t cap, char *why, size_t whylen) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        snprintf(why, whylen, "cannot open it: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    char buf[64];
    ssize_t n = -1;
    int rc = -1;
    if (fstat(fd, &st) < 0) snprintf(why, whylen, "cannot look at it: %s", strerror(errno));
    else if (!S_ISREG(st.st_mode)) snprintf(why, whylen, "it is not a plain file");
    else if (st.st_uid != geteuid()) snprintf(why, whylen, "it does not belong to root");     /* the server's own user: root on the system */
    else if (st.st_mode & 077) snprintf(why, whylen, "others may read or write it (mode %03o; it must be 0600)", (unsigned)(st.st_mode & 0777));
    else if ((n = read(fd, buf, sizeof buf - 1)) < 0) snprintf(why, whylen, "cannot read it: %s", strerror(errno));
    else {
        buf[n] = 0;
        size_t len = strcspn(buf, "\n");
        if (len == (size_t)n) snprintf(why, whylen, "the line does not end (the file is cut short or longer than a password)");
        else if (buf[len + 1] != 0) snprintf(why, whylen, "it has more than one line");
        else if (valid_password(buf, len, why, whylen) && len < cap) {
            memcpy(out, buf, len);
            out[len] = 0;
            rc = 1;
        }
    }
    memset(buf, 0, sizeof buf);
    close(fd);
    return rc;
}

/* A fresh /etc (every boot) gets the copy kept on the volume: looked for at start and at every connection while /etc has none, so a
 * USB stick that is mounted after vncd started still brings its password. */
static void restore_from_volume(void) {
    if (!g_opt.keep_path) return;
    struct stat st;
    if (stat(g_opt.passwd_path, &st) == 0 || errno != ENOENT) return;
    static bool told;                                           /* a damaged copy or a failing write: said once, not at every connection */
    char pw[16], why[160];
    int rc = read_file(g_opt.keep_path, pw, sizeof pw, why, sizeof why);
    if (rc == 0) return;
    if (rc < 0) { if (!told) vlog("the kept password %s is not used: %s", g_opt.keep_path, why); told = true; return; }
    char line[16];
    size_t n = (size_t)snprintf(line, sizeof line, "%s\n", pw);
    if (keep_file_write(g_opt.passwd_path, line, n) == 0) vlog("the VNC password was restored from the Human Village's records (%s)", g_opt.keep_path);
    else if (!told) { vlog("cannot restore %s: %s", g_opt.passwd_path, strerror(errno)); told = true; }
    memset(pw, 0, sizeof pw);
    memset(line, 0, sizeof line);
}

int auth_read_password(char *out, size_t cap) {
    restore_from_volume();
    char why[160];
    int rc = read_file(g_opt.passwd_path, out, cap, why, sizeof why);
    if (rc < 0) {
        static long last_said;
        long now = mono_ms();
        if (!last_said || now - last_said > 60000) {          /* once a minute is enough */
            vlog("%s is not used: %s; every viewer is turned away until it is fixed (vncd -passwd)", g_opt.passwd_path, why);
            last_said = now;
        }
    }
    return rc;
}

int auth_set_password(const char *pw, char *why, size_t whylen) {
    size_t n = strlen(pw);
    if (!valid_password(pw, n, why, whylen)) return -1;
    char line[16];
    n = (size_t)snprintf(line, sizeof line, "%s\n", pw);
    if (keep_file_write(g_opt.passwd_path, line, n) < 0) {
        snprintf(why, whylen, "cannot write %s: %s", g_opt.passwd_path, strerror(errno));
        memset(line, 0, sizeof line);
        return -1;
    }
    int rc = 0;
    if (g_opt.keep_path) {
        int k = keep_file_write(g_opt.keep_path, line, n);
        if (k > 0) { snprintf(why, whylen, "no HUMANVILLAGE volume: the password lasts until the next restart"); rc = 1; }
        else if (k < 0) { snprintf(why, whylen, "the volume could not keep it (%s): the password lasts until the next restart", strerror(errno)); rc = 1; }
    }
    memset(line, 0, sizeof line);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the throttle */

static struct { uint32_t ip; int fails; long first, blocked_until; } g_bad[32];

bool auth_blocked(uint32_t ip, long now) {
    if (g_opt.throttle_fails <= 0) return false;
    for (int i = 0; i < 32; i++) if (g_bad[i].ip == ip && g_bad[i].blocked_until > now) return true;
    return false;
}

void auth_failed(uint32_t ip, long now, const char *peer) {
    if (g_opt.throttle_fails <= 0) return;
    int slot = -1;
    for (int i = 0; i < 32; i++) if (g_bad[i].ip == ip && (g_bad[i].fails || g_bad[i].blocked_until)) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < 32 && slot < 0; i++) if (!g_bad[i].fails && g_bad[i].blocked_until <= now) slot = i;
        if (slot < 0) {                                         /* full: reuse the oldest that is not blocked now */
            long oldest = 0;
            for (int i = 0; i < 32; i++) if (g_bad[i].blocked_until <= now && (slot < 0 || g_bad[i].first < oldest)) { slot = i; oldest = g_bad[i].first; }
            if (slot < 0) slot = 0;
        }
        memset(&g_bad[slot], 0, sizeof g_bad[slot]);
        g_bad[slot].ip = ip;
    }
    if (now - g_bad[slot].first > THROTTLE_WINDOW_MS) { g_bad[slot].fails = 0; g_bad[slot].first = now; }
    if (++g_bad[slot].fails >= g_opt.throttle_fails) {
        g_bad[slot].blocked_until = now + THROTTLE_BLOCK_MS;
        g_bad[slot].fails = 0;
        g_bad[slot].first = now;
        vlog("%s: %d wrong passwords within a minute: Hatate looks away from that address for %d seconds", peer, g_opt.throttle_fails, THROTTLE_BLOCK_MS / 1000);
    }
}

void auth_succeeded(uint32_t ip) {
    for (int i = 0; i < 32; i++) if (g_bad[i].ip == ip) memset(&g_bad[i], 0, sizeof g_bad[i]);
}
