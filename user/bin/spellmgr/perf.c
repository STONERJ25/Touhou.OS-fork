/* The Performance tab: CPU and memory over the last 60 seconds, uptime, load, tmpfs and kernel heap use. */
#include "spell.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST 60

static int g_cpu[HIST], g_mem[HIST];
static int g_nsamples;
static uint64_t g_last_sample_ms, g_last_wall_ms, g_last_idle;
static bool g_have_prev;
static int g_cpu_now, g_mem_now;
static unsigned long g_total_kb, g_free_kb, g_used_kb, g_heap_kb, g_tmpfs_kb;
static unsigned long long g_uptime_ms;
static int g_ntasks, g_nprocs;
static double g_load[3];                 /* from the kernel */
static double g_est[3];                  /* our own estimate from CPU use */
static bool g_est_seeded;
static bool g_load_is_estimate;
static Doc g_info;

int perf_cpu_percent(void) { return g_cpu_now; }
int perf_mem_percent(void) { return g_mem_now; }
unsigned long long perf_uptime_ms(void) { return g_uptime_ms; }

static unsigned long meminfo_field(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    if (!p) return 0;
    return strtoul(p + strlen(key), NULL, 10);
}

void perf_sample(uint64_t now) {
    if (g_last_sample_ms && now - g_last_sample_ms < 800) return;
    char buf[4096];

    /* CPU: the idle task's share of the wall time that passed is the part nobody used */
    ssize_t n = read_file("/proc/tasks", buf, sizeof buf);
    uint64_t idle = g_last_idle;
    g_ntasks = 0;
    if (n > 0) {
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            int tid;
            char st[16], name[32];
            unsigned long ticks;
            if (sscanf(line, "%d %15s %31s ticks=%lu", &tid, st, name, &ticks) == 4) {
                g_ntasks++;
                if (!strcmp(name, "idle")) idle = ticks;
            }
        }
        if (g_have_prev && now > g_last_wall_ms) {
            unsigned long dw = (unsigned long)(now - g_last_wall_ms);
            unsigned long di = (unsigned long)(idle - g_last_idle);
            long pct = dw ? 100 - (long)(di * 100 / dw) : 0;
            g_cpu_now = pct < 0 ? 0 : pct > 100 ? 100 : (int)pct;
        }
        g_last_idle = idle;
        g_last_wall_ms = now;
        g_have_prev = true;
    }

    n = read_file("/proc/meminfo", buf, sizeof buf);
    if (n > 0) {
        g_total_kb = meminfo_field(buf, "MemTotal:");
        g_free_kb = meminfo_field(buf, "MemFree:");
        g_used_kb = meminfo_field(buf, "MemUsed:");
        g_heap_kb = meminfo_field(buf, "HeapInUse:");
        g_tmpfs_kb = meminfo_field(buf, "Tmpfs:");
        g_mem_now = g_total_kb ? (int)(g_used_kb * 100 / g_total_kb) : 0;
    }

    n = read_file("/proc/uptime", buf, sizeof buf);
    if (n > 0) g_uptime_ms = (unsigned long long)(atof(buf) * 1000.0);

    g_nprocs = A.procs.n;
    n = read_file("/proc/loadavg", buf, sizeof buf);
    if (n > 0) {
        double a = 0, b = 0, c = 0;
        if (sscanf(buf, "%lf %lf %lf", &a, &b, &c) == 3) { g_load[0] = a; g_load[1] = b; g_load[2] = c; }
    }
    /* the kernel reports 0.00 for all three: estimate them from how busy the CPU was (1, 5 and 15 minute averages) */
    double dt = g_last_sample_ms ? (double)(now - g_last_sample_ms) / 1000.0 : 1.0;
    static const double tau[3] = { 60, 300, 900 };
    double sample = g_cpu_now / 100.0;
    for (int i = 0; i < 3; i++) {
        double k = exp(-dt / tau[i]);
        if (!g_est_seeded) g_est[i] = sample;
        else g_est[i] = g_est[i] * k + sample * (1 - k);
    }
    g_est_seeded = true;
    g_load_is_estimate = g_load[0] == 0 && g_load[1] == 0 && g_load[2] == 0;

    if (g_nsamples == HIST) {
        memmove(g_cpu, g_cpu + 1, (HIST - 1) * sizeof g_cpu[0]);
        memmove(g_mem, g_mem + 1, (HIST - 1) * sizeof g_mem[0]);
        g_nsamples--;
    }
    g_cpu[g_nsamples] = g_cpu_now;
    g_mem[g_nsamples] = g_mem_now;
    g_nsamples++;
    g_last_sample_ms = now;
}

/* ---------------------------------------------------------------- drawing */

