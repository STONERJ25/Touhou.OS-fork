/* The Users pane: accounts and their groups, and a way to change a password (through the terminal, when passwd exists). */
#include "panel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ACCOUNTS 32

typedef struct Account {
    char name[32], gecos[64], home[96], shell[48];
    unsigned uid, gid;
} Account;

static Account g_acc[MAX_ACCOUNTS];
static int g_nacc;
static char *g_names[MAX_ACCOUNTS];
static KListState g_list;
static bool g_loaded;
static uint64_t g_loaded_ms;
static char g_groups_file[2048];

static int split(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ':');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    return n;
}

static void load_accounts(void) {
    char buf[4096];
    if (!g_loaded) k_list_init(&g_list);
    g_nacc = 0;
    if (read_file("/etc/passwd", buf, sizeof buf) > 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line && g_nacc < MAX_ACCOUNTS; line = strtok_r(NULL, "\n", &save)) {
            if (line[0] == '#' || !line[0]) continue;
            char *f[8];
            if (split(line, f, 8) < 7) continue;
            Account *a = &g_acc[g_nacc];
            snprintf(a->name, sizeof a->name, "%s", f[0]);
            a->uid = (unsigned)strtoul(f[2], NULL, 10);
            a->gid = (unsigned)strtoul(f[3], NULL, 10);
            snprintf(a->gecos, sizeof a->gecos, "%s", f[4]);
            snprintf(a->home, sizeof a->home, "%s", f[5]);
            snprintf(a->shell, sizeof a->shell, "%s", f[6]);
            g_names[g_nacc] = a->name;
            g_nacc++;
        }
    }
    if (read_file("/etc/group", g_groups_file, sizeof g_groups_file) <= 0) g_groups_file[0] = 0;
    if (g_list.selected < 0 || g_list.selected >= g_nacc) {
        g_list.selected = -1;
        for (int i = 0; i < g_nacc; i++) if (g_acc[i].uid == getuid()) g_list.selected = i;
        if (g_list.selected < 0 && g_nacc) g_list.selected = 0;
    }
    g_loaded = true;
    g_loaded_ms = k_now_ms();
}

/* the groups an account belongs to: its primary group and every group listing it as a member */
static void groups_of(const Account *a, char *out, size_t cap) {
    out[0] = 0;
    char copy[sizeof g_groups_file];
    snprintf(copy, sizeof copy, "%s", g_groups_file);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[5];
        int n = split(line, f, 5);
        if (n < 3) continue;
        unsigned gid = (unsigned)strtoul(f[2], NULL, 10);
        bool member = gid == a->gid;
        if (!member && n > 3) {
            char members[256];
            snprintf(members, sizeof members, ",%s,", f[3]);
            char needle[40];
            snprintf(needle, sizeof needle, ",%s,", a->name);
            member = strstr(members, needle) != NULL;
        }
        if (member) {
            if (out[0]) strncat(out, ", ", cap - strlen(out) - 1);
            strncat(out, f[0], cap - strlen(out) - 1);
        }
    }
    if (!out[0]) snprintf(out, cap, "(none)");
}

static const char *passwd_path(void) {
    const char *p = getenv("KAPPA_PASSWD");                              /* lets tests point at a stand-in */
    return p && *p ? p : "/bin/passwd";
}

/* Opens a terminal that runs passwd and waits for Enter. gapterm always starts a login shell, and that shell runs
 * ~/.gaprc, so the terminal is started with a throw-away home folder whose .gaprc does the job. */
static void change_password(const Account *a) {
    char dir[96], rc[128];
    snprintf(dir, sizeof dir, "/tmp/kappa-passwd-%d", (int)getpid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        const char *btn[] = { "OK" };
        k_messagebox(kapp_display(P.app), "Control Panel", "Cannot prepare the terminal window.", KI_WARNING, btn, 1);
        return;
    }
    snprintf(rc, sizeof rc, "%s/.gaprc", dir);
    FILE *f = fopen(rc, "w");
    if (!f) return;
    fprintf(f, "echo 'Changing the password of %s'\n%s %s\necho\necho 'Press Enter to close this window.'\nread answer\nexit\n", a->name, passwd_path(),
            a->uid == getuid() ? "" : a->name);
    fclose(f);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        setenv("HOME", dir, 1);
        execl("/bin/gapterm", "gapterm", (char *)NULL);
        _exit(127);
    }
    trace("kappa-panel: password terminal started for %s", a->name);
}

void pane_users(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    draw_heading(ui, pane, KI_USER, "Users", "Who lives in this Gensokyo");
    KRect b = pane_body(pane);
    if (!g_loaded || k_now_ms() - g_loaded_ms > 10000) load_accounts();

    int lw = b.w >= 520 ? 190 : 140;
    KRect list = kr(b.x + 6, b.y + 6, lw, b.h - 12);
    if (g_nacc == 0) {
        kc_panel(cv, list, KB_SUNKEN, ui->theme->field_bg);
        kc_text_in(cv, kr(list.x + 8, list.y + 8, list.w - 16, 16), "No accounts found.", ui->theme->text_dim, KA_LEFT);
    } else {
        KIcon icons[MAX_ACCOUNTS];
        for (int i = 0; i < g_nacc; i++) icons[i] = KI_USER;
        if (k_list(ui, list, (const char *const *)g_names, icons, g_nacc, &g_list)) trace("kappa-panel: selected account %s", g_acc[g_list.selected].name);
    }

    KRect d = kr(list.x + list.w + 14, b.y + 10, b.w - lw - 30, b.h - 20);
    if (g_list.selected < 0 || g_list.selected >= g_nacc) return;
    const Account *a = &g_acc[g_list.selected];
    k_icon(cv, KI_USER, d.x, d.y, 48);
    kc_text_bold(cv, d.x + 60, d.y + 4, a->name, ui->theme->accent_dark);
    kc_text_in(cv, kr(d.x + 60, d.y + 24, d.w - 60, 16), a->gecos[0] ? a->gecos : "(no full name)", ui->theme->text, KA_LEFT);
    if (a->uid == getuid()) kc_text_in(cv, kr(d.x + 60, d.y + 42, d.w - 60, 16), "This is you.", ui->theme->accent, KA_LEFT);
    int y = d.y + 70, labw = 144;
    char buf[200];
    snprintf(buf, sizeof buf, "%u", a->uid);
    kv_line(ui, d.x, y, labw, d.w, "User ID", buf); y += 20;
    snprintf(buf, sizeof buf, "%u", a->gid);
    kv_line(ui, d.x, y, labw, d.w, "Primary group ID", buf); y += 20;
    kv_line(ui, d.x, y, labw, d.w, "Home folder", a->home); y += 20;
    kv_line(ui, d.x, y, labw, d.w, "Shell", a->shell); y += 20;
    groups_of(a, buf, sizeof buf);
    kv_line(ui, d.x, y, labw, d.w, "Groups", buf); y += 36;

    bool have = access(passwd_path(), X_OK) == 0;
    KRect btn = kr(d.x, y, 190, 28);
    if (have) {
        if (k_button(ui, btn, "Change password...")) change_password(a);
    } else {
        kc_panel(cv, btn, KB_RAISED, kc_shade(ui->theme->button_bg, 96));
        kc_text_in(cv, btn, "Change password...", ui->theme->text_dim, KA_CENTER);
        draw_wrapped(ui, d.x, y + 38, d.w, "The passwd program is not installed yet. Once it is, this button opens a terminal window and runs it there.", ui->theme->text_dim);
    }
}
