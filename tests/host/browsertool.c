/*
 * browsertool: drives the browser's own glue (user/bin/browser: fonts.c, paint.c, ...) from the command line, for the host
 * tests (tests/host/test_browser.py) and for looking at pages without booting the OS.
 *
 *   browsertool render PAGE.html OUT.png [W [H [SCALE [SCROLL_Y]]]]
 *       parses the page with the real font provider and the real painter, lays it out for W x H CSS pixels (default 800 x 600),
 *       paints the first screen (from SCROLL_Y) at SCALE device pixels per CSS pixel and writes a PNG. Style sheets and pictures
 *       the page names are read from the page's own folder. Prints one line: size, items, content size and a checksum of the pixels.
 *   browsertool measure FAMILY WEIGHT STYLE SIZE TEXT
 *       prints the face, the metrics and the width the provider gives for TEXT (SIZE in CSS px, WEIGHT 400 or 700, STYLE 0 or 1)
 *   browsertool text OUT.png SIZE SCALE FAMILY TEXT...
 *       draws one line of text on a white background
 *   browsertool session [SCRIPT]
 *       runs a whole browsing session through the browser core (core.h) without a window: one command per line from SCRIPT or
 *       standard input, one answer line per command. Commands: size W H [SCALE] (before the first go), go TEXT, wait [SECONDS],
 *       shot OUT.png, pixel X Y, count X Y W H RRGGBB [TOL], click X Y [CLICKS] (or click @ID), down X Y [CLICKS], where @ID, up X Y, move X Y [BUTTONS], wheel X Y TICKS, key NAME [ctrl] [shift] [alt],
 *       type TEXT, find TEXT [DIR], findend, copy, selall, scroll Y, back, forward, reload, stop, url, title, status, text (the
 *       selection), value ID (a form control's value), content, scrolly, busy, cursor, html, lock
 */
#include <web.h>
#include <img.h>
#include <keys.h>
#include "../../user/bin/browser/fonts.h"
#include "../../user/bin/browser/paint.h"
#include "../../user/bin/browser/core.h"
#include "../../user/bin/browser/urls.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *b = malloc((size_t)len + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)len, f);
    fclose(f);
    b[got] = 0;
    if (n) *n = got;
    return b;
}

/* ---- a loader that answers from files, one step after the request (the engine does not take answers from inside fetch) ---- */

typedef struct Pending { uint32_t id; char url[1024]; } Pending;
static Pending g_pending[256];
static int g_npending;
static uint32_t g_next_id = 1;

static uint32_t t_fetch(void *user, WebDoc *doc, const char *url, WebResourceKind kind) {
    (void)user; (void)doc; (void)kind;
    if (g_npending >= 256) return 0;
    Pending *p = &g_pending[g_npending++];
    p->id = g_next_id++;
    snprintf(p->url, sizeof p->url, "%s", url);
    return p->id;
}

static void answer_pending(WebDoc *doc) {
    for (int guard = 0; guard < 100 && g_npending; guard++) {
        Pending list[256];
        int n = g_npending;
        memcpy(list, g_pending, (size_t)n * sizeof list[0]);
        g_npending = 0;
        for (int i = 0; i < n; i++) {
            const char *path = list[i].url;
            if (!strncmp(path, "file://", 7)) path += 7;
            size_t len = 0;
            char *data = read_file(path, &len);
            web_doc_resource_done(doc, list[i].id, data ? 0 : -1, data, len, NULL);
            free(data);
        }
    }
}

static const WebLoader g_loader = { NULL, t_fetch, NULL };

static uint64_t checksum(const BrSurface *s) {
    uint64_t sum = 1469598103934665603ull;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) sum = (sum ^ (s->px[(size_t)y * (size_t)s->stride + (size_t)x] & 0xFFFFFFu)) * 1099511628211ull;
    return sum;
}

static int save_png(const char *path, const BrSurface *s) {
    Image im = { s->w, s->h, malloc((size_t)s->w * (size_t)s->h * 4), 0 };
    if (!im.px) return -1;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) im.px[(size_t)y * (size_t)s->w + (size_t)x] = 0xFF000000u | s->px[(size_t)y * (size_t)s->stride + (size_t)x];
    int rc = img_save_png(path, &im, 6);
    free(im.px);
    return rc;
}