static void graph(KUI *ui, KRect r, const int *hist, int n, uint32_t line, const char *title, const char *now_text, const char *y_note) {
    KCanvas *cv = &ui->cv;
    kc_text_bold(cv, r.x + 2, r.y, title, ui->theme->accent_dark);
    int tw = (int)strlen(now_text) * 8;
    kc_text_bold(cv, r.x + r.w - tw - 2, r.y, now_text, ui->theme->text);
    KRect g = kr(r.x, r.y + 20, r.w, r.h - 20 - 16);
    kc_panel(cv, g, KB_SUNKEN, 0x0E1330);
    KRect in = kr_inset(g, 2);
    for (int q = 1; q < 4; q++) kc_hline(cv, in.x, in.y + in.h * q / 4, in.w, 0x1D2A5C);
    int step_px = in.w / 6;                                            /* a vertical line every 10 seconds */
    for (int s = 1; s < 6; s++) kc_vline(cv, in.x + in.w - s * step_px, in.y, in.h, 0x1D2A5C);
    kc_text(cv, in.x + 3, in.y + 1, "100%", 0x4B5AA0);
    kc_text(cv, in.x + 3, in.y + in.h - 17, "0", 0x4B5AA0);
    if (n >= 2) {
        int prev_x = 0, prev_y = 0;
        for (int i = 0; i < n; i++) {
            int x = in.x + in.w - 1 - (n - 1 - i) * (in.w - 1) / (HIST - 1);
            int y = in.y + in.h - 3 - hist[i] * (in.h - 6) / 100;                 /* keeps a 0% line off the border */
            if (i > 0) {
                for (int cx = prev_x; cx <= x; cx++) {
                    int t = x == prev_x ? 0 : (cx - prev_x) * 256 / (x - prev_x);
                    int cy = prev_y + (y - prev_y) * t / 256;
                    kc_fill_alpha(cv, kr(cx, cy, 1, in.y + in.h - cy), line, 55);
                }
                kc_line(cv, prev_x, prev_y, x, y, line);
                kc_line(cv, prev_x, prev_y - 1, x, y - 1, line);
            }
            prev_x = x;
            prev_y = y;
        }
    }
    kc_text_in(cv, kr(r.x, r.y + r.h - 16, r.w, 16), y_note, ui->theme->text_dim, KA_LEFT);
}

static void rebuild_info(void) {
    doc_clear(&g_info);
    char a[48], b[48], c[48];
    fmt_duration(g_uptime_ms, a, sizeof a);
    doc_kv(&g_info, "Uptime", "%s", a);
    doc_kv(&g_info, "Spells (processes)", "%d running, %d tasks inside the kernel", g_nprocs, g_ntasks);
    const double *l = g_load_is_estimate ? g_est : g_load;
    doc_kv(&g_info, "Load average", "%.2f  %.2f  %.2f%s", l[0], l[1], l[2], g_load_is_estimate ? "  (1, 5, 15 min; estimated from CPU use)" : "  (1, 5, 15 min)");
    fmt_kb(g_total_kb, a, sizeof a);
    fmt_kb(g_used_kb, b, sizeof b);
    fmt_kb(g_free_kb, c, sizeof c);
    doc_kv(&g_info, "Memory", "%s used of %s, %s free", b, a, c);
    fmt_kb(g_heap_kb, a, sizeof a);
    doc_kv(&g_info, "Kernel heap in use", "%s", a);
    fmt_kb(g_tmpfs_kb, a, sizeof a);
    doc_kv(&g_info, "tmpfs (files in RAM)", "%s", a);
}

void perf_tab(KUI *ui, KRect b) {
    KCanvas *cv = &ui->cv;
    (void)cv;
    int pad = 10;
    bool side = b.w >= 640;
    int gh = side ? 150 : 124;
    if (!side && b.h < 2 * gh + 120) gh = (b.h - 120) / 2 > 70 ? (b.h - 120) / 2 : 70;
    int gw = side ? (b.w - pad * 3) / 2 : b.w - pad * 2;
    char now_text[32];
    snprintf(now_text, sizeof now_text, "%d%%", g_cpu_now);
    KRect gcpu = kr(b.x + pad, b.y + pad, gw, gh);
    graph(ui, gcpu, g_cpu, g_nsamples, 0xE8B83C, "CPU", now_text, "60 seconds ago ... now (the idle task's share is subtracted)");
    snprintf(now_text, sizeof now_text, "%d%%", g_mem_now);
    KRect gmem = side ? kr(gcpu.x + gw + pad, gcpu.y, gw, gh) : kr(gcpu.x, gcpu.y + gh + 6, gw, gh);
    graph(ui, gmem, g_mem, g_nsamples, 0x5BC0E0, "Memory", now_text, "60 seconds ago ... now");
    int y = (side ? gcpu.y + gh : gmem.y + gh) + 8;
    if (y < b.y + b.h - 40) {
        rebuild_info();
        doc_draw(ui, kr(b.x + pad, y, b.w - pad * 2, b.y + b.h - y - pad), &g_info);
    }
}
