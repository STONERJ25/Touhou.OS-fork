/* kshell: a tiny in-kernel debug shell on the console tty. The real shell is gapsh in user space;
 * this one is for bring-up and for when user space is broken (boot with "init=kshell"). */
#include "kernel.h"
#include "vfs.h"
#include "tty.h"
#include "task.h"
#include "mm.h"
#include "ktime.h"
#include "cpuinfo.h"
#include "arch.h"
#include "cpu.h"
#include "irq.h"

void machine_poweroff(void);
void machine_reboot(void);

#define MAXARGS 16

static int tokenize(char *s, char **argv) {
    int n = 0;
    while (*s && n < MAXARGS - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        argv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    argv[n] = NULL;
    return n;
}

static void mode_string(u32 mode, char *out) {
    static const char types[] = { [S_IFDIR >> 12] = 'd', [S_IFREG >> 12] = '-', [S_IFLNK >> 12] = 'l',
                                  [S_IFCHR >> 12] = 'c', [S_IFBLK >> 12] = 'b', [S_IFIFO >> 12] = 'p' };
    out[0] = types[(mode & S_IFMT) >> 12] ? types[(mode & S_IFMT) >> 12] : '?';
    for (int i = 0; i < 9; i++) out[1 + i] = (mode & (0400 >> i)) ? "rwx"[i % 3] : '-';
    out[10] = 0;
}

static void cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/";
    File *f;
    int rc = vfs_open(path, O_RDONLY, 0, NULL, NULL, &f);
    if (rc) { printk("ls: %s: error %d\n", path, rc); return; }
    struct gdirent de;
    while (vfs_readdir(f, &de) > 0) {
        char full[PATH_MAX], ms[11];
        struct gstat st;
        snprintf(full, sizeof full, "%s/%s", path, de.d_name);
        if (vfs_stat(full, NULL, NULL, LOOKUP_NOFOLLOW, &st)) memset(&st, 0, sizeof st);
        mode_string(st.st_mode, ms);
        printk("%s %4u %4u %8ld  %s%s\n", ms, st.st_uid, st.st_gid, (long)st.st_size, de.d_name,
               S_ISDIR(st.st_mode) ? "/" : S_ISLNK(st.st_mode) ? "@" : "");
    }
    file_put(f);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { printk("usage: cat FILE\n"); return; }
    void *data;
    size_t size;
    int rc = vfs_read_file(argv[1], &data, &size);
    if (rc) { printk("cat: %s: error %d\n", argv[1], rc); return; }
    console_write(data, size);
    kfree(data);
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) return;
    void *data;
    size_t size;
    if (vfs_read_file(argv[1], &data, &size)) { printk("hexdump: cannot read\n"); return; }
    const u8 *p = data;
    for (size_t i = 0; i < size && i < 256; i += 16) {
        printk("%04lx  ", (unsigned long)i);
        for (size_t j = 0; j < 16; j++) { if (i + j < size) printk("%02x ", p[i + j]); else printk("   "); }
        for (size_t j = 0; j < 16 && i + j < size; j++) printk("%c", p[i + j] >= 32 && p[i + j] < 127 ? p[i + j] : '.');
        printk("\n");
    }
    kfree(data);
}

static void ps_line(Task *t, void *ctx) {
    (void)ctx;
    static const char *const st[] = { "ready", "running", "blocked", "sleeping", "zombie" };
    printk("%4d  %-9s %-16s %lu ticks\n", t->tid, st[t->state], t->name, (unsigned long)t->cpu_ticks);
}

