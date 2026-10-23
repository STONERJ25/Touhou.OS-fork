/* The Hardware tab: processor, memory, display, PCI devices, block devices, USB devices and mounted volumes. */
#include "spell.h"
#include <fcntl.h>
#include <gensokyo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static Doc g_doc;
static uint64_t g_refreshed_ms;

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'); ) s[--n] = 0;
}

/* value of "key : value" in /proc/cpuinfo style text */
static bool field(const char *buf, const char *key, char *out, size_t cap) {
    const char *p = strstr(buf, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    trim(out);
    return out[0] != 0;
}

static void section_cpu(void) {
    doc_head(&g_doc, "Processor");
    char buf[1024], v[96];
    struct gsysinfo si;
    bool have = gap_sysinfo(&si) == 0;
    if (read_file("/proc/cpuinfo", buf, sizeof buf) > 0) {
        if (field(buf, "model name", v, sizeof v)) doc_kv(&g_doc, "Model", "%s", v);
        if (field(buf, "vendor_id", v, sizeof v)) doc_kv(&g_doc, "Vendor", "%s", v);
        char fam[16] = "?", mod[16] = "?", step[16] = "?";
        field(buf, "family", fam, sizeof fam);
        field(buf, "model     ", mod, sizeof mod);
        field(buf, "stepping", step, sizeof step);
        doc_kv(&g_doc, "Family / model / stepping", "%s / %s / %s", fam, mod, step);
    } else if (have) doc_kv(&g_doc, "Model", "%s", si.cpu_brand);
    else doc_note(&g_doc, "No processor information is available.");
    if (have && si.cpu_mhz) doc_kv(&g_doc, "Clock", "about %u MHz", si.cpu_mhz);
    doc_kv(&g_doc, "Logical processors", "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    doc_gap(&g_doc);
}

static void section_memory(void) {
    doc_head(&g_doc, "Memory");
    char buf[512], a[32], b[32];
    if (read_file("/proc/meminfo", buf, sizeof buf) <= 0) { doc_note(&g_doc, "/proc/meminfo cannot be read."); doc_gap(&g_doc); return; }
    unsigned long total = 0, free_ = 0;
    const char *p = strstr(buf, "MemTotal:");
    if (p) total = strtoul(p + 9, NULL, 10);
    p = strstr(buf, "MemFree:");
    if (p) free_ = strtoul(p + 8, NULL, 10);
    fmt_kb(total, a, sizeof a);
    fmt_kb(free_, b, sizeof b);
    doc_kv(&g_doc, "Installed RAM", "%s (%s free)", a, b);
    doc_gap(&g_doc);
}

static void section_display(void) {
    doc_head(&g_doc, "Display");
    struct gsysinfo si;
    if (gap_sysinfo(&si) == 0 && si.fb_width) doc_kv(&g_doc, "Framebuffer", "%u x %u pixels", si.fb_width, si.fb_height);
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd >= 0) {
        struct gfbinfo fi;
        if (ioctl(fd, FBIOGET_INFO, &fi) == 0) {
            doc_kv(&g_doc, "Pixel format", "%u bits per pixel, %s byte order", fi.bpp, (fi.flags & FBINFO_SWAP_RB) ? "RGBX" : "BGRX");
            doc_kv(&g_doc, "Row pitch", "%u bytes", fi.pitch);
        }
        close(fd);
    }
    int w = 0, h = 0;
    if (A.app) fairy_screen_size(kapp_display(A.app), &w, &h);
    if (w) doc_kv(&g_doc, "Desktop", "%d x %d (window server)", w, h);
    doc_gap(&g_doc);
}

static void section_pci(void) {
    doc_head(&g_doc, "PCI devices");
    char buf[4096];
    if (read_file("/proc/pci", buf, sizeof buf) <= 0) { doc_note(&g_doc, "The PCI bus cannot be read (/proc/pci)."); doc_gap(&g_doc); return; }
    static const int xs[4] = { 0, 70, 150, 250 };
    DocRow *h = doc_row(&g_doc, DOC_HEADROW, xs, 4);
    if (h) { snprintf(h->text[0], 96, "Address"); snprintf(h->text[1], 96, "Class"); snprintf(h->text[2], 96, "ID"); snprintf(h->text[3], 96, "Device"); }
    int count = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == ' ' || line[0] == '\t') continue;                  /* BAR lines belong to the device above */
        char addr[24], cls[24], ids[24];
        int off = 0;
        if (sscanf(line, "%23s %23s %23s %n", addr, cls, ids, &off) < 3 || off <= 0) continue;
        DocRow *r = doc_row(&g_doc, DOC_ROW, xs, 4);
        if (!r) break;
        snprintf(r->text[0], 96, "%s", addr);
        snprintf(r->text[1], 96, "%s", cls);
        snprintf(r->text[2], 96, "%s", ids);
        snprintf(r->text[3], 96, "%s", line + off);
        count++;
    }
    if (!count) doc_note(&g_doc, "No PCI devices were found.");
    doc_gap(&g_doc);
}