static int cmd_render(int argc, char **argv) {
    if (argc < 4) return 2;
    int w = argc > 4 ? atoi(argv[4]) : 800, h = argc > 5 ? atoi(argv[5]) : 600, scale = argc > 6 ? atoi(argv[6]) : 1;
    int scroll = argc > 7 ? atoi(argv[7]) : 0;
    size_t n = 0;
    char *html = read_file(argv[2], &n);
    if (!html) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    BrFonts *fonts = brf_new(NULL);
    BrPainter *painter = brp_new(fonts);
    WebConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fonts = brf_provider(fonts);
    cfg.loader = &g_loader;
    cfg.images = web_image_libimg();
    cfg.scale = scale;
    char url[1100];
    char *abs = realpath(argv[2], NULL);
    snprintf(url, sizeof url, "file://%s", abs ? abs : argv[2]);
    free(abs);
    WebDoc *doc = web_doc_new(&cfg, url);
    web_doc_parse(doc, html, n, NULL);
    answer_pending(doc);
    web_doc_layout(doc, w, h);
    answer_pending(doc);
    web_doc_layout(doc, w, h);
    int cw, ch;
    web_doc_content_size(doc, &cw, &ch);
    const WebDisplayList *dl = web_doc_paint(doc, 0, scroll, w, h);
    BrSurface s = { calloc((size_t)(w * scale) * (size_t)(h * scale), 4), w * scale, h * scale, w * scale };
    BrPaintOpts o;
    memset(&o, 0, sizeof o);
    o.scale = scale;
    o.fill_background = true;
    brp_render(painter, &s, dl, &o);
    int rc = save_png(argv[3], &s);
    printf("size=%dx%d items=%d content=%dx%d files=%d checksum=%016llx rc=%d\n", s.w, s.h, dl->n, cw, ch, brf_file_count(fonts),
           (unsigned long long)checksum(&s), rc);
    free(s.px);
    web_doc_free(doc);
    brp_free(painter);
    brf_free(fonts);
    free(html);
    return rc ? 1 : 0;
}

static int cmd_measure(int argc, char **argv) {
    if (argc < 7) return 2;
    BrFonts *f = brf_new(NULL);
    const char *fam[1] = { argv[2] };
    WebFontDesc d;
    memset(&d, 0, sizeof d);
    d.families = fam;
    d.nfamilies = 1;
    d.weight = atoi(argv[3]);
    d.style = atoi(argv[4]);
    d.size = (WebUnit)(atof(argv[5]) * 64.0 + 0.5);
    const WebFontProvider *p = brf_provider(f);
    WebFontHandle h = p->match(p->user, &d);
    BrFaceInfo info;
    WebFontMetrics m;
    p->metrics(p->user, h, &m);
    brf_face_info(f, h, &info);
    printf("kind=%d size=%.3f bold=%d italic=%d mono=%d file=%s ascent=%d descent=%d gap=%d xh=%d zero=%d space=%d uoff=%d uthick=%d width=%d\n", info.kind,
           (double)info.size, info.synth_bold, info.synth_italic, info.mono, info.file, m.ascent, m.descent, m.line_gap, m.x_height, m.zero_advance,
           m.space_advance, m.underline_offset, m.underline_thickness, p->measure(p->user, h, argv[6], strlen(argv[6])));
    brf_free(f);
    return 0;
}

