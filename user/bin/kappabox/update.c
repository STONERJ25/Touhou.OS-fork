/*
 * update: the receiving end of a pushed update, and the safety net around it (docs/UPDATE.md).
 *
 *   update recv [--reboot[=SECONDS]] [--loader] [--no-trial] [--replace-trial] [--force]   read a pushed update from standard input and put it on the ESP
 *   update status                                                                         what is installed and whether a trial is running
 *   update confirm [--anyway]                                                             this start works: end the trial
 *   update rollback [--anyway]                                                            put the previous kernel and initrd back for good
 *   update watchdog [SECONDS]                                                             (init starts it on a trial start) restart unless somebody confirms
 *
 * The push (tools/push-update.py makes it) is a header and the files' bytes on standard input, carried by ssh to `sudo update recv`:
 *
 *   touhou-push 1
 *   version VERSION
 *   file NAME SIZE SHA256            one line per file, NAME one of kernel.elf initrd.img cmdline.txt EFI/BOOT/BOOTX64.EFI
 *   end
 *   (the bytes of the files, in the order of the lines)
 *
 * Every file is checked against its hash before the ESP is touched, then written with the power-cut-safe sequence of espgrant.h. The kernel and
 * the initrd always go as a pair (a kernel only runs with the initrd of its own build); the previous pair is kept as kernel.old and initrd.old, read
 * back from the disk, and listed with its hashes in update.old. update.try is written before the first replacement (and removed after the last one when
 * the push is not on trial): the loader then counts the starts of the new files and, if they have not been confirmed after two, starts the old ones
 * (boot/update.c). A push never touches the kept set while an earlier push is still on trial (that set is the last one known to work). The loader
 * itself (BOOTX64.EFI) is only replaced when asked for, because a loader that does not work cannot fall back to anything.
 *
 * The work on the ESP is done by a process of its own, in a session of its own: if the ssh connection dies (and sshd hangs up and then shoots the
 * programs of the connection), the update is finished all the same, instead of being cut in the middle.
 */
#include "kappabox.h"
#include "espgrant.h"
#include <abi.h>
#include <crypto.h>
#include <stdarg.h>

#define TRY_NAME   "update.try"
#define MF_NAME    "update.mf"
#define OLD_RECORD "update.old"
#define CONFIRMED  "/tmp/.update-confirmed"
#define WRITTEN    "/tmp/.update-written"      /* root's file: this start has written to the ESP, so the files there are not the ones it runs */
#define MAX_FILES  4
#define TRIAL_SECONDS 300
#define MAX_CMDLINE 200                 /* the loader keeps 255 characters and adds a few words of its own */

typedef struct {
    const char *name;
    size_t max;
} Allowed;

static const Allowed ALLOWED[] = {
    { "kernel.elf", 64u << 20 }, { "initrd.img", 256u << 20 }, { "cmdline.txt", MAX_CMDLINE }, { "EFI/BOOT/BOOTX64.EFI", 4u << 20 },
};

typedef struct {
    const char *name;
    size_t size;
    uint8_t want[32], got[32];
    uint8_t *data;
} PFile;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("update: %s\n", line);
    fflush(stdout);
}

static void say_step(const char *step, const char *name) {
    if (!strcmp(step, "done")) return;                             /* (the tool says its own, with the result before it) */
    if (*name) say("%s %s", step, name);
    else say("%s", step);
}

static void to_hex(const uint8_t *h, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[h[i] >> 4]; out[2 * i + 1] = hex[h[i] & 15]; }
    out[64] = 0;
}

static int from_hex(const char *s, uint8_t out[32]) {
    if (strlen(s) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void sha256_of(const void *d, size_t n, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, d, n);
    sha256_final(&c, out);
}

static char *path_in(const EspSession *s, const char *name) { return kb_join(s->dir, name); }

static int exists_in(const EspSession *s, const char *name) {
    char *p = path_in(s, name);
    int r = access(p, F_OK) == 0;
    free(p);
    return r;
}

static void remove_in(const EspSession *s, const char *name) {
    char *p = path_in(s, name);
    unlink(p);
    free(p);
}

static int read_line(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (n + 1 >= cap) return -1;
        buf[n++] = c;
    }
    buf[n] = 0;
    return (int)n;
}

