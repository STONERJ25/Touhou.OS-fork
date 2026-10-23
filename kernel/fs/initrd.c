/* Unpacks the ustar initrd the loader handed over into the (tmpfs) root filesystem. */
#include "vfs.h"
#include "kernel.h"

static u64 parse_octal(const char *s, size_t n) {
    u64 v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (u64)(s[i] - '0');
    return v;
}

static void parent_of(const char *path, char *out, size_t n) {
    strlcpy(out, path, n);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) *slash = 0;
    else strlcpy(out, "/", n);
}

void initrd_unpack(const void *tar, size_t size) {
    const u8 *base = tar;
    size_t off = 0;
    int files = 0, dirs = 0, links = 0;
    u64 bytes = 0;

    while (off + 512 <= size) {
        const char *h = (const char *)(base + off);
        if (h[0] == 0) break;                           /* end-of-archive marker */
        char name[300];
        char prefix[156];
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        char shortname[101];
        memcpy(shortname, h, 100);
        shortname[100] = 0;
        if (prefix[0]) snprintf(name, sizeof name, "%s/%s", prefix, shortname);
        else strlcpy(name, shortname, sizeof name);

        u64 fsize = parse_octal(h + 124, 12);
        u32 mode = (u32)parse_octal(h + 100, 8);
        char type = h[156];
        const u8 *data = base + off + 512;
        off += 512 + ALIGN_UP(fsize, 512);
        if (off > size + 512) { klog("Keine", "initrd: truncated archive"); break; }

        char *n = name;
        while (n[0] == '.' && n[1] == '/') n += 2;
        while (*n == '/') n++;
        size_t len = strlen(n);
        while (len && n[len - 1] == '/') n[--len] = 0;
        if (!*n || !strcmp(n, ".")) continue;

        char path[320];
        snprintf(path, sizeof path, "/%s", n);

        if (type == '5') {
            vfs_mkdir_p(path, mode & 0777);
            vfs_chmod(path, mode & 07777, NULL, NULL);
            dirs++;
        } else if (type == '0' || type == 0) {
            char parent[320];
            parent_of(path, parent, sizeof parent);
            vfs_mkdir_p(parent, 0755);
            int rc = vfs_write_file(path, data, (size_t)fsize, mode & 07777);
            if (rc) klog("Keine", "initrd: cannot write %s (%d)", path, rc);
            files++;
            bytes += fsize;
        } else if (type == '2') {
            char link[101];
            memcpy(link, h + 157, 100);
            link[100] = 0;
            char parent[320];
            parent_of(path, parent, sizeof parent);
            vfs_mkdir_p(parent, 0755);
            vfs_symlink(link, path, NULL, NULL);
            links++;
        }
    }
    klog("Keine", "initrd unpacked: %d files (%lu KiB), %d directories, %d symlinks", files,
         (unsigned long)(bytes >> 10), dirs, links);
}