static void cmd_date(void) {
    int y, mo, d, h, mi, s, wd;
    civil_from_epoch(time_now_sec(), &y, &mo, &d, &h, &mi, &s, &wd);
    static const char *const wds[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    printk("%s %04d-%02d-%02d %02d:%02d:%02d UTC\n", wds[wd], y, mo, d, h, mi, s);
}

static void run_line(char *line) {
    char *argv[MAXARGS];
    int argc = tokenize(line, argv);
    if (!argc) return;
    const char *c = argv[0];

    if (!strcmp(c, "help")) {
        printk("kshell commands: ls cat hexdump echo write mkdir rmdir rm mv ln stat touch ps mem uptime date mounts\n"
               "                 clear crash selftest reboot poweroff exit\n");
    } else if (!strcmp(c, "ls")) cmd_ls(argc, argv);
    else if (!strcmp(c, "cat")) cmd_cat(argc, argv);
    else if (!strcmp(c, "hexdump")) cmd_hexdump(argc, argv);
    else if (!strcmp(c, "echo")) {
        for (int i = 1; i < argc; i++) printk("%s%s", argv[i], i + 1 < argc ? " " : "");
        printk("\n");
    } else if (!strcmp(c, "write") && argc >= 3) {
        char buf[256] = "";
        for (int i = 2; i < argc; i++) { strlcat(buf, argv[i], sizeof buf); if (i + 1 < argc) strlcat(buf, " ", sizeof buf); }
        strlcat(buf, "\n", sizeof buf);
        int rc = vfs_write_file(argv[1], buf, strlen(buf), 0644);
        if (rc) printk("write: error %d\n", rc);
    } else if (!strcmp(c, "mkdir") && argc >= 2) { int rc = vfs_mkdir(argv[1], 0755, NULL, NULL); if (rc) printk("mkdir: error %d\n", rc); }
    else if (!strcmp(c, "rmdir") && argc >= 2) { int rc = vfs_rmdir(argv[1], NULL, NULL); if (rc) printk("rmdir: error %d\n", rc); }
    else if (!strcmp(c, "rm") && argc >= 2) { int rc = vfs_unlink(argv[1], NULL, NULL); if (rc) printk("rm: error %d\n", rc); }
    else if (!strcmp(c, "mv") && argc >= 3) { int rc = vfs_rename(argv[1], argv[2], NULL, NULL); if (rc) printk("mv: error %d\n", rc); }
    else if (!strcmp(c, "ln") && argc >= 3) { int rc = vfs_symlink(argv[1], argv[2], NULL, NULL); if (rc) printk("ln: error %d\n", rc); }
    else if (!strcmp(c, "touch") && argc >= 2) {
        File *f;
        int rc = vfs_open(argv[1], O_WRONLY | O_CREAT, 0644, NULL, NULL, &f);
        if (rc) printk("touch: error %d\n", rc); else file_put(f);
    } else if (!strcmp(c, "stat") && argc >= 2) {
        struct gstat st;
        int rc = vfs_stat(argv[1], NULL, NULL, LOOKUP_NOFOLLOW, &st);
        if (rc) printk("stat: error %d\n", rc);
        else printk("mode %o uid %u gid %u size %ld nlink %u ino %lu dev %lu\n", st.st_mode, st.st_uid, st.st_gid,
                    (long)st.st_size, st.st_nlink, (unsigned long)st.st_ino, (unsigned long)st.st_dev);
    } else if (!strcmp(c, "ps")) { task_for_each(ps_line, NULL); }
    else if (!strcmp(c, "mem")) {
        printk("RAM %lu MiB, free %lu MiB, heap %lu KiB\n", (unsigned long)(pmm_total_pages() >> 8),
               (unsigned long)(pmm_free_pages_count() >> 8), (unsigned long)(heap_bytes_in_use() >> 10));
    } else if (!strcmp(c, "uptime")) printk("%lu ms\n", (unsigned long)uptime_ms());
    else if (!strcmp(c, "date")) cmd_date();
    else if (!strcmp(c, "mounts")) {
        char t[64], y[16], s[64];
        for (int i = 0; vfs_mount_list(i, t, sizeof t, y, sizeof y, s, sizeof s); i++) printk("%-12s %-8s on %s\n", s, y, t);
    } else if (!strcmp(c, "clear")) printk("\x1b[2J\x1b[H");
    else if (!strcmp(c, "selftest")) selftest_run();
    else if (!strcmp(c, "reboot")) { vfs_sync_all(); machine_reboot(); }
    else if (!strcmp(c, "poweroff")) { vfs_sync_all(); machine_poweroff(); }
    else if (!strcmp(c, "exit") && argc >= 2) qemu_exit((int)(argv[1][0] - '0'));
    else if (!strcmp(c, "crash") && argc >= 2) {
        if (!strcmp(argv[1], "pf")) *(volatile u64 *)0xdead0000UL = 1;
        else if (!strcmp(argv[1], "panic")) panic("requested from kshell");
        else printk("crash pf|panic\n");
    } else printk("kshell: unknown command '%s' (try help)\n", c);
}

void kshell_main(void *arg) {
    (void)arg;
    char line[256];
    printk("\n\x1b[1;36mkshell\x1b[0m: type 'help'. (the real shell is gapsh)\n");
    for (;;) {
        printk("\x1b[1;35mkshell\x1b[0m:\x1b[1;34m#\x1b[0m ");
        ssize_t n = tty_read(tty_console(), line, sizeof line - 1, false);
        if (n <= 0) { task_sleep_ms(50); continue; }
        line[n] = 0;
        if (line[n - 1] == '\n') line[n - 1] = 0;
        run_line(line);
    }
}