/* Why a file cannot be what its name says, or NULL. (The kept copies are looked at too: what a rollback installs must be as sound as what a push brings.) */
static const char *looks_wrong(const char *name, const uint8_t *d, size_t size) {
    if (!strcmp(name, "kernel.elf") || !strcmp(name, "kernel.old")) {
        if (size < 4096 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 2 || d[5] != 1 || d[18] != 62) return "is not an x86-64 ELF file";
    } else if (!strcmp(name, "initrd.img") || !strcmp(name, "initrd.old")) {
        if (size < 1024 || memcmp(d + 257, "ustar", 5)) return "is not a tar archive";
    } else if (!strcmp(name, "EFI/BOOT/BOOTX64.EFI")) {
        if (size < 1024 || d[0] != 'M' || d[1] != 'Z') return "is not a PE/COFF (EFI) program";
    } else if (!strcmp(name, "cmdline.txt") || !strcmp(name, "cmdline.old")) {
        if (!size) return "is empty";
        for (size_t i = 0; i < size; i++)
            if ((d[i] < 0x20 && d[i] != '\n' && d[i] != '\r' && d[i] != '\t') || d[i] >= 0x7f) return "has characters that do not belong in a command line";
        /* the loader puts its own words (update.loader, update.trial, update.fellback) in front: a file that carries them would only confuse the system */
        for (size_t i = 0; i + 7 < size; i++)
            if ((i == 0 || d[i - 1] == ' ' || d[i - 1] == '\n') && !memcmp(d + i, "update.", 7) && (!memcmp(d + i, "update.trial", 12) || !memcmp(d + i, "update.fellback", 15) ||
                                                                                                     !memcmp(d + i, "update.loader", 13)))
                return "carries words that only the loader may put on the command line (update.trial, update.fellback, update.loader)";
    }
    return NULL;
}

static PFile *find_file(PFile *files, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(files[i].name, name)) return &files[i];
    return NULL;
}

static int same_as_installed(const EspSession *s, const PFile *f) {
    size_t len = 0;
    uint8_t *cur = (uint8_t *)esp_read(s, f->name, &len);
    if (!cur) return 0;
    uint8_t h[32];
    sha256_of(cur, len, h);
    free(cur);
    return len == f->size && !memcmp(h, f->got, 32);
}

/* The command line the system was started with (/proc/cmdline): does it have the word, and what value. */
static int cmdline_word(const char *word, char *value, size_t cap) {
    char cl[512] = "";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) { ssize_t r = read(fd, cl, sizeof cl - 1); close(fd); if (r > 0) cl[r] = 0; }
    size_t wl = strlen(word);
    for (char *p = cl; *p;) {
        while (*p == ' ' || *p == '\n') p++;
        char *e = p;
        while (*e && *e != ' ' && *e != '\n') e++;
        if ((size_t)(e - p) >= wl && !strncmp(p, word, wl) && (word[wl - 1] == '=' || (size_t)(e - p) == wl)) {
            if (value) { size_t v = (size_t)(e - p) - wl; if (v >= cap) v = cap - 1; memcpy(value, p + wl, v); value[v] = 0; }
            return 1;
        }
        p = e;
    }
    return 0;
}

/* Has this start written to the ESP (a push)? Then what is on the ESP is not what this start runs, whatever its command line says (update.trial= describes the files it
 * was started with), and nothing has proved those files: `update status` does not call them "on trial" and `update confirm` refuses until the machine has been restarted.
 * (2026-10-25: a script confirmed a push on the start that had only received it; the files were installed without ever having run.) /tmp is everybody's, so only root's
 * file counts; it is empty at every start. */
