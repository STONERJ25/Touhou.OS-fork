/* popuptest X Y W H: opens a plain red popup window (what a menu is: no frame, top layer, never focused) at that position of the screen and stays until it is
 * closed or killed. tests/case_tooltip.py uses it to check that the window server keeps a popup on the screen when a client asks for a place beyond an edge. */
#include <kappa.h>
#include <stdio.h>
#include <stdlib.h>

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    kc_fill(&ui->cv, kr(0, 0, ui->cv.w, ui->cv.h), 0xFF0000);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: popuptest X Y W H\n"); return 2; }
    KApp *app = kapp_new_at("popuptest", atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), FW_POPUP | FW_NO_DECOR);
    if (!app) return 1;
    kapp_set_draw(app, draw, NULL);
    return kapp_run(app);
}
