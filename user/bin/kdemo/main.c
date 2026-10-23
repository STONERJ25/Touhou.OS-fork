/* kdemo: a tour of the Kappa widgets; also the first client used to test the window server. */
#include <kappa.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char name[64];
    bool flag_a, flag_b;
    int choice;
    int volume;
    int tab;
    KListState list;
    int clicks;
    KTextArea notes;
} Demo;

static const char *const g_items[] = { "Hakurei Shrine", "Scarlet Devil Mansion", "Youkai Mountain", "Bamboo Forest", "Human Village", "Netherworld", "Misty Lake", "Eientei", "Moriya Shrine", "Former Hell" };
static const KIcon g_icons[] = { KI_TORII, KI_HOME, KI_MONITOR, KI_BAMBOO, KI_FOLDER, KI_MOON, KI_DRIVE, KI_STAR, KI_TORII, KI_GEAR };

static char g_result[300] = "(nothing yet)";
static void on_text(const char *text, void *user) { (void)user; snprintf(g_result, sizeof g_result, "text: %s", text ? text : "(cancelled)"); }
static void on_file(const char *path, void *user) { (void)user; snprintf(g_result, sizeof g_result, "file: %s", path ? path : "(cancelled)"); }
static void on_yes(bool yes, void *user) { (void)user; snprintf(g_result, sizeof g_result, "confirm: %s", yes ? "yes" : "no"); }

static void draw(KApp *app, KUI *ui, void *user) {
    Demo *d = user;
    k_clear(ui);
    static const char *const tabs[] = { "Widgets", "Notes", "Icons", "Dialogs" };
    k_tabs(ui, kr(8, 8, ui->cv.w - 16, 26), tabs, 4, &d->tab);
    KRect body = kr(8, 34, ui->cv.w - 16, ui->cv.h - 42);
    kc_panel(&ui->cv, body, KB_FLAT, ui->theme->window_bg);
    if (d->tab == 0) {
        k_heading(ui, 20, 46, "Gensokyo widget gallery");
        if (k_button(ui, kr(20, 72, 100, 26), "Click me")) d->clicks++;
        char buf[64];
        snprintf(buf, sizeof buf, "clicked %d times", d->clicks);
        k_label(ui, 132, 78, buf);
        k_button_icon(ui, kr(20, 106, 150, 28), KI_GEAR, "Settings");
        k_icon_button(ui, kr(180, 106, 28, 28), KI_REFRESH, false);
        k_checkbox(ui, 20, 148, "Sakuya stops time", &d->flag_a);
        k_checkbox(ui, 20, 170, "Marisa borrows books", &d->flag_b);
        k_radio(ui, 20, 196, "Reimu", &d->choice, 0);
        k_radio(ui, 90, 196, "Marisa", &d->choice, 1);
        k_radio(ui, 170, 196, "Sakuya", &d->choice, 2);
        k_label(ui, 20, 226, "Name:");
        k_textfield(ui, kr(70, 222, 200, 24), d->name, sizeof d->name);
        k_label(ui, 20, 256, "Volume:");
        k_slider(ui, kr(80, 254, 190, 20), &d->volume, 0, 100);
        k_progress(ui, kr(20, 284, 250, 18), d->volume);
        k_list(ui, kr(290, 46, 230, 256), g_items, g_icons, 10, &d->list);
        if (d->list.selected >= 0) k_label(ui, 290, 310, g_items[d->list.selected]);
    } else if (d->tab == 1) {
        k_textarea(ui, kr(16, 44, body.w - 16, body.h - 20), &d->notes);
    } else if (d->tab == 3) {
        k_heading(ui, 20, 50, "Dialogs");
        if (k_button(ui, kr(20, 80, 160, 28), "Ask for text...")) k_input_dialog(app, "Rename", "New name for the spell card:", "Master Spark", on_text, NULL);
        if (k_button(ui, kr(20, 116, 160, 28), "Open a file...")) k_file_dialog(app, 0, "Open file", "/", "", on_file, NULL);
        if (k_button(ui, kr(20, 152, 160, 28), "Save a file...")) k_file_dialog(app, 1, "Save file", NULL, "untitled.txt", on_file, NULL);
        if (k_button(ui, kr(20, 188, 160, 28), "Choose a folder...")) k_file_dialog(app, 2, "Choose a folder", "/", "", on_file, NULL);
        if (k_button(ui, kr(20, 224, 160, 28), "Are you sure?")) k_confirm_dialog(app, "Confirm", "Move 3 files to the Netherworld? Yuyuko will keep them for a while.", KI_QUESTION, "Move", "Cancel", on_yes, NULL);
        kc_text_in(&ui->cv, kr(20, 270, ui->cv.w - 40, 20), g_result, ui->theme->accent_dark, KA_LEFT);
    } else {
        for (int i = 0; i < KI_COUNT; i++) {
            int x = 24 + (i % 11) * 48, y = 50 + (i / 11) * 64;
            k_icon(&ui->cv, (KIcon)i, x, y, 32);
            kc_text_in(&ui->cv, kr(x - 8, y + 34, 48, 16), k_icon_name((KIcon)i), ui->theme->text_dim, KA_CENTER);
        }
    }
}

int main(void) {
    Demo d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "Reimu Hakurei");
    d.volume = 60;
    k_list_init(&d.list);
    kta_init(&d.notes, 4096);
    const char *intro = "Patchouli's notes\n\nThis is a multi-line text area.\n\tTabs work.\nTry typing, selecting and scrolling.\n";
    kta_set(&d.notes, intro, strlen(intro));
    KApp *app = kapp_new("Kappa Gallery", 540, 360, FW_RESIZABLE);
    if (!app) return 1;
    kapp_set_draw(app, draw, &d);
    kapp_set_minsize(app, 400, 300);
    return kapp_run(app);
}
