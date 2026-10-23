/*
 * calculator: Ran's Calculator.
 *
 *   calculator                       the desktop application
 *   calculator --eval EXPR...        print the value of each expression (for scripts and tests); --rad / --deg pick the angle unit
 *
 * "ans" in the second expression of a --eval run is the result of the first.
 */
#include "engine.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

static int run_eval(int argc, char **argv) {
    CalcContext ctx = { .degrees = true, .ans = 0 };
    int status = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--eval")) continue;
        if (!strcmp(a, "--rad")) { ctx.degrees = false; continue; }
        if (!strcmp(a, "--deg")) { ctx.degrees = true; continue; }
        any = 1;
        cnum v;
        CalcError err;
        if (calc_eval(&ctx, a, &v, &err)) {
            char text[64];
            calc_format(v, text, sizeof text);
            puts(text);
            ctx.ans = v;
        } else {
            printf("error: %s\n", err.msg);
            status = 1;
        }
    }
    if (!any) { fprintf(stderr, "usage: calculator --eval [--rad|--deg] EXPRESSION...\n"); return 2; }
    return status;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--eval")) return run_eval(argc, argv);
    return calc_gui(argc, argv);
}
