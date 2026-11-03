/*
 * kappa-warp: put the mouse pointer at an exact position (a developer tool for scripted desktop tests).
 *
 *   kappa-warp X Y
 *
 * The window server moves its cursor; real clicks from the input device then land exactly there. The program waits for the
 * server's answer to a follow-up request, so when it prints "warped" the move has been carried out.
 */
#include <fairy.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: kappa-warp X Y\n"); return 2; }
    int x = atoi(argv[1]), y = atoi(argv[2]);
    FairyDisplay *d = fairy_connect("kappa-warp");
    if (!d) { fprintf(stderr, "kappa-warp: no window server answered\n"); return 1; }
    fairy_warp_pointer(d, x, y);
    fairy_clipboard_request(d);                       /* requests are answered in order: the reply proves the warp happened */
    FairyEvent ev;
    for (int i = 0; i < 40; i++) {
        int r = fairy_next_event(d, &ev, 250);
        if (r < 0 || (r == 1 && ev.type == FEV_CLIPBOARD)) break;
    }
    fairy_disconnect(d);
    printf("warped %d %d\n", x, y);
    return 0;
}