static int written_here(void) {
    struct stat st;
    return stat(WRITTEN, &st) == 0 && st.st_uid == 0;
}

static void mark_written(void) {
    unlink(WRITTEN);
    int fd = open(WRITTEN, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) close(fd);
}

/* The detached restart: ssh gets its answer first, then the machine restarts. */
static void restart_later(int seconds) {
    pid_t pid = fork();
    if (pid != 0) return;
    setsid();
    for (int fd = 0; fd < 64; fd++) close(fd);
    open("/dev/null", O_RDWR); dup(0); dup(0);
    sleep((unsigned)seconds);
    sync();
    gap_reboot(REBOOT_RESTART);
    _exit(0);
}

/* The kept previous set is sound when update.old lists kernel.old and initrd.old (and cmdline.old, if there is one) and the files are what it says. */
static int check_old_set(const EspSession *s, char *why, size_t wn) {
    size_t rl = 0;
    char *rec = esp_read(s, OLD_RECORD, &rl);
    if (!rec) { snprintf(why, wn, "there is no %s on the ESP: no complete previous set was kept", OLD_RECORD); return -1; }
    int rc = 0, seen_k = 0, seen_i = 0, seen_c = 0;
    for (char *line = strtok(rec, "\n"); line && !rc; line = strtok(NULL, "\n")) {
        char name[40], hex[80];
        unsigned long size;
        uint8_t want[32];
        if (sscanf(line, "%39s %lu %79s", name, &size, hex) != 3 || from_hex(hex, want)) { snprintf(why, wn, "%s is damaged", OLD_RECORD); rc = -1; break; }
        size_t len = 0;
        uint8_t *d = (uint8_t *)esp_read(s, name, &len);
        uint8_t h[32];
        if (!d) { snprintf(why, wn, "%s is missing", name); rc = -1; break; }
        sha256_of(d, len, h);
        const char *bad = looks_wrong(name, d, len);
        if (len != size || memcmp(h, want, 32)) { snprintf(why, wn, "%s is not what %s says (the previous set is incomplete or damaged)", name, OLD_RECORD); rc = -1; }
        else if (bad) { snprintf(why, wn, "%s %s", name, bad); rc = -1; }
        free(d);
        if (!strcmp(name, "kernel.old")) seen_k = 1;
        else if (!strcmp(name, "initrd.old")) seen_i = 1;
        else if (!strcmp(name, "cmdline.old")) seen_c = 1;
    }
    free(rec);
    if (!rc && (!seen_k || !seen_i)) { snprintf(why, wn, "%s does not list both kernel.old and initrd.old", OLD_RECORD); rc = -1; }
    if (!rc && !seen_c && exists_in(s, "cmdline.old")) { snprintf(why, wn, "cmdline.old is not part of the kept set (%s does not list it)", OLD_RECORD); rc = -1; }
    return rc;
}

/* ------------------------------------------------------------------------------------------------ recv */

typedef struct { int reboot_in, allow_loader, trial, force, replace_trial; } RecvOpts;