static int cmd_text(int argc, char **argv) {
    if (argc < 7) return 2;
    float size = (float)atof(argv[3]);
    int scale = atoi(argv[4]);
    BrFonts *f = brf_new(NULL);
    const char *fam[1] = { argv[5] };
    WebFontDesc d;
    memset(&d, 0, sizeof d);
    d.families = fam;
    d.nfamilies = 1;
    d.weight = 400;
    d.size = (WebUnit)(size * 64.0f + 0.5f);
    const WebFontProvider *p = brf_provider(f);
    char text[2048] = "";
    for (int i = 6; i < argc; i++) { if (i > 6) strcat(text, " "); strncat(text, argv[i], sizeof text - strlen(text) - 2); }
    WebFontHandle h = p->match(p->user, &d);
    int tw = (int)((int64_t)p->measure(p->user, h, text, strlen(text)) * scale / 64) + 20 * scale;
    int th = (int)(size * 1.6f * (float)scale) + 8;
    BrSurface s = { malloc((size_t)tw * (size_t)th * 4), tw, th, tw };
    for (int i = 0; i < tw * th; i++) s.px[i] = 0xFFFFFF;
    brf_draw_text(f, &s, br_rect(0, 0, tw, th), h, scale, text, strlen(text), 10 * scale * 64, (int)(size * 1.1f * (float)scale) * 64, 0xFF000000u, 255);
    int rc = save_png(argv[2], &s);
    int x0 = tw, x1 = -1;                                          /* the columns that hold ink */
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            if ((s.px[(size_t)y * (size_t)tw + (size_t)x] & 0xFF) < 200) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
    printf("size=%dx%d checksum=%016llx ink_x0=%d ink_x1=%d width=%d\n", tw, th, (unsigned long long)checksum(&s), x0, x1,
           (int)((int64_t)p->measure(p->user, h, text, strlen(text)) * scale / 64));
    free(s.px);
    brf_free(f);
    return rc ? 1 : 0;
}


/* ---------------------------------------------------------------- a browsing session without a window */

