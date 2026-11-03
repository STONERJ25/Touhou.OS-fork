/*
 * The system pages of the paper. Everything here is written from the machine's real state when the app starts: uptime and
 * memory from /proc, the processes, the mounted volumes, the devices the kernel found, and the lines of /dev/kmsg that
 * look worth a story (Eiki's incident reports, warnings) rewritten in the paper's voice.
 */
#include "news.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Top { char name[32], user[24]; unsigned long ticks; int pid; } Top;

typedef struct Sys {
    unsigned long long uptime_ms;
    unsigned long total_kb, free_kb, used_kb, heap_kb, tmpfs_kb;
    int nprocs, nzombies;
    Top top[3];
    int ntop;
    char commonest[32];
    int commonest_n;
    int nmounts;
    char mnt[8][40], fst[8][16];
    int npci;
    char pci[10][64];
    int ndisk;
    char disk[4][24];
    unsigned long long dsize[4];
    char dmodel[4][40];
    int nusb;
    int neiki, nwarn, nbrief;
    char eiki[6][220];
    char warn[4][220];
    char brief[4][220];
    char brief_tag[4][16];
    int nlines;
} Sys;

static ssize_t slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (ssize_t)n;
}

static unsigned long meminfo(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    return p ? strtoul(p + strlen(key), NULL, 10) : 0;
}

/* "3 hours and 12 minutes" */
static void duration_words(unsigned long long ms, char *out, size_t cap) {
    unsigned long long s = ms / 1000;
    if (s < 60) snprintf(out, cap, "%llu second%s", s, s == 1 ? "" : "s");
    else if (s < 3600) snprintf(out, cap, "%llu minute%s", s / 60, s / 60 == 1 ? "" : "s");
    else if (s < 86400) {
        unsigned long long h = s / 3600, mi = s / 60 % 60;
        if (mi) snprintf(out, cap, "%llu hour%s and %llu minute%s", h, h == 1 ? "" : "s", mi, mi == 1 ? "" : "s");
        else snprintf(out, cap, "%llu hour%s", h, h == 1 ? "" : "s");
    } else {
        unsigned long long dd = s / 86400, h = s / 3600 % 24;
        snprintf(out, cap, "%llu day%s and %llu hour%s", dd, dd == 1 ? "" : "s", h, h == 1 ? "" : "s");
    }
}

static void size_words(unsigned long kb, char *out, size_t cap) {
    if (kb < 1024) snprintf(out, cap, "%lu kB", kb);
    else if (kb < 1024 * 1024) snprintf(out, cap, "%lu.%lu MB", kb >> 10, (kb & 1023) * 10 >> 10);
    else snprintf(out, cap, "%lu.%lu GB", kb >> 20, (kb & 0xFFFFF) * 10 >> 20);
}

static const char *s_(int n) { return n == 1 ? "" : "s"; }

/* ---------------------------------------------------------------- gathering */

static void gather_procs(Sys *s) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct { char name[32]; int n; } kinds[24];
    int nk = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[48], buf[320];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        if (slurp(path, buf, sizeof buf) <= 0) continue;
        int pid, ppid, pgid, used = 0;
        unsigned uid, gid;
        char st[4];
        unsigned long vsz, rss, ticks, start;
        if (sscanf(buf, "%d %d %d %u %u %3s %lu %lu %lu %lu %n", &pid, &ppid, &pgid, &uid, &gid, st, &vsz, &rss, &ticks, &start, &used) < 10 || used <= 0) continue;
        char name[32];
        size_t len = strcspn(buf + used, "\r\n");
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, buf + used, len);
        name[len] = 0;
        s->nprocs++;
        if (st[0] == 'Z') s->nzombies++;
        int k = 0;
        for (; k < nk; k++) if (!strcmp(kinds[k].name, name)) { kinds[k].n++; break; }
        if (k == nk && nk < 24) { snprintf(kinds[nk].name, sizeof kinds[nk].name, "%s", name); kinds[nk].n = 1; nk++; }
        int at = -1;                                                    /* a place among the three busiest */
        for (int i = 0; i < 3; i++) if (i >= s->ntop || ticks > s->top[i].ticks) { at = i; break; }
        if (at >= 0) {
            if (s->ntop < 3) s->ntop++;
            for (int i = s->ntop - 1; i > at; i--) s->top[i] = s->top[i - 1];
            snprintf(s->top[at].name, sizeof s->top[at].name, "%s", name);
            struct passwd *pw = getpwuid(uid);
            snprintf(s->top[at].user, sizeof s->top[at].user, "%s", pw ? pw->pw_name : "someone");
            s->top[at].ticks = ticks;
            s->top[at].pid = pid;
        }
    }
    closedir(d);
    for (int k = 0; k < nk; k++)
        if (kinds[k].n > s->commonest_n) { s->commonest_n = kinds[k].n; snprintf(s->commonest, sizeof s->commonest, "%s", kinds[k].name); }
}