static int recv_worker(PFile *files, int n, const char *version, const RecvOpts *o) {
    if (o->trial && !cmdline_word("update.loader=", NULL, 0)) {
        kb_error("this machine's loader does not count the starts of a trial (it is older than the update feature, or its word update.loader is missing from the command line "
                 "of this start): push with --no-trial, together with --loader if the loader is to be replaced as well");
        return 1;
    }
    EspSession s;
    char err[400];
    if (esp_open(&s, NULL, o->force, err, sizeof err)) {
        kb_error("%s%s", err, strstr(err, "not cleanly unmounted") ? " (`update recv --force` mounts it anyway)" : "");
        return 1;
    }
    say("the ESP (/dev/%s) is mounted read-write for this update", s.dev);
    int rc = 1, nlist = 0;
    EspFile list[MAX_FILES + 2];
    uint8_t *own_kernel = NULL, *own_initrd = NULL;
    char try_text[160], mf[1400];

    int trial_open = exists_in(&s, TRY_NAME);
    if (trial_open) {
        if (!o->replace_trial) {
            kb_error("an earlier update is still on trial (update.try is on the ESP): `update confirm` if it works, `update rollback` if it does not; or --replace-trial to put a "
                     "newer build over it (the previous set that is kept stays what it is)");
            goto out;
        }
        if (!o->trial) { kb_error("--replace-trial and --no-trial do not go together: an update that is not on trial must not end the trial of an earlier one"); goto out; }
        char why[200];
        if (check_old_set(&s, why, sizeof why)) { kb_error("--replace-trial needs the previous set to fall back to, and %s", why); goto out; }
        say("replacing the update that is on trial; the previous set that is kept stays as it is");
    }
    PFile *kern = find_file(files, n, "kernel.elf"), *rd = find_file(files, n, "initrd.img");
    PFile *cmd = find_file(files, n, "cmdline.txt"), *ldr = find_file(files, n, "EFI/BOOT/BOOTX64.EFI");
    int pair_same = kern && same_as_installed(&s, kern) && same_as_installed(&s, rd);
    int cmd_new = cmd && !same_as_installed(&s, cmd), ldr_new = ldr && !same_as_installed(&s, ldr);
    if (!((kern && !pair_same) || cmd_new || ldr_new)) {
        say("everything in the push is installed already: nothing to do");
        say("result: unchanged");
        rc = 0;
        goto out;
    }
    if (kern && !pair_same) {
        list[nlist++] = (EspFile){ kern->name, kern->data, kern->size };
        list[nlist++] = (EspFile){ rd->name, rd->data, rd->size };
    } else {                                              /* the pair stays, but the previous set to fall back to must be today's: copy it */
        size_t kl = 0, il = 0;
        own_kernel = (uint8_t *)esp_read(&s, "kernel.elf", &kl);
        own_initrd = (uint8_t *)esp_read(&s, "initrd.img", &il);
        if (!own_kernel || !own_initrd) { kb_error("the ESP has no kernel.elf and initrd.img of its own to keep as the previous set"); goto out; }
        list[nlist++] = (EspFile){ "kernel.elf", own_kernel, kl };
        list[nlist++] = (EspFile){ "initrd.img", own_initrd, il };
    }
    if (cmd_new) list[nlist++] = (EspFile){ cmd->name, cmd->data, cmd->size };
    else if (!trial_open && exists_in(&s, "cmdline.old")) {   /* cmdline.old only exists for an update that changed the command line */
        remove_in(&s, "cmdline.old");
        sync();
    }
    if (ldr_new) list[nlist++] = (EspFile){ ldr->name, ldr->data, ldr->size };

    snprintf(try_text, sizeof try_text, "version %s\nissued %lld\n", version, (long long)time(NULL));
    s.arm = (EspFile){ TRY_NAME, try_text, strlen(try_text) };   /* always: the marker guards the renames; a push that is not on trial removes it afterwards */
    s.disarm = !o->trial;
    s.record = trial_open ? NULL : OLD_RECORD;
    say("writing %d file%s%s", nlist, nlist == 1 ? "" : "s", o->trial ? " (on trial)" : " (not on trial)");
    mark_written();                                       /* from here on the ESP is not what this start runs (also if the write stops half-way: the safe side) */
    if (esp_update(&s, list, nlist, !trial_open, say_step, err, sizeof err)) {
        kb_error("%s", err);
        if (s.armed) kb_error("the replacement had begun: the ESP holds a mix of old and new files and update.try is armed, so the machine restarts on trial; push again");
        goto out;
    }
    /* what is installed now, for `update status` (informational: the safety is update.try) */
    {
        size_t m = (size_t)snprintf(mf, sizeof mf, "touhou-push 1\nversion %s\nissued %lld\n", version, (long long)time(NULL));
        for (int i = 0; i < n && m + 120 < sizeof mf; i++) {
            char hex[65];
            to_hex(files[i].got, hex);
            m += (size_t)snprintf(mf + m, sizeof mf - m, "file %s %zu %s\n", files[i].name, files[i].size, hex);
        }
        s.arm = (EspFile){ NULL, NULL, 0 };
        s.disarm = 0;
        s.record = NULL;
        EspFile one = { MF_NAME, mf, m };
        if (esp_update(&s, &one, 1, 0, NULL, err, sizeof err)) kb_error("(the record of this update could not be written: %s)", err);
    }
    rc = 0;
out:
    free(own_kernel);
    free(own_initrd);
    if (esp_close(&s)) { kb_error("cannot unmount %s: it stays mounted read-only", s.dir); rc = 1; }
    if (rc == 0 && nlist) {
        say("result: installed");
        if (o->trial) say("done. The new files are on trial: confirm them (update confirm) within %d minutes of the restart, or the machine restarts again and,"
                          " after the second try, starts the previous kernel and initrd", TRIAL_SECONDS / 60);
        else say("done");
        if (o->reboot_in >= 0) {
            say("restarting in %d second%s", o->reboot_in, o->reboot_in == 1 ? "" : "s");
            restart_later(o->reboot_in);
        }
    } else if (rc == 0) {
        say("done");
    }
    return rc;
}