static char g_clip[8192];
static size_t g_cliplen;
static void s_invalidate(void *user) { (void)user; }
static void s_clipboard(void *user, const char *text, size_t len) {
    (void)user;
    g_cliplen = len < sizeof g_clip - 1 ? len : sizeof g_clip - 1;
    memcpy(g_clip, text, g_cliplen);
    g_clip[g_cliplen] = 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void render_now(BrCore *c);

static void settle(BrCore *c, double seconds) {                  /* pump and lay out until nothing is loading, or the time is up */
    uint64_t end = now_ms() + (uint64_t)(seconds * 1000);
    int quiet = 0;
    do {
        struct pollfd pf = { brc_wake_fd(c), POLLIN, 0 };
        poll(&pf, 1, 20);
        brc_pump(c, now_ms());
        render_now(c);                                          /* layout is what makes the engine ask for stylesheets and pictures */
        quiet = brc_busy(c) ? 0 : quiet + 1;
    } while (quiet < 3 && now_ms() < end);
}

static int key_code(const char *name) {
    static const struct { const char *n; int code; } k[] = {
        { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "pgup", KEY_PAGEUP }, { "pgdn", KEY_PAGEDOWN },
        { "home", KEY_HOME }, { "end", KEY_END }, { "space", KEY_SPACE }, { "tab", KEY_TAB }, { "enter", KEY_ENTER }, { "esc", KEY_ESC },
        { "backspace", KEY_BACKSPACE }, { "delete", KEY_DELETE }, { "a", KEY_A }, { "c", KEY_C }, { "x", KEY_X }, { "v", KEY_V }, { NULL, 0 } };
    for (int i = 0; k[i].n; i++) if (!strcmp(k[i].n, name)) return k[i].code;
    return 0;
}

static BrSurface g_surf;

static void render_now(BrCore *c) {                              /* paints the page into a surface that stays for pixel, count and shot */
    int w = c->vw * c->scale, h = c->vh * c->scale;
    if (g_surf.w != w || g_surf.h != h) {
        free(g_surf.px);
        g_surf.px = calloc((size_t)w * (size_t)h, 4);
        g_surf.w = g_surf.stride = w;
        g_surf.h = h;
    }
    brc_render(c, &g_surf, now_ms() + 100000);                   /* a late "now": the loading throttle never skips a picture that was asked for */
}

static void shot(BrCore *c, const char *path) {
    render_now(c);
    int rc = save_png(path, &g_surf);
    printf("shot %s %dx%d checksum=%016llx rc=%d\n", path, g_surf.w, g_surf.h, (unsigned long long)checksum(&g_surf), rc);
}

/* "@id" in place of "X Y": the middle of the element with that id, in viewport pixels; rest points past it */
static bool at_element(BrCore *c, const char *arg, int *x, int *y, const char **rest) {
    if (arg[0] != '@') return false;
    char id[128];
    size_t n = strcspn(arg + 1, " ");
    snprintf(id, sizeof id, "%.*s", (int)(n < sizeof id - 1 ? n : sizeof id - 1), arg + 1);
    *rest = arg + 1 + n;
    if (!c->page || !c->page->doc) return true;
    web_doc_layout(c->page->doc, c->vw, c->vh);
    WebNode *el = web_doc_get_element_by_id(c->page->doc, id);
    WebRect r;
    if (el && web_element_rect(el, &r)) {
        *x = (r.x + r.w / 2) / 64 - c->page->scroll_x;
        *y = (r.y + r.h / 2) / 64 - c->page->scroll_y;
    } else { *x = -1; *y = -1; }
    return true;
}

static int cmd_session(int argc, char **argv) {
    FILE *in = argc > 2 ? fopen(argv[2], "r") : stdin;
    if (!in) { perror(argv[2]); return 1; }
    BrHost host = { NULL, s_invalidate, s_clipboard };
    BrCore *core = NULL;
    int vw = 800, vh = 600, scale = 1;
    char line[4096];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *cmd = line, *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = line + n;
        if (!strcmp(cmd, "size")) {
            int w = 0, h = 0, sc = 1;
            sscanf(arg, "%d %d %d", &w, &h, &sc);
            if (w > 0 && h > 0) { vw = w; vh = h; }
            if (sc > 0) scale = sc;
            if (core) brc_set_viewport(core, vw, vh);
            printf("size %dx%d scale %d\n", vw, vh, scale);
            continue;
        }
        if (!core) {
            core = brc_new(&host, scale);
            if (!core) { printf("error: no core\n"); return 1; }
            brc_set_viewport(core, vw, vh);
        }
        BrCore *c = core;
        c->now_ms = now_ms();
        if (!strcmp(cmd, "go")) { brc_go(c, arg); printf("go %s\n", brc_url(c)); }
        else if (!strcmp(cmd, "wait")) { settle(c, *arg ? atof(arg) : 20.0); printf("wait busy=%d\n", brc_busy(c)); }
        else if (!strcmp(cmd, "shot")) shot(c, arg);
        else if (!strcmp(cmd, "pixel")) {                                                  /* pixel X Y: the colour at a device pixel of the last picture */
            int x = 0, y = 0;
            sscanf(arg, "%d %d", &x, &y);
            render_now(c);
            uint32_t v = x >= 0 && y >= 0 && x < g_surf.w && y < g_surf.h ? g_surf.px[(size_t)y * (size_t)g_surf.stride + (size_t)x] & 0xFFFFFF : 0xFFFFFFFF;
            printf("pixel %d %d %06x\n", x, y, v);
        }
        else if (!strcmp(cmd, "count")) {                                                  /* count X Y W H RRGGBB TOL: pixels near a colour in a rectangle */
            int x = 0, y = 0, w = 0, h = 0, tol = 24;
            unsigned col = 0;
            sscanf(arg, "%d %d %d %d %x %d", &x, &y, &w, &h, &col, &tol);
            render_now(c);
            long n = 0;
            for (int j = y; j < y + h && j < g_surf.h; j++)
                for (int i = x; i < x + w && i < g_surf.w; i++) {
                    uint32_t v = g_surf.px[(size_t)j * (size_t)g_surf.stride + (size_t)i];
                    if (abs((int)((v >> 16) & 255) - (int)((col >> 16) & 255)) <= tol && abs((int)((v >> 8) & 255) - (int)((col >> 8) & 255)) <= tol &&
                        abs((int)(v & 255) - (int)(col & 255)) <= tol) n++;
                }
            printf("count %ld\n", n);
        }
        else if (!strcmp(cmd, "click") || !strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "move")) {
            int x = 0, y = 0, k = !strcmp(cmd, "move") ? 0 : 1;                                      /* the last number: buttons held (move) or clicks */
            const char *rest = NULL;
            if (at_element(c, arg, &x, &y, &rest)) sscanf(rest, "%d", &k);
            else sscanf(arg, "%d %d %d", &x, &y, &k);
            if (!strcmp(cmd, "move")) { int cur = brc_pointer_move(c, x, y, (unsigned)k); printf("move cursor=%d status=%s\n", cur, brc_status(c)); }
            else {
                if (strcmp(cmd, "up")) brc_pointer_down(c, x, y, 1, k);
                if (strcmp(cmd, "down")) brc_pointer_up(c, x, y, 1);
                printf("%s url=%s\n", cmd, brc_url(c));
            }
        }
        else if (!strcmp(cmd, "wheel")) {
            int x = 0, y = 0, t = 0;
            const char *rest = NULL;
            if (at_element(c, arg, &x, &y, &rest)) sscanf(rest, "%d", &t);
            else sscanf(arg, "%d %d %d", &x, &y, &t);
            brc_wheel(c, x, y, t);
            printf("wheel scrolly=%d\n", brc_scroll_y(c));
        }
        else if (!strcmp(cmd, "where")) {                                                  /* where @id: the element's box in viewport pixels */
            int x = 0, y = 0;
            const char *rest = NULL;
            at_element(c, arg, &x, &y, &rest);
            printf("where %d %d\n", x, y);
        }
        else if (!strcmp(cmd, "key")) {
            char name[64] = "";
            sscanf(arg, "%63s", name);
            int mods = 0;
            if (strstr(arg, " ctrl")) mods |= MOD_CTRL;
            if (strstr(arg, " shift")) mods |= MOD_SHIFT;
            if (strstr(arg, " alt")) mods |= MOD_ALT;
            uint32_t ch = name[1] == 0 && name[0] ? (uint32_t)(unsigned char)name[0] : 0;
            if (!strcmp(name, "space")) ch = ' ';
            bool used = brc_key(c, (uint8_t)key_code(name), (uint8_t)mods, (mods & MOD_CTRL) ? 0 : ch);
            printf("key %s used=%d url=%s\n", name, used, brc_url(c));
        }
        else if (!strcmp(cmd, "type")) {
            int used = 0;
            for (const char *t = arg; *t; t++) used += brc_key(c, 0, 0, (uint32_t)(unsigned char)*t) ? 1 : 0;
            printf("type used=%d\n", used);
        }
        else if (!strcmp(cmd, "find")) {
            char text[256];
            int dir = 0;
            const char *sp = strrchr(arg, ' ');
            snprintf(text, sizeof text, "%s", arg);
            if (sp && (!strcmp(sp + 1, "1") || !strcmp(sp + 1, "-1"))) { dir = atoi(sp + 1); text[sp - arg] = 0; }
            int cnt = brc_find(c, text, dir);
            printf("find count=%d index=%d scrolly=%d\n", cnt, brc_find_index(c), brc_scroll_y(c));
        }
        else if (!strcmp(cmd, "findend")) { brc_find_done(c); printf("findend\n"); }
        else if (!strcmp(cmd, "copy")) {
            g_cliplen = 0;
            g_clip[0] = 0;
            bool ok = brc_copy(c);
            for (char *q = g_clip; *q; q++) if (*q == '\n') *q = '|';
            printf("copy ok=%d text=%s\n", ok, g_clip);
        }
        else if (!strcmp(cmd, "selall")) { brc_select_all(c); printf("selall\n"); }
        else if (!strcmp(cmd, "scroll")) { brc_scroll_to(c, atoi(arg)); printf("scroll y=%d\n", brc_scroll_y(c)); }
        else if (!strcmp(cmd, "back")) { brc_back(c); printf("back\n"); }
        else if (!strcmp(cmd, "forward")) { brc_forward(c); printf("forward\n"); }
        else if (!strcmp(cmd, "reload")) { brc_reload(c); printf("reload\n"); }
        else if (!strcmp(cmd, "stop")) { brc_stop(c); printf("stop\n"); }
        else if (!strcmp(cmd, "url")) printf("url %s\n", brc_url(c));
        else if (!strcmp(cmd, "title")) { settle(c, 0); printf("title %s\n", brc_title(c)); }
        else if (!strcmp(cmd, "status")) printf("status %s\n", brc_status(c));
        else if (!strcmp(cmd, "text")) {
            size_t len = 0;
            char *t = c->page && c->page->doc ? web_doc_selection_text(c->page->doc, &len) : NULL;
            for (char *q = t; q && *q; q++) if (*q == '\n') *q = '|';
            printf("text %s\n", t ? t : "");
            if (t) web_free(c->page->doc, t);
        }
        else if (!strcmp(cmd, "value")) {
            WebNode *el = c->page && c->page->doc ? web_doc_get_element_by_id(c->page->doc, arg) : NULL;
            const char *v = el ? web_control_value(el) : NULL;
            printf("value %s=%s checked=%d\n", arg, v ? v : "(none)", el ? web_control_checked(el) : -1);
        }
        else if (!strcmp(cmd, "content")) { int w, h; brc_content_size(c, &w, &h); printf("content %dx%d\n", w, h); }
        else if (!strcmp(cmd, "scrolly")) printf("scrolly %d\n", brc_scroll_y(c));
        else if (!strcmp(cmd, "busy")) printf("busy %d\n", brc_busy(c));
        else if (!strcmp(cmd, "lock")) {
            char text[240];
            BrSecurity sec = brc_security(c, text, sizeof text);
            printf("lock %s %s\n", sec == BR_SEC_SECURE ? "secure" : sec == BR_SEC_PLAIN ? "plain" : "none", text);
        }
        else if (!strcmp(cmd, "cursor")) printf("cursor %d\n", c->page ? c->page->cursor : -1);
        else if (!strcmp(cmd, "html")) {                                                   /* the page's text as the engine sees it, for tests */
            size_t len = 0;
            char *t = c->page && c->page->doc && web_doc_body(c->page->doc) ? web_node_text_content(web_doc_body(c->page->doc), &len) : NULL;
            for (char *q = t; q && *q; q++) if (*q == '\n') *q = ' ';
            printf("html %s\n", t ? t : "");
            if (t) web_free(c->page->doc, t);
        }
        else printf("error: unknown command %s\n", cmd);
    }
    if (core) brc_free(core);
    return 0;
}

