/* Hardware-facing tools: dd lsblk lspci lsusb. */
#include "kappabox.h"

static unsigned long long parse_size(const char *s) {
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    switch (*end) {
    case 'b': v *= 512; break;
    case 'k': case 'K': v *= 1024; break;
    case 'M': v *= 1024ull * 1024; break;
    case 'G': v *= 1024ull * 1024 * 1024; break;
    case 'w': v *= 2; break;
    }
    return v;
}

APPLET(dd) {
    const char *in = NULL, *out = NULL;
    unsigned long long bs = 512, count = ~0ull, skip = 0, seek = 0;
    int notrunc = 0, quiet = 0, sync_pad = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { kb_error("unrecognized operand '%s'", argv[i]); return 1; }
        *eq = 0;
        const char *k = argv[i], *v = eq + 1;
        if (!strcmp(k, "if")) in = v;
        else if (!strcmp(k, "of")) out = v;
        else if (!strcmp(k, "bs") || !strcmp(k, "ibs") || !strcmp(k, "obs")) bs = parse_size(v);
        else if (!strcmp(k, "count")) count = strtoull(v, NULL, 10);
        else if (!strcmp(k, "skip")) skip = parse_size(v);
        else if (!strcmp(k, "seek")) seek = parse_size(v);
        else if (!strcmp(k, "status")) quiet = !strcmp(v, "none");
        else if (!strcmp(k, "conv")) { if (strstr(v, "notrunc")) notrunc = 1; if (strstr(v, "sync")) sync_pad = 1; }
        else { kb_error("unrecognized operand '%s=%s'", k, v); return 1; }
    }
    if (!bs) bs = 512;
    int ifd = in ? open(in, O_RDONLY) : 0;
    if (ifd < 0) { kb_error("failed to open '%s': %s", in, strerror(errno)); return 1; }
    int ofd = 1;
    if (out) {
        ofd = open(out, O_WRONLY | O_CREAT | (notrunc || seek ? 0 : O_TRUNC), 0644);
        if (ofd < 0) { kb_error("failed to open '%s': %s", out, strerror(errno)); return 1; }
    }
    char *buf = malloc((size_t)bs);
    if (!buf) kb_die("out of memory");
    if (skip) {
        if (lseek(ifd, (off_t)(skip * bs), SEEK_SET) < 0) {
            for (unsigned long long i = 0; i < skip; i++) if (read(ifd, buf, (size_t)bs) <= 0) break;
        }
    }
    if (seek) lseek(ofd, (off_t)(seek * bs), SEEK_SET);
    unsigned long long full_in = 0, part_in = 0, full_out = 0, part_out = 0, bytes = 0;
    long t0 = gap_time_ms();
    while (count--) {
        ssize_t n = read(ifd, buf, (size_t)bs);
        if (n < 0) { kb_error("error reading '%s': %s", in ? in : "stdin", strerror(errno)); break; }
        if (n == 0) break;
        if ((unsigned long long)n == bs) full_in++; else part_in++;
        size_t put = (size_t)n;
        if (sync_pad && (unsigned long long)n < bs) { memset(buf + n, 0, (size_t)bs - (size_t)n); put = (size_t)bs; }
        ssize_t off = 0;
        while ((size_t)off < put) {
            ssize_t w = write(ofd, buf + off, put - (size_t)off);
            if (w < 0) { kb_error("error writing '%s': %s", out ? out : "stdout", strerror(errno)); goto done; }
            off += w;
        }
        if (put == bs) full_out++; else part_out++;
        bytes += put;
    }
done:
    sync();
    if (!quiet) {
        long ms = gap_time_ms() - t0;
        if (ms < 1) ms = 1;
        fprintf(stderr, "%llu+%llu records in\n%llu+%llu records out\n%llu bytes copied, %.2f s, %.1f MB/s\n", full_in, part_in, full_out, part_out,
                bytes, (double)ms / 1000.0, (double)bytes / 1048576.0 / ((double)ms / 1000.0));
    }
    free(buf);
    return 0;
}

APPLET(lsblk) {
    FILE *f = fopen("/proc/partitions", "r");
    if (!f) { kb_error("/proc/partitions: %s", strerror(errno)); return 1; }
    char line[256];
    int any = 0;
    while (fgets(line, sizeof line, f)) {
        char name[32], parent[32], label[48], driver[24], model[64] = "";
        unsigned long long size, start;
        unsigned ss;
        int ro;
        if (sscanf(line, "%31s %llu %u %d %31s %llu %47s %23s %63[^\n]", name, &size, &ss, &ro, parent, &start, label, driver, model) < 8) continue;
        if (!any) { printf("%-12s %9s %-3s %-8s %-16s %-10s %s\n", "NAME", "SIZE", "RO", "TYPE", "LABEL", "DRIVER", "MODEL"); any = 1; }
        char sz[16];
        kb_format_size(size, sz, sizeof sz);
        printf("%-12s %9s %-3d %-8s %-16s %-10s %s\n", name, sz, ro, strcmp(parent, "-") ? "part" : "disk", strcmp(label, "-") ? label : "", driver,
               strcmp(model, "-") ? model : "");
    }
    fclose(f);
    if (!any) puts("no block devices");
    return 0;
}

APPLET(lspci) {
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    FILE *f = fopen("/proc/pci", "r");
    if (!f) { kb_error("/proc/pci: %s", strerror(errno)); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) if (verbose || isxdigit((unsigned char)line[0])) fputs(line, stdout);
    fclose(f);
    return 0;
}

APPLET(lsusb) {
    FILE *f = fopen("/proc/usb", "r");
    if (!f) { puts("no USB subsystem (no /proc/usb)"); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
    return 0;
}