static int cmd_recv(int argc, char **argv) {
    RecvOpts o = { .reboot_in = -1, .trial = 1 };
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--reboot")) o.reboot_in = 3;
        else if (!strncmp(argv[i], "--reboot=", 9)) o.reboot_in = atoi(argv[i] + 9);
        else if (!strcmp(argv[i], "--loader")) o.allow_loader = 1;
        else if (!strcmp(argv[i], "--no-trial")) o.trial = 0;
        else if (!strcmp(argv[i], "--replace-trial")) o.replace_trial = 1;
        else if (!strcmp(argv[i], "--force")) o.force = 1;
        else { kb_error("usage: update recv [--reboot[=SECONDS]] [--loader] [--no-trial] [--replace-trial] [--force]"); return 2; }
    }
    if (geteuid() != 0) { kb_error("only root may update the system (use sudo)"); return 1; }

    /* the header */
    char line[256], version[80] = "";
    if (read_line(line, sizeof line) < 0 || strcmp(line, "touhou-push 1")) { kb_error("the input is not a push (first line 'touhou-push 1')"); return 2; }
    if (read_line(line, sizeof line) < 0 || strncmp(line, "version ", 8) || strlen(line + 8) < 1 || strlen(line + 8) > 64) { kb_error("bad version line"); return 2; }
    for (const char *p = line + 8; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._+-", *p)) { kb_error("bad version line"); return 2; }
    snprintf(version, sizeof version, "%s", line + 8);
    PFile files[MAX_FILES];
    int n = 0;
    for (;;) {
        if (read_line(line, sizeof line) < 0) { kb_error("the header ends too early"); return 2; }
        if (!strcmp(line, "end")) break;
        char name[40], hex[80], junk;
        unsigned long size;
        if (n == MAX_FILES || sscanf(line, "file %39s %lu %79s %c", name, &size, hex, &junk) != 3) { kb_error("bad file line: %.60s", line); return 2; }
        const Allowed *a = NULL;
        for (size_t k = 0; k < sizeof ALLOWED / sizeof ALLOWED[0]; k++)
            if (!strcmp(name, ALLOWED[k].name)) a = &ALLOWED[k];
        if (!a) { kb_error("%s is not a file an update may carry", name); return 2; }
        if (find_file(files, n, a->name)) { kb_error("%s is listed twice", name); return 2; }
        if (size == 0 || size > a->max) { kb_error("%s: %lu bytes is not a size it may have (at most %zu)", name, size, a->max); return 2; }
        if (!strcmp(name, "EFI/BOOT/BOOTX64.EFI") && !o.allow_loader) {
            kb_error("the push carries the loader (EFI/BOOT/BOOTX64.EFI), which is only replaced with --loader: a loader that does not work cannot fall back");
            return 2;
        }
        files[n].name = a->name;
        files[n].size = size;
        files[n].data = NULL;
        if (from_hex(hex, files[n].want)) { kb_error("%s: the hash is not 64 hex digits", name); return 2; }
        n++;
    }
    if (n == 0) { kb_error("the push carries no files"); return 2; }
    PFile *kern = find_file(files, n, "kernel.elf"), *rd = find_file(files, n, "initrd.img");
    if (!kern != !rd) { kb_error("the kernel and the initrd go together (a kernel only runs with the initrd of its own build)"); return 2; }

    /* the bytes, each checked against its hash before anything else happens */
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        PFile *f = &files[i];
        f->data = malloc(f->size);
        if (!f->data) { kb_error("out of memory for %s (%zu bytes)", f->name, f->size); return 1; }
        sha256_ctx h;
        sha256_init(&h);
        size_t got = 0;
        while (got < f->size) {
            ssize_t r = read(0, f->data + got, f->size - got > 65536 ? 65536 : f->size - got);
            if (r <= 0) { kb_error("the input ended after %zu of %zu bytes of %s", got, f->size, f->name); return 2; }
            sha256_update(&h, f->data + got, (size_t)r);
            got += (size_t)r;
        }
        sha256_final(&h, f->got);
        if (memcmp(f->got, f->want, 32)) { kb_error("%s arrived damaged (its hash is not the one announced): nothing was changed", f->name); return 2; }
        const char *bad = looks_wrong(f->name, f->data, f->size);
        if (bad) { kb_error("%s %s: nothing was changed", f->name, bad); return 2; }
        total += f->size;
        say("received %s (%zu bytes, hash ok)", f->name, f->size);
    }
    say("version %s: %d file%s, %zu bytes, all hashes match", version, n, n == 1 ? "" : "s", total);

    /* From here on nothing may cut the work in the middle: it goes on in a process of its own, in a session of its own. If the ssh connection dies, sshd hangs up the
     * programs of the connection and shoots them a second later; this one is no longer among them. */
    pid_t pid = fork();
    if (pid < 0) { kb_error("cannot fork: %s", strerror(errno)); return 1; }
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    setsid();
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    _exit(recv_worker(files, n, version, &o));
}

