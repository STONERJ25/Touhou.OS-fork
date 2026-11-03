/*
 * Pages the browser makes itself: error pages, the blank page and about:browser. The joke sits on top of the mechanism (docs/THEMING.md):
 * every error page says in plain words what happened and shows the exact sentence from the library that found it, and Kyouko,
 * the mountain echo, comments on it. There is no "continue anyway" for a certificate that was refused.
 */
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buf { char *p; size_t n, cap; } Buf;

static void put(Buf *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + l + 1) cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

static void put_escaped(Buf *b, const char *s) {
    char tmp[2];
    tmp[1] = 0;
    for (; *s; s++) {
        switch (*s) {
        case '<': put(b, "&lt;"); break;
        case '>': put(b, "&gt;"); break;
        case '&': put(b, "&amp;"); break;
        case '"': put(b, "&quot;"); break;
        default: tmp[0] = *s; put(b, tmp);
        }
    }
}

static const char *const STYLE =
    "<style>"
    "body{margin:0;background:#0e1330;color:#f4efd8;font-family:sans-serif;font-size:16px}"
    ".card{max-width:620px;margin:44px auto;padding:22px 28px 26px;background:#1d2a5c;border:2px solid #d9a441;border-radius:8px}"
    "h1{margin:0 0 6px;font-size:26px;font-weight:bold;color:#f4efd8}"
    ".joke{margin:0 0 16px;color:#d9a441;font-style:italic}"
    ".what{margin:0 0 14px}"
    ".fact{margin:0 0 14px;padding:9px 13px;background:#0e1330;border-left:4px solid #c8102e;font-family:monospace;white-space:pre-wrap;word-wrap:break-word}"
    ".url{color:#9aa3d0}"
    "a{color:#f4efd8}"
    ".small{margin-top:18px;font-size:13px;color:#9aa3d0}"
    "</style>";

typedef struct Entry {
    const char *kind, *title, *joke, *what;
} Entry;

static const Entry ENTRIES[] = {
    { "dns", "Nobody lives at that address",
      "Kyouko shouted the name into the valley, and no one by that name answered.",
      "The name of the server could not be turned into an address. Check the spelling, or whether this computer has a name server (the network panel shows it)." },
    { "connect", "The echo never came back",
      "Kyouko called across the valley, but nothing called back.",
      "The server did not accept the connection (nothing listens on that port, or the way there is closed)." },
    { "timeout", "The mountain is asleep",
      "Kyouko waited, and waited, and the valley stayed silent.",
      "The server did not answer in time. The browser waits for a while and then gives up so that the window never hangs." },
    { "tls", "That voice is not who it claims to be",
      "Eiki has made a note of it. Kyouko will not pass this one on.",
      "The secure connection was refused because the server's certificate did not pass the checks below. There is no way to continue anyway: a page that cannot prove who it is is not shown." },
    { "notrust", "The shrine keeps no list of trusted voices",
      "Without the register Kyouko cannot tell a friend from a fox.",
      "Secure pages need a bundle of trusted root certificates, and there is none on this computer." },
    { "io", "The echo came back in pieces",
      "Something cut the valley's voice short.",
      "The connection broke while the page was arriving." },
    { "truncated", "The echo was cut off",
      "Half a shout is worse than none, says Kyouko.",
      "The connection ended before the whole page had arrived, so it may be incomplete." },
    { "parse", "The echo came back garbled",
      "Kyouko cannot make anything of it.",
      "The server's answer is not valid HTTP (or its compressed body is damaged)." },
    { "redirects", "Lost in the bamboo forest",
      "Every path led to another path. Kyouko has stopped following.",
      "The server sent the browser on from address to address more times than the browser follows." },
    { "toobig", "That echo is too long to hear",
      "Even a yamabiko has to draw breath.",
      "The answer is larger than the browser reads. The limits are in docs/BROWSER.md." },
    { "notfound", "Nothing here but the wind",
      "Kyouko looked everywhere she could think of.",
      "There is no such file or folder." },
    { "access", "Eiki says that is not yours to open",
      "Kyouko may not echo what the judge has sealed.",
      "The file exists, but this user is not allowed to read it." },
    { "scheme", "Kyouko does not speak that language",
      "She only knows http, https, file and data.",
      "The address uses a scheme this browser cannot fetch." },
    { "url", "That is not an address",
      "Even an echo needs something to repeat.",
      "The text is not a web address or the path of a file." },
    { "unsupported", "Kyouko can only echo pages",
      "This one is not something she knows how to repeat yet.",
      "The browser can show web pages, plain text and pictures (PNG, JPEG, GIF, BMP). This is something else, so it was not downloaded." },
    { "post", "Kyouko can only ask, not send",
      "Forms that post their answers are beyond the valley for now.",
      "This form sends its data with POST. The browser only makes GET requests so far (a form with method=get works)." },
    { "script", "Scripts are not read here yet",
      "Kosuzu has not been introduced to this page.",
      "The link runs JavaScript, which this version of the browser does not do." },
    { "mail", "No messenger is waiting",
      "Kyouko cannot carry letters.",
      "The browser does not hand mailto: links to a mail program." },
};