/* browsertool urlinput CWD HOME TEXT | filepath URL | fileurl PATH | display URL MAX | kind URL | samedoc A B */
static int cmd_url(int argc, char **argv) {
    char out[BR_MAX_URL];
    if (!strcmp(argv[1], "urlinput") && argc >= 5) {
        bool ok = br_url_from_input(argv[4], argv[2], argv[3], out, sizeof out);
        printf("%s\n", ok ? out : "FAIL");
    } else if (!strcmp(argv[1], "filepath") && argc >= 3) {
        printf("%s\n", br_file_url_to_path(argv[2], out, sizeof out) ? out : "FAIL");
    } else if (!strcmp(argv[1], "fileurl") && argc >= 3) {
        br_path_to_file_url(argv[2], out, sizeof out);
        printf("%s\n", out);
    } else if (!strcmp(argv[1], "display") && argc >= 4) {
        br_url_display(argv[2], out, sizeof out, (size_t)atoi(argv[3]));
        printf("%s\n", out);
    } else if (!strcmp(argv[1], "kind") && argc >= 3) {
        printf("%d\n", (int)br_url_kind(argv[2]));
    } else if (!strcmp(argv[1], "samedoc") && argc >= 4) {
        printf("%d\n", br_url_same_document(argv[2], argv[3]) ? 1 : 0);
    } else return 2;
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "urlinput") || !strcmp(argv[1], "filepath") || !strcmp(argv[1], "fileurl") || !strcmp(argv[1], "display") ||
                      !strcmp(argv[1], "kind") || !strcmp(argv[1], "samedoc"))) return cmd_url(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "render")) return cmd_render(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "measure")) return cmd_measure(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "text")) return cmd_text(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "session")) return cmd_session(argc, argv);
    fprintf(stderr, "usage: browsertool render|measure|text ...\n");
    return 2;
}