/* ------------------------------------------------------------------------------------------------ status, confirm, rollback */

static int cmd_status(void) {
    char val[16];
    int trial = cmdline_word("update.trial=", val, sizeof val), fell = cmdline_word("update.fellback", NULL, 0), written = written_here();
    if (fell) say("this start: the PREVIOUS kernel and initrd (the last update did not come up in time); `update rollback` makes that permanent");
    else if (written) say("this start: not the files that are on the ESP now (an update was written after it began and has not run yet): restart to run them");
    else if (trial) say("this start: the new files, on trial (start %s of 2); `update confirm` if they work", val);
    else say("this start: the installed files");
    if (fell && written) say("an update was written to the ESP after this start began; it has not run yet");
    say("loader: %s", cmdline_word("update.loader=", NULL, 0) ? "counts the starts of a trial (update.loader)" : "does NOT count trial starts: push with --no-trial");
    if (geteuid() != 0) { say("(run as root, with sudo, to see what the ESP holds)"); return 0; }
    char dev[32], why[200];
    if (esp_find(dev, sizeof dev, why, sizeof why)) { say("the ESP is unknown: %s", why); return 0; }
    char dir[64];
    snprintf(dir, sizeof dir, "/BambooForest/update.%d", (int)getpid());
    mkdir(dir, 0700);
    char src[48];
    snprintf(src, sizeof src, "/dev/%s", dev);
    if (gap_mount_opts(src, dir, "fat32", MNT_RDONLY, "ro") < 0) { say("cannot read the ESP (%s): %s", src, strerror(errno)); rmdir(dir); return 0; }
    EspSession s;
    memset(&s, 0, sizeof s);
    snprintf(s.dir, sizeof s.dir, "%s", dir);
    size_t len = 0;
    char *mf = esp_read(&s, MF_NAME, &len);
    if (mf) {
        char version[80] = "?";
        long long issued = 0;
        sscanf(mf, "touhou-push 1\nversion %79s\nissued %lld", version, &issued);
        time_t t = (time_t)issued;
        struct tm tmv;
        gmtime_r(&t, &tmv);
        char when[40];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tmv);
        say("installed by the last push: version %s, %s", version, when);
        free(mf);
    } else {
        say("no push has been installed yet (no %s on the ESP)", MF_NAME);
    }
    say("update.try: %s", exists_in(&s, TRY_NAME) ? "present: the files are on trial until they are confirmed" : "absent: nothing is on trial");
    char bad[200];
    if (!exists_in(&s, "kernel.old") && !exists_in(&s, "initrd.old")) say("previous kernel and initrd kept: no");
    else if (check_old_set(&s, bad, sizeof bad) == 0) say("previous kernel and initrd kept: yes, complete (%s%s)", "kernel.old initrd.old", exists_in(&s, "cmdline.old") ? " cmdline.old" : "");
    else say("previous kernel and initrd kept: INCOMPLETE: %s", bad);
    sync();
    gap_umount(dir);
    rmdir(dir);
    return 0;
}