static void section_block(void) {
    doc_head(&g_doc, "Block devices");
    char buf[4096];
    if (read_file("/proc/partitions", buf, sizeof buf) <= 0) { doc_note(&g_doc, "No block devices (/proc/partitions is empty)."); doc_gap(&g_doc); return; }
    static const int xs[6] = { 0, 80, 160, 280, 380, 470 };
    DocRow *h = doc_row(&g_doc, DOC_HEADROW, xs, 6);
    if (h) {
        snprintf(h->text[0], 96, "Name"); snprintf(h->text[1], 96, "Size"); snprintf(h->text[2], 96, "Kind");
        snprintf(h->text[3], 96, "Label"); snprintf(h->text[4], 96, "Driver"); snprintf(h->text[5], 96, "Model");
    }
    int count = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char name[32], parent[32], label[48], driver[24], model[64] = "";
        unsigned long long size, start;
        unsigned ss;
        int ro;
        if (sscanf(line, "%31s %llu %u %d %31s %llu %47s %23s %63[^\n]", name, &size, &ss, &ro, parent, &start, label, driver, model) < 8) continue;
        DocRow *r = doc_row(&g_doc, DOC_ROW, xs, 6);
        if (!r) break;
        char sz[32];
        fmt_bytes(size, sz, sizeof sz);
        snprintf(r->text[0], 96, "%s", name);
        snprintf(r->text[1], 96, "%s", sz);
        snprintf(r->text[2], 96, "%s%s", strcmp(parent, "-") ? "partition of " : "disk", strcmp(parent, "-") ? parent : "");
        snprintf(r->text[3], 96, "%s", strcmp(label, "-") ? label : "");
        snprintf(r->text[4], 96, "%s%s", driver, ro ? " (read only)" : "");
        snprintf(r->text[5], 96, "%s", strcmp(model, "-") ? model : "");
        count++;
    }
    if (!count) doc_note(&g_doc, "No disks were found.");
    doc_gap(&g_doc);
}

static void section_usb(void) {
    doc_head(&g_doc, "USB devices");
    char buf[4096];
    if (read_file("/proc/usb", buf, sizeof buf) <= 0) { doc_note(&g_doc, "No USB devices are reported (this kernel has no /proc/usb)."); doc_gap(&g_doc); return; }
    int count = 0;
    for (char *line = strtok(buf, "\n"); line && count < 40; line = strtok(NULL, "\n")) {
        if (!line[0]) continue;
        doc_note(&g_doc, "%s", line);
        count++;
    }
    if (!count) doc_note(&g_doc, "No USB devices are connected.");
    doc_gap(&g_doc);
}

static void section_mounts(void) {
    doc_head(&g_doc, "Mounted volumes");
    char buf[4096];
    if (read_file("/proc/mounts", buf, sizeof buf) <= 0) { doc_note(&g_doc, "/proc/mounts cannot be read."); return; }
    static const int xs[3] = { 0, 220, 320 };
    DocRow *h = doc_row(&g_doc, DOC_HEADROW, xs, 3);
    if (h) { snprintf(h->text[0], 96, "Mounted on"); snprintf(h->text[1], 96, "Type"); snprintf(h->text[2], 96, "Source"); }
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char src[64], target[96], type[24];
        if (sscanf(line, "%63s %95s %23s", src, target, type) < 3) continue;
        DocRow *r = doc_row(&g_doc, DOC_ROW, xs, 3);
        if (!r) break;
        snprintf(r->text[0], 96, "%s", target);
        snprintf(r->text[1], 96, "%s", type);
        snprintf(r->text[2], 96, "%s", src);
    }
}

void hardware_refresh(void) {
    doc_clear(&g_doc);
    section_cpu();
    section_memory();
    section_display();
    section_pci();
    section_block();
    section_usb();
    section_mounts();
    g_refreshed_ms = k_now_ms();
    trace("spellmgr: hardware page has %d lines", g_doc.n);
}

Doc *hardware_doc(void) { return &g_doc; }

void hardware_tab(KUI *ui, KRect b) {
    if (!g_refreshed_ms || k_now_ms() - g_refreshed_ms > 5000) hardware_refresh();
    doc_draw(ui, kr_inset(b, 8), &g_doc);
}