static void gather_machine(Sys *s) {
    char buf[4096];
    if (slurp("/proc/uptime", buf, sizeof buf) > 0) s->uptime_ms = (unsigned long long)(atof(buf) * 1000.0);
    if (slurp("/proc/meminfo", buf, sizeof buf) > 0) {
        s->total_kb = meminfo(buf, "MemTotal:");
        s->free_kb = meminfo(buf, "MemFree:");
        s->used_kb = meminfo(buf, "MemUsed:");
        s->heap_kb = meminfo(buf, "HeapInUse:");
        s->tmpfs_kb = meminfo(buf, "Tmpfs:");
    }
    gather_procs(s);
    if (slurp("/proc/mounts", buf, sizeof buf) > 0) {
        for (char *line = strtok(buf, "\n"); line && s->nmounts < 8; line = strtok(NULL, "\n")) {
            char src[64], target[40], type[16];
            if (sscanf(line, "%63s %39s %15s", src, target, type) == 3) {
                snprintf(s->mnt[s->nmounts], sizeof s->mnt[0], "%s", target);
                snprintf(s->fst[s->nmounts], sizeof s->fst[0], "%s", type);
                s->nmounts++;
            }
        }
    }
    if (slurp("/proc/pci", buf, sizeof buf) > 0) {
        for (char *line = strtok(buf, "\n"); line && s->npci < 10; line = strtok(NULL, "\n")) {
            if (line[0] == ' ' || line[0] == '\t') continue;
            char a[24], c[24], i[24];
            int off = 0;
            if (sscanf(line, "%23s %23s %23s %n", a, c, i, &off) < 3 || off <= 0) continue;
            char *drv = strstr(line + off, "  [");
            snprintf(s->pci[s->npci], sizeof s->pci[0], "%.*s", drv ? (int)(drv - (line + off)) : 60, line + off);
            s->npci++;
        }
    }
    if (slurp("/proc/partitions", buf, sizeof buf) > 0) {
        for (char *line = strtok(buf, "\n"); line && s->ndisk < 4; line = strtok(NULL, "\n")) {
            char name[32], parent[32], label[48], driver[24], model[64] = "";
            unsigned long long size, start;
            unsigned ss;
            int ro;
            if (sscanf(line, "%31s %llu %u %d %31s %llu %47s %23s %63[^\n]", name, &size, &ss, &ro, parent, &start, label, driver, model) < 8) continue;
            if (strcmp(parent, "-")) continue;                              /* whole disks only */
            snprintf(s->disk[s->ndisk], sizeof s->disk[0], "%s", name);
            s->dsize[s->ndisk] = size;
            snprintf(s->dmodel[s->ndisk], sizeof s->dmodel[0], "%s", strcmp(model, "-") ? model : driver);
            s->ndisk++;
        }
    }
    if (slurp("/proc/usb", buf, sizeof buf) > 0) for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) if (line[0]) s->nusb++;
}

/* the kernel log: keep what looks like news */
static void gather_log(Sys *s) {
    int fd = open("/dev/kmsg", O_RDONLY);
    if (fd < 0) return;
    size_t cap = 200 * 1024, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return; }
    for (;;) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= cap) break;
    }
    close(fd);
    buf[n] = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        s->nlines++;
        if (line[0] != '[') continue;
        char *close_b = strchr(line, ']');
        if (!close_b) continue;
        char tag[24];
        size_t tl = (size_t)(close_b - line - 1);
        if (tl >= sizeof tag) tl = sizeof tag - 1;
        memcpy(tag, line + 1, tl);
        tag[tl] = 0;
        const char *msg = close_b + 1;
        while (*msg == ' ') msg++;
        if (!strcmp(tag, "Eiki")) {
            if (s->neiki < 6) snprintf(s->eiki[s->neiki], sizeof s->eiki[0], "%s", msg);
            s->neiki++;
        } else if (strstr(msg, "fail") || strstr(msg, "cannot") || strstr(msg, "error") || strstr(msg, "truncated") || strstr(msg, "hungry")) {
            if (s->nwarn < 4) snprintf(s->warn[s->nwarn], sizeof s->warn[0], "[%s] %s", tag, msg);
            s->nwarn++;
        } else if ((!strcmp(tag, "Koishi") || !strcmp(tag, "Satori") || !strcmp(tag, "Fairies") || !strcmp(tag, "Sakuya") || !strcmp(tag, "Kappa")) && s->nbrief < 4) {
            int dup = 0;
            for (int i = 0; i < s->nbrief; i++) if (!strcmp(s->brief_tag[i], tag)) dup = 1;
            if (!dup) {
                snprintf(s->brief[s->nbrief], sizeof s->brief[0], "%s", msg);
                snprintf(s->brief_tag[s->nbrief], sizeof s->brief_tag[0], "%s", tag);
                s->nbrief++;
            }
        }
    }
    free(buf);
}