/* Tells the watchdog of this start to stand down. /tmp is everybody's: a file somebody else made there would not count (the watchdog wants root's), so any file is
 * removed first and this one is made exclusively. */
static int mark_confirmed(void) {
    unlink(CONFIRMED);
    return open(CONFIRMED, O_WRONLY | O_CREAT | O_EXCL, 0644);
}

/* confirm and rollback open the ESP with force: after a power cut during the update the file system is flagged dirty, and a system that works must still be
 * able to say so (and a system that does not must still be able to go back). They change one directory entry or rewrite whole files through the safe sequence. */
static int cmd_confirm(int argc, char **argv) {
    int anyway = argc > 2 && !strcmp(argv[2], "--anyway");
    if (argc > 2 && !anyway) { kb_error("usage: update confirm [--anyway]"); return 2; }
    if (geteuid() != 0) { kb_error("only root may confirm (use sudo)"); return 1; }
    int fell = cmdline_word("update.fellback", NULL, 0), trial = cmdline_word("update.trial=", NULL, 0);
    if (fell && !anyway) {
        kb_error("this start runs the PREVIOUS files, because the new ones did not come up: confirming now would make the files that failed the installed ones. "
                 "`update rollback` keeps the previous files; `update confirm --anyway` really means it");
        return 1;
    }
    EspSession s;
    char err[400];
    if (esp_open(&s, NULL, 1, err, sizeof err)) { kb_error("%s", err); return 1; }
    int had = exists_in(&s, TRY_NAME), rc = 0;
    if (had && !trial && !anyway) {
        kb_error("these files are on trial, but this start is not the trial start (the machine has not been restarted since they were written): nothing has proved them. "
                 "Restart first; `update confirm --anyway` really means it");
        esp_close(&s);
        return 1;
    }
    if (had && written_here() && !anyway) {                  /* (a trial start that received another push: the command line describes the files it started with) */
        kb_error("an update was written to the ESP after this start began: the files on trial have not run yet, so nothing has proved them. "
                 "Restart first; `update confirm --anyway` really means it");
        esp_close(&s);
        return 1;
    }
    if (had) {
        remove_in(&s, TRY_NAME);
        sync();
        if (exists_in(&s, TRY_NAME)) { kb_error("cannot delete %s", TRY_NAME); rc = 1; }
    }
    if (esp_close(&s)) { kb_error("cannot unmount %s: it stays mounted read-only", s.dir); rc = 1; }
    if (rc) return rc;
    int fd = mark_confirmed();       /* tells the watchdog of this start to stand down (root's file: the watchdog checks the owner) */
    if (fd >= 0) close(fd);
    say(had ? "confirmed: the new files are the installed system from now on" : "nothing was on trial; this start is marked as good");
    return 0;
}