static const Entry *entry_for(const char *kind) {
    for (size_t i = 0; i < sizeof ENTRIES / sizeof ENTRIES[0]; i++) if (strcmp(ENTRIES[i].kind, kind) == 0) return &ENTRIES[i];
    return NULL;
}

char *br_error_page(const BrPageInfo *info, size_t *len) {
    Buf b = { 0 };
    char tmp[160];
    const char *title, *joke, *what;
    if (strcmp(info->kind, "http") == 0) {
        int st = info->status;
        title = st == 404 ? "No such shrine" : st == 403 ? "The gate is shut" : st == 401 ? "The gate wants a password" : st >= 500 ? "The other side is having a bad day"
                : st >= 400 ? "The server turned the request down" : "The page came back empty";
        joke = st == 404 ? "Kyouko echoes only what is really there." : st >= 500 ? "Something went wrong in the valley, and it was not Kyouko's fault." : "Kyouko asked politely.";
        snprintf(tmp, sizeof tmp, "The server answered %d%s and sent no page.", st, st == 404 ? " (not found)" : st == 403 ? " (forbidden)" : st == 401 ? " (unauthorized)" : "");
        what = tmp;
    } else {
        const Entry *e = entry_for(info->kind);
        if (!e) e = entry_for("io");
        title = e->title;
        joke = e->joke;
        what = e->what;
    }
    put(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title>");
    put_escaped(&b, title);
    put(&b, "</title>");
    put(&b, STYLE);
    put(&b, "</head><body><div class=\"card\"><h1>");
    put_escaped(&b, title);
    put(&b, "</h1><p class=\"joke\">");
    put_escaped(&b, joke);
    put(&b, "</p><p class=\"what\">");
    put_escaped(&b, what);
    put(&b, "</p>");
    if (info->detail && *info->detail) {
        put(&b, "<div class=\"fact\">");
        put_escaped(&b, info->detail);
        put(&b, "</div>");
    }
    if (info->extra && *info->extra) {
        put(&b, "<p class=\"what\">");
        put_escaped(&b, info->extra);
        put(&b, "</p>");
    }
    if (info->url && *info->url && strncmp(info->url, "about:", 6) != 0) {
        put(&b, "<p class=\"url\">");
        put_escaped(&b, info->url);
        put(&b, "</p>");
        if (strcmp(info->kind, "url") != 0 && strcmp(info->kind, "script") != 0 && strcmp(info->kind, "mail") != 0 && strcmp(info->kind, "post") != 0) {
            put(&b, "<p><a href=\"");
            put_escaped(&b, info->url);
            put(&b, "\">Try again</a></p>");
        }
    }
    put(&b, "</div></body></html>");
    if (len) *len = b.n;
    return b.p;
}

char *br_blank_page(size_t *len) {
    Buf b = { 0 };
    put(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title></title></head><body></body></html>");
    if (len) *len = b.n;
    return b.p;
}

char *br_about_page(size_t *len, int font_files, const char *font_dir) {
    Buf b = { 0 };
    char tmp[256];
    put(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title>About Kyouko's browser</title>");
    put(&b, STYLE);
    put(&b, "<style>td{padding:2px 14px 2px 0;vertical-align:top}th{text-align:left;padding:8px 14px 2px 0;color:#d9a441}</style></head><body><div class=\"card\">");
    put(&b, "<h1>Kyouko's browser</h1><p class=\"joke\">Kyouko Kasodani is the yamabiko, the mountain echo: she shouts an address into the valley and what answers comes back.</p>");
    put(&b, "<p class=\"what\">That is what a browser does with a URL, so the first TouhouOS browser is hers. Behind her stand Hatate, who looks at the page through her camera "
            "(the engine: HTML, CSS and layout), Kosuzu, who will read the scripts (the JavaScript engine, not connected yet), and Yukari, who opens the gaps the packets travel through.</p>");
    put(&b, "<table>");
    put(&b, "<tr><th colspan=\"2\">Keys</th></tr>"
            "<tr><td>Ctrl+L, F6</td><td>the address field</td></tr><tr><td>Alt+Left, Alt+Right</td><td>back and forward</td></tr>"
            "<tr><td>F5, Ctrl+R</td><td>reload</td></tr><tr><td>Esc</td><td>stop loading; close find</td></tr>"
            "<tr><td>Ctrl+F, F3</td><td>find in the page; Enter or F3 for the next match, Shift+F3 for the previous</td></tr>"
            "<tr><td>Ctrl+A, Ctrl+C</td><td>select all, copy</td></tr>"
            "<tr><td>Space, PgUp, PgDn, Home, End, arrows</td><td>scroll</td></tr><tr><td>Tab</td><td>next form control</td></tr>");
    put(&b, "<tr><th colspan=\"2\">What it reads</th></tr>"
            "<tr><td>Addresses</td><td>http://, https:// (certificates always checked), file:// and plain paths, data:, about:blank</td></tr>"
            "<tr><td>Content</td><td>HTML with CSS (no JavaScript), plain text, PNG, JPEG, GIF, BMP, TGA and PNM pictures</td></tr>"
            "<tr><td>Forms</td><td>method=get; text fields, areas, check boxes, radio buttons, selects</td></tr>");
    snprintf(tmp, sizeof tmp, "a page up to %u MiB, a stylesheet or picture up to %u MiB, %u requests per page, %d seconds of silence, %d seconds per download, %u MiB of memory for the document",
             (unsigned)(BR_MAX_DOC_BYTES >> 20), (unsigned)(BR_MAX_SUB_BYTES >> 20), (unsigned)BR_MAX_SUBRES, BR_TIMEOUT_MS / 1000, BR_DEADLINE_MS / 1000, (unsigned)(BR_MEMORY_LIMIT >> 20));
    put(&b, "<tr><th colspan=\"2\">Limits</th></tr><tr><td>Per page</td><td>");
    put_escaped(&b, tmp);
    put(&b, "</td></tr>");
    snprintf(tmp, sizeof tmp, "%d TrueType file%s found in %s", font_files, font_files == 1 ? "" : "s", font_dir ? font_dir : "the font folder");
    put(&b, "<tr><th colspan=\"2\">Fonts</th></tr><tr><td>Gensokyo Sans</td><td>");
    put_escaped(&b, tmp);
    put(&b, "; characters it lacks come from the built-in bitmap font</td></tr></table>");
    put(&b, "<p class=\"small\">Engine: Hatate (user/lib/web). Fonts: libttf. Pictures: libimg. Network: libhttp and libtls. Docs: docs/BROWSER.md.</p>");
    put(&b, "</div></body></html>");
    if (len) *len = b.n;
    return b.p;
}

/* about:home: where a new window starts */
char *br_home_page(size_t *len, const char *home) {
    Buf b = { 0 };
    put(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title>Kyouko's browser</title>");
    put(&b, STYLE);
    put(&b, "<style>li{margin:4px 0}</style></head><body><div class=\"card\"><h1>Kyouko's browser</h1>"
            "<p class=\"joke\">Shout an address into the valley and see what answers.</p>"
            "<p class=\"what\">Type an address in the field above: a web address (https://example.org), the path of a file, or about:browser for the keys. Places to start:</p><ul>"
            "<li><a href=\"file:///\">the files of this computer</a></li>");
    if (home && *home) {
        put(&b, "<li><a href=\"file://");
        put_escaped(&b, home);
        put(&b, "\">your home folder</a></li>");
    }
    put(&b, "<li><a href=\"file:///ScarletDevilMansion/share/\">the shared files of the library (fonts and manual pages)</a></li>"
            "<li><a href=\"about:browser\">the keys and the limits of this browser</a></li>"
            "<li><a href=\"about:blank\">a blank page</a></li></ul>"
            "<p class=\"small\">Pages are read through Hatate, the engine; secure addresses are checked against the root certificates in /etc/ssl/cacert.pem.</p>"
            "</div></body></html>");
    if (len) *len = b.n;
    return b.p;
}