/* ---------------------------------------------------------------- writing */

static void add(Issue *is, const char *kicker, const char *headline, const char *byline, const char *body) {
    if (is->nsys >= MAX_SYS) return;
    is->sys[is->nsys++] = (Article){ kicker, headline, byline, body };
}

static const char *docket_verdict(uint32_t h) {
    static const char *const v[] = {
        "Case closed.", "The judge adjourned for tea.", "No appeal was lodged.", "Filed under 'about what we expected'.",
        "The paperwork was stamped twice, to be safe.", "The court thanks all parties and the furniture.",
    };
    return v[h % (sizeof v / sizeof v[0])];
}

static void docket_entry(Issue *is, char *out, size_t cap, int n, const char *msg) {
    uint32_t h = hash32((uint32_t)is->daynum + (uint32_t)n * 131u);
    char name[40];
    int pid = 0, sig = 0;
    if (sscanf(msg, "'%39[^']' (pid %d) was exorcised by signal %d", name, &pid, &sig) == 3) {
        const char *kind = sig == 15 ? "the polite kind" : sig == 9 ? "the impolite kind" : sig == 11 ? "the sort that follows a boundary violation" : "a signal of unusual taste";
        snprintf(out, cap, "Case %d: the spell '%s' (pid %d) was sent away by signal %d, %s. %s", n, name, pid, sig, kind, docket_verdict(h));
    } else if (strstr(msg, "BOUNDARY VIOLATION")) {
        snprintf(out, cap, "Case %d: a spell strayed across a boundary. Yukari has been informed and is pretending to be asleep. The record reads: %.150s. %s", n, msg, docket_verdict(h));
    } else if (strstr(msg, "Ctrl+Alt+Backspace")) {
        snprintf(out, cap, "Case %d: somebody pressed the great three-key chord, and the window server went to the Netherworld on purpose. The judge approves of deliberate acts. %s", n, docket_verdict(h));
    } else if (strstr(msg, "hungry") || strstr(msg, "out of memory")) {
        snprintf(out, cap, "Case %d: Yuyuko was hungry. The record reads: %.150s. %s", n, msg, docket_verdict(h));
    } else {
        snprintf(out, cap, "Case %d: %.170s. %s", n, msg, docket_verdict(h));
    }
}

static const char *brief_voice(const char *tag, const char *msg, char *out, size_t cap) {
    if (!strcmp(tag, "Koishi")) snprintf(out, cap, "Koishi, who does most things unconsciously, reports that '%.100s'. This paper noticed anyway.", msg);
    else if (!strcmp(tag, "Satori")) snprintf(out, cap, "Satori, the mind reader, confirms: '%.100s'. She asks readers not to think too loudly.", msg);
    else if (!strcmp(tag, "Fairies")) snprintf(out, cap, "The three fairies of light, holding the pixels this morning, say: '%.100s'. They would like a medal.", msg);
    else if (!strcmp(tag, "Sakuya")) snprintf(out, cap, "Sakuya's clock reports: '%.110s'. The maid's own time is not for sale.", msg);
    else if (!strcmp(tag, "Kappa")) snprintf(out, cap, "The kappa engineers wish to note: '%.110s'. It is, they add, mostly waterproof.", msg);
    else snprintf(out, cap, "%s says: '%.130s'.", tag, msg);
    return out;
}