static int cmd_rollback(int argc, char **argv) {
    (void)argv;
    if (argc > 2) { kb_error("usage: update rollback"); return 2; }
    if (geteuid() != 0) { kb_error("only root may roll back (use sudo)"); return 1; }
    EspSession s;
    char err[400];
    if (esp_open(&s, NULL, 1, err, sizeof err)) { kb_error("%s", err); return 1; }
    int rc = 1;
    char why[200];
    size_t kl = 0, il = 0, cl = 0;
    uint8_t *k = NULL, *i = NULL, *c = NULL;
    if (check_old_set(&s, why, sizeof why)) { kb_error("cannot roll back: %s", why); goto out; }
    k = (uint8_t *)esp_read(&s, "kernel.old", &kl);
    i = (uint8_t *)esp_read(&s, "initrd.old", &il);
    c = (uint8_t *)esp_read(&s, "cmdline.old", &cl);
    if (!k || !i) { kb_error("cannot read the previous kernel and initrd"); goto out; }
    {
        char try_text[64];
        snprintf(try_text, sizeof try_text, "rollback\nissued %lld\n", (long long)time(NULL));
        s.arm = (EspFile){ TRY_NAME, try_text, strlen(try_text) };       /* the marker guards the renames, and ends up removed: the trial is over either way */
        s.disarm = 1;
        EspFile list[3] = { { "kernel.elf", k, kl }, { "initrd.img", i, il }, { "cmdline.txt", c, cl } };
        if (esp_update(&s, list, c ? 3 : 2, 0, say_step, err, sizeof err)) { kb_error("%s", err); goto out; }
    }
    remove_in(&s, TRY_NAME);                                  /* (also when an earlier trial's marker was there and the arm file replaced it) */
    sync();
    rc = 0;
out:
    free(k); free(i); free(c);
    if (esp_close(&s)) { kb_error("cannot unmount %s: it stays mounted read-only", s.dir); rc = 1; }
    if (rc == 0) {
        int fd = mark_confirmed();
        if (fd >= 0) close(fd);
        say("the previous kernel and initrd are the installed ones again; restart to use them");
    }
    return rc;
}

static int cmd_watchdog(int argc, char **argv) {
    int seconds = argc > 2 ? atoi(argv[2]) : TRIAL_SECONDS;
    if (seconds < 10) seconds = TRIAL_SECONDS;
    gap_setname("update");
    for (int waited = 0; waited < seconds + 2; waited += 2) {
        struct stat st;
        if (stat(CONFIRMED, &st) == 0 && st.st_uid == 0) return 0;      /* only root's file counts: /tmp is everybody's */
        if (waited < seconds) sleep(2);
    }
    gap_log("the new files were not confirmed in time: restarting (after the second unconfirmed start the loader brings back the previous kernel)");
    sync();
    gap_reboot(REBOOT_RESTART);
    return 0;
}

APPLET(update) {
    signal(SIGPIPE, SIG_IGN);                  /* `update status | head`: a closed pipe must not skip the umount (the ESP stayed mounted read-only on the Dell and the next status said "busy") */
    if (argc >= 2 && !strcmp(argv[1], "recv")) return cmd_recv(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "status")) return cmd_status();
    if (argc >= 2 && !strcmp(argv[1], "confirm")) return cmd_confirm(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "rollback")) return cmd_rollback(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "watchdog")) return cmd_watchdog(argc, argv);
    kb_error("usage: update recv [--reboot[=SECONDS]] [--loader] [--no-trial] [--replace-trial] [--force]\n"
             "       update status | confirm [--anyway] | rollback | watchdog [SECONDS]");
    return 2;
}