void sysnews_build(Issue *is) {
    static Sys sy;
    memset(&sy, 0, sizeof sy);
    gather_machine(&sy);
    gather_log(&sy);
    uint32_t h = hash32((uint32_t)is->daynum * 31u + 5u);
    char t1[64], t2[64], t3[64], t4[64];

    /* ---- the lead: how long the shrine has been open */
    duration_words(sy.uptime_ms, t1, sizeof t1);
    const char *lead_head, *lead_body;
    if (sy.uptime_ms < 120000) {
        lead_head = pool_printf(is, "Shrine Opens For The Day; The Floor Is Still Warm");
        lead_body = pool_printf(is,
            "The local shrine opened its doors only %s ago and has so far run without any trouble at all. Early visitors report a pleasant smell of fresh tatami and a very small number of complaints. There are already %d spells at work inside.\n"
            "The keeper of the shrine declined to comment on the grounds that she had only just sat down. The day, she is understood to have said, can wait until the tea has finished.",
            t1, sy.nprocs);
    } else {
        if (sy.neiki == 0)
            lead_head = pool_printf(is, "Local Shrine Runs For %s Without Incident", t1);
        else
            lead_head = pool_printf(is, "Local Shrine Runs For %s With %d Minor Incident%s", t1, sy.neiki, s_(sy.neiki));
        const char *quote = (h & 1) ? "'We would like to keep it that way,' said an unnamed init, pid 1, who was reached at its desk."
                                    : "'Nothing happened,' reported a spell who asked not to be named, 'and long may it go on not happening.'";
        char incidents[200] = "";
        if (sy.neiki) snprintf(incidents, sizeof incidents, "\nThe shrine admits to %d minor incident%s, all of them filed neatly by the office of Eiki Shiki (see page 2). None were serious, probably.", sy.neiki, s_(sy.neiki));
        else snprintf(incidents, sizeof incidents, "\nEiki Shiki's office has no cases to hear today and has asked whether anyone has anything it could look into.");
        lead_body = pool_printf(is,
            "The local shrine has now been open for %s, and our correspondent can confirm that nothing of note has caught fire. %s\n"
            "There are %d spell%s in flight, and the larder is %lu%% full. Page 2 has the details, and page 3 has the gossip.%s",
            t1, quote, sy.nprocs, s_(sy.nprocs), sy.total_kb ? sy.used_kb * 100 / sy.total_kb : 0UL, incidents);
    }
    is->sys_lead = is->nsys;
    add(is, "FRONT PAGE", lead_head, "By Aya Shameimaru, with the help of /proc", lead_body);

    /* ---- memory: Yuyuko's appetite */
    {
        unsigned long pct = sy.total_kb ? sy.used_kb * 100 / sy.total_kb : 0;
        size_words(sy.total_kb, t1, sizeof t1);
        size_words(sy.used_kb, t2, sizeof t2);
        size_words(sy.free_kb, t3, sizeof t3);
        const char *head = pct < 5 ? pool_printf(is, "Yuyuko On A Diet: Only %lu%% Of The Larder Eaten", pct) :
                           pct < 30 ? pool_printf(is, "Yuyuko Nibbles: %lu%% Of The Larder Eaten", pct) :
                           pct < 60 ? pool_printf(is, "Yuyuko Half Fed: %lu%% Of The Larder In Use", pct) :
                           pct < 85 ? pool_printf(is, "Larder Running Low: Yuyuko At %lu%%", pct) :
                                      pool_printf(is, "Yuyuko Is Hungry: The Larder Is %lu%% Gone", pct);
        size_words(sy.heap_kb, t4, sizeof t4);
        char tmp[64];
        size_words(sy.tmpfs_kb, tmp, sizeof tmp);
        static const char *const tails[] = {
            "Experts call this a perfectly ordinary appetite for the hour.",
            "Nutritionists at the Eientei clinic say there is no cause for alarm, and some cause for snacks.",
            "The ghost princess was reported to have said 'Mm' in a satisfied way.",
        };
        add(is, "THE LARDER", head, "By Aya Shameimaru",
            pool_printf(is,
                "Of the %s of memory in the pantry, %s is in use and %s is free. The kernel's own heap holds %s, and the RAM disk where temporary files live (tmpfs) holds %s.\n%s",
                t1, t2, t3, t4, tmp, tails[(h >> 3) % 3]));
    }

    /* ---- the spells in flight */
    {
        char head[160], body[700];
        if (sy.ntop && sy.top[0].ticks > 0) snprintf(head, sizeof head, "%d Spell%s In Flight; '%s' Has Been The Busiest", sy.nprocs, s_(sy.nprocs), sy.top[0].name);
        else snprintf(head, sizeof head, "%d Spell%s In Flight, All Of Them Rather Shy", sy.nprocs, s_(sy.nprocs));
        size_t n = (size_t)snprintf(body, sizeof body, "At last count %d spell%s %s running. ", sy.nprocs, s_(sy.nprocs), sy.nprocs == 1 ? "is" : "are");
        for (int i = 0; i < sy.ntop; i++) {
            char t[40];
            if (sy.top[i].ticks < 1000) snprintf(t, sizeof t, "%lu milliseconds", sy.top[i].ticks);
            else duration_words((unsigned long long)sy.top[i].ticks, t, sizeof t);
            n += (size_t)snprintf(body + n, sizeof body - n, "%s%s (pid %d, cast by %s) has used %s of the CPU", i == 0 ? "" : (i == sy.ntop - 1 ? ", and " : ", "), sy.top[i].name, sy.top[i].pid, sy.top[i].user, t);
        }
        if (sy.ntop) n += (size_t)snprintf(body + n, sizeof body - n, ".\n");
        if (sy.commonest_n > 1) n += (size_t)snprintf(body + n, sizeof body - n, "The most numerous resident is '%s', of which there are %d. ", sy.commonest, sy.commonest_n);
        if (sy.nzombies) snprintf(body + n, sizeof body - n, "%d spell%s ha%s ended but still wait%s for a parent to sign %s off. They are known as zombies, and are harmless, if awkward.",
                                  sy.nzombies, s_(sy.nzombies), sy.nzombies == 1 ? "s" : "ve", sy.nzombies == 1 ? "s" : "", sy.nzombies == 1 ? "it" : "them");
        else snprintf(body + n, sizeof body - n, "Every one of them is accounted for. Open the Spell Manager (Ctrl+Alt+Del) to see them for yourself.");
        add(is, "SPELLS", pool_printf(is, "%s", head), "By Aya Shameimaru", pool_printf(is, "%s", body));
    }

    /* ---- volumes */
    {
        char body[700];
        size_t n = (size_t)snprintf(body, sizeof body, "The shrine currently has %d volume%s mounted: ", sy.nmounts, s_(sy.nmounts));
        for (int i = 0; i < sy.nmounts; i++)
            n += (size_t)snprintf(body + n, sizeof body - n, "%s%s (%s)", i == 0 ? "" : (i == sy.nmounts - 1 ? " and " : ", "), sy.mnt[i], sy.fst[i]);
        n += (size_t)snprintf(body + n, sizeof body - n, ".\n");
        bool has_proc = false, has_dev = false;
        for (int i = 0; i < sy.nmounts; i++) { if (!strcmp(sy.mnt[i], "/proc")) has_proc = true; if (!strcmp(sy.mnt[i], "/dev")) has_dev = true; }
        if (has_proc) n += (size_t)snprintf(body + n, sizeof body - n, "The most talkative is /proc, which reports on everybody, including itself. ");
        if (has_dev) n += (size_t)snprintf(body + n, sizeof body - n, "/dev, meanwhile, is where the devices live, and it keeps very odd hours.");
        add(is, "VOLUMES", pool_printf(is, "%d Volume%s Open For Business", sy.nmounts, s_(sy.nmounts)), "By Aya Shameimaru", pool_printf(is, "%s", body));
    }

    /* ---- the PCI quarter and the disks */
    {
        char body[900];
        size_t n = 0;
        if (sy.npci == 0) n = (size_t)snprintf(body, sizeof body, "Our correspondent knocked on every door in the PCI quarter and found nobody in. Either they are all out, or the quarter has not been surveyed. Locals say it is the latter.");
        else {
            n = (size_t)snprintf(body, sizeof body, "A census of the PCI quarter found %d resident%s: ", sy.npci, s_(sy.npci));
            for (int i = 0; i < sy.npci; i++)
                n += (size_t)snprintf(body + n, sizeof body - n, "%s%s", i == 0 ? "" : (i == sy.npci - 1 ? " and " : ", "), sy.pci[i]);
            n += (size_t)snprintf(body + n, sizeof body - n, ".\n");
            bool vga = false, host = false;
            for (int i = 0; i < sy.npci; i++) { if (strstr(sy.pci[i], "VGA")) vga = true; if (strstr(sy.pci[i], "Host")) host = true; }
            if (vga) n += (size_t)snprintf(body + n, sizeof body - n, "The VGA controller paints every window you see and has never once been thanked. ");
            if (host) n += (size_t)snprintf(body + n, sizeof body - n, "The host bridge, the oldest resident, knows everyone and is not telling.");
        }
        add(is, "THE PCI QUARTER", pool_printf(is, sy.npci ? "PCI Quarter Home To %d Resident%s" : "PCI Quarter Silent; Nobody Home", sy.npci, s_(sy.npci)),
            "By Nitori Kawashiro (guest column)", pool_printf(is, "%s", body));
    }
    {
        char body[700], head[160];
        if (sy.ndisk == 0) {
            snprintf(head, sizeof head, "No Disks Attached: The Kernel Lives On RAM Alone");
            snprintf(body, sizeof body, "The kernel has no disks attached this morning and is living, as it puts it, 'entirely in the moment'. Everything in /tmp, and in the home folders, will vanish when the shrine closes for the night.\nUSB devices: %s.",
                     sy.nusb ? "some are reported" : "none reported (the kernel has no USB listing yet)");
        } else {
            char sz[32];
            snprintf(sz, sizeof sz, "%llu MiB", sy.dsize[0] >> 20);
            snprintf(head, sizeof head, "%d Disk%s Move%s In: '%s' (%s) Is The First", sy.ndisk, s_(sy.ndisk), sy.ndisk == 1 ? "s" : "", sy.disk[0], sz);
            size_t n = (size_t)snprintf(body, sizeof body, "The following disk%s moved into the neighbourhood: ", s_(sy.ndisk));
            for (int i = 0; i < sy.ndisk; i++)
                n += (size_t)snprintf(body + n, sizeof body - n, "%s%s, %llu MiB, model %s", i == 0 ? "" : "; ", sy.disk[i], sy.dsize[i] >> 20, sy.dmodel[i]);
            snprintf(body + n, sizeof body - n, ".\nNeighbours describe the new arrivals as 'solid' and 'round, in a spinning way'. Files kept on them will still be there tomorrow.");
        }
        add(is, "HARDWARE", pool_printf(is, "%s", head), "By Nitori Kawashiro (guest column)", pool_printf(is, "%s", body));
    }

    /* ---- Eiki's docket */
    {
        char body[1400];
        size_t n = 0;
        if (sy.neiki == 0) n = (size_t)snprintf(body, sizeof body, "The office of Eiki Shiki reports that no incidents have been filed today. The judge has asked, politely, whether somebody could please do something about that. Anything small will do.");
        else {
            int shown = sy.neiki < 4 ? sy.neiki : 4;
            n = (size_t)snprintf(body, sizeof body, "The court of Eiki Shiki has heard %d case%s since the shrine opened.", sy.neiki, s_(sy.neiki));
            for (int i = 0; i < shown; i++) {
                char entry[400];
                docket_entry(is, entry, sizeof entry, i + 1, sy.eiki[i]);
                n += (size_t)snprintf(body + n, sizeof body - n, "\n%s", entry);
            }
            if (sy.neiki > shown) snprintf(body + n, sizeof body - n, "\n%d further case%s are on the judge's desk.", sy.neiki - shown, s_(sy.neiki - shown));
        }
        add(is, "THE COURT", pool_printf(is, sy.neiki ? "Eiki's Docket: %d Case%s Heard" : "No Incidents Today; Eiki Asks For Something To Do", sy.neiki, s_(sy.neiki)),
            "By Aya Shameimaru", pool_printf(is, "%s", body));
    }

    /* ---- the kernel log in brief */
    if (sy.nbrief || sy.nwarn) {
        char body[1200];
        size_t n = (size_t)snprintf(body, sizeof body, "%d line%s of the kernel log were read for this edition.", sy.nlines, s_(sy.nlines));
        for (int i = 0; i < sy.nbrief; i++) {
            char voiced[320];
            n += (size_t)snprintf(body + n, sizeof body - n, "\n%s", brief_voice(sy.brief_tag[i], sy.brief[i], voiced, sizeof voiced));
        }
        for (int i = 0; i < sy.nwarn && i < 2; i++)
            n += (size_t)snprintf(body + n, sizeof body - n, "\nA mutter was also overheard: '%.150s'. Our analysts say it is probably nothing. Probably.", sy.warn[i]);
        add(is, "THE KERNEL LOG", pool_printf(is, "The Kernel Log, In Brief"), "By Aya Shameimaru", pool_printf(is, "%s", body));
    }
    news_trace("bunbunmaru: %d system stories, %d kernel log lines, %d incidents", is->nsys, sy.nlines, sy.neiki);
}
