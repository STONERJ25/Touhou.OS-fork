/*
 * The browser core, part three: form controls. Text fields and areas are edited here (the engine keeps the value, see
 * web_control_value; the painter draws the caret from the numbers kept in BrPage), a select opens a list drawn over the page,
 * Tab walks the controls, and activations the engine reports (links, submissions, toggles) are carried out.
 */
#include "core.h"
#include "urls.h"

#include <keys.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* the engine's implicit submission (Enter in a text field); declared in user/lib/web/src/forms.c, not in web.h */
WebAction forms_implicit_submit(WebDoc *d, WebNode *field);

bool field_is_text(WebNode *n) {
    if (!n) return false;
    if (web_node_is(n, "textarea")) return true;
    if (!web_node_is(n, "input")) return false;
    const char *t = web_element_get_attr(n, "type");
    if (!t || !*t) return true;
    static const char *const other[] = { "hidden", "checkbox", "radio", "submit", "reset", "button", "image", "file", "range", "color", NULL };
    for (int i = 0; other[i]; i++) if (!strcasecmp(t, other[i])) return false;
    return true;
}

static bool is_password(WebNode *n) {
    const char *t = web_element_get_attr(n, "type");
    return t && strcasecmp(t, "password") == 0;
}

static bool is_readonly(WebNode *n) { return web_element_get_attr(n, "readonly") != NULL || web_element_get_attr(n, "disabled") != NULL; }

/* ---------------------------------------------------------------- geometry */

bool page_control_geom(BrCore *c, BrPage *p, WebNode *node, WebDisplayItem *item, BrControlGeom *g) {
    WebRect r;
    if (!p->doc || !node || !web_element_rect(node, &r)) return false;
    int x0 = r.x >> 6, y0 = r.y >> 6;
    int w = ((r.x + r.w + 63) >> 6) - x0, h = ((r.y + r.h + 63) >> 6) - y0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const WebDisplayList *dl = web_doc_paint(p->doc, x0, y0, w, h);
    for (int i = 0; i < dl->n; i++) {
        const WebDisplayItem *it = &dl->items[i];
        if (it->kind == WEB_DI_CONTROL && it->node == node) {
            if (item) *item = *it;
            return brp_control_geom(c->painter, it, (x0 - p->scroll_x) * c->scale, (y0 - p->scroll_y) * c->scale, c->scale, g);
        }
    }
    return false;
}

void scroll_into_view(BrCore *c, BrPage *p, WebNode *el) {
    WebRect r;
    if (!p->doc || !el || !web_element_rect(el, &r)) return;
    int top = r.y >> 6, bottom = (r.y + r.h + 63) >> 6;
    if (top < p->scroll_y + 8) p->scroll_y = top - 24;
    else if (bottom > p->scroll_y + c->vh - 8) p->scroll_y = bottom - c->vh + 24;
    else return;
    page_scroll_clamp(c, p);
    core_invalidate(c);
}

/* ---------------------------------------------------------------- activations the engine reports */

void page_navigate_action(BrCore *c, BrPage *p, WebAction *a) {
    switch (a->kind) {
    case WEB_ACT_NAVIGATE: case WEB_ACT_SUBMIT:
        if (a->kind == WEB_ACT_SUBMIT && a->post) { brc_note(c, "This form sends its data with POST, which the browser cannot do yet"); break; }
        if (a->url) {
            if (!strncasecmp(a->url, "javascript:", 11)) brc_note(c, "Scripts are not supported yet");
            else if (!strncasecmp(a->url, "mailto:", 7)) brc_note(c, "The browser does not hand mailto: links to a mail program");
            else brc_navigate(c, a->url, BR_NAV_LINK);
        }
        break;
    case WEB_ACT_TOGGLED: p->layout_dirty = true; core_invalidate(c); break;
    case WEB_ACT_FOCUS:
        if (a->node && field_is_text(a->node)) field_focus(c, p, a->node, false, 0, 0);
        else if (a->node) { web_doc_set_focus(p->doc, a->node); core_invalidate(c); }
        break;
    case WEB_ACT_OPEN_SELECT: if (a->node) dropdown_open(c, p, a->node); break;
    default: break;
    }
    if (a->url) web_free(p->doc, a->url);
    if (a->body) web_free(p->doc, a->body);
    a->url = NULL;
    a->body = NULL;
}

/* ---------------------------------------------------------------- text fields */

static size_t u8_prev(const char *s, size_t i) {
    if (!i) return 0;
    i--;
    while (i && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t u8_next(const char *s, size_t len, size_t i) {
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static bool wordc(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }

/* the width (device px) of the first `at` bytes of the value as the field shows them */
static int value_prefix_width(BrCore *c, WebFontHandle font, const char *value, size_t at, bool password) {
    if (!password) return brf_prefix_width(c->fonts, font, c->scale, value, strlen(value), at);
    size_t chars = 0;
    for (size_t i = 0; i < at && value[i]; i++) if (((unsigned char)value[i] & 0xC0) != 0x80) chars++;
    char *shown = malloc(chars * 3 + 1);
    if (!shown) return 0;
    for (size_t i = 0; i < chars; i++) memcpy(shown + i * 3, "\xE2\x80\xA2", 3);
    shown[chars * 3] = 0;
    int w = brf_prefix_width(c->fonts, font, c->scale, shown, chars * 3, chars * 3);
    free(shown);
    return w;
}

/* the byte offset in value[0, len) whose caret position is nearest to x (device px from the text's origin) */
static size_t offset_at_x(BrCore *c, WebFontHandle font, const char *value, size_t len, int x, bool password) {
    char *copy = malloc(len + 1);                                      /* the line is a slice: terminate it for the width helpers */
    size_t *bound = malloc((len + 2) * sizeof *bound);                 /* the offsets at which a character starts (and the end) */
    if (!copy || !bound) { free(copy); free(bound); return 0; }
    memcpy(copy, value, len);
    copy[len] = 0;
    size_t n = 0;
    for (size_t i = 0;; i = u8_next(copy, len, i)) {
        bound[n++] = i;
        if (i >= len) break;
    }
    size_t lo = 0, hi = n - 1;                                         /* the last boundary whose width does not pass x: the widths only grow, so a binary search */
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (value_prefix_width(c, font, copy, bound[mid], password) <= x) lo = mid;
        else hi = mid - 1;
    }
    size_t best = bound[lo];
    if (lo + 1 < n) {                                                  /* the nearer of the two sides of x */
        int w0 = value_prefix_width(c, font, copy, bound[lo], password), w1 = value_prefix_width(c, font, copy, bound[lo + 1], password);
        if (abs(w1 - x) < abs(w0 - x)) best = bound[lo + 1];
    }
    free(copy);
    free(bound);
    return best;
}

static void ensure_visible(BrCore *c, BrPage *p) {
    WebNode *n = p->focus;
    BrControlGeom g;
    WebDisplayItem it;
    if (!n || !page_control_geom(c, p, n, &it, &g)) return;
    const char *v = web_control_value(n);
    if (!v) v = "";
    bool multi = web_node_is(n, "textarea");
    int cw = g.content.x1 - g.content.x0, ch = g.content.y1 - g.content.y0;
    if (!multi) {
        bool pw = is_password(n);
        int cx = value_prefix_width(c, g.font, v, p->caret, pw), total = value_prefix_width(c, g.font, v, strlen(v), pw), pad = 2 * c->scale;
        if (total <= cw) p->field_scroll = 0;
        else {
            if (cx - p->field_scroll > cw - pad) p->field_scroll = cx - cw + pad;
            if (cx - p->field_scroll < 0) p->field_scroll = cx;
            if (p->field_scroll < 0) p->field_scroll = 0;
        }
        return;
    }
    size_t len = strlen(v), nl = brp_wrap(c->painter, g.font, c->scale, cw, v, len, NULL, 0);
    BrLine *lines = malloc((nl ? nl : 1) * sizeof *lines);
    if (!lines) return;
    brp_wrap(c->painter, g.font, c->scale, cw, v, len, lines, nl);
    size_t li = 0;
    for (size_t i = 0; i < nl; i++) if (lines[i].start <= p->caret) li = i;
    free(lines);
    int top = (int)li * g.line_h, bottom = top + g.line_h;
    if (top < p->field_scroll) p->field_scroll = top;
    if (bottom - p->field_scroll > ch) p->field_scroll = bottom - ch;
    if (p->field_scroll < 0) p->field_scroll = 0;
}

static size_t caret_from_click(BrCore *c, BrPage *p, WebNode *n, int x_dev, int y_dev) {
    const char *v = web_control_value(n);
    if (!v) v = "";
    size_t len = strlen(v);
    BrControlGeom g;
    WebDisplayItem it;
    if (!page_control_geom(c, p, n, &it, &g)) return len;
    bool pw = is_password(n);
    if (!web_node_is(n, "textarea")) return offset_at_x(c, g.font, v, len, x_dev - g.text_x + p->field_scroll, pw);
    int cw = g.content.x1 - g.content.x0;
    size_t nl = brp_wrap(c->painter, g.font, c->scale, cw, v, len, NULL, 0);
    BrLine *lines = malloc((nl ? nl : 1) * sizeof *lines);
    if (!lines) return len;
    brp_wrap(c->painter, g.font, c->scale, cw, v, len, lines, nl);
    int row = g.line_h > 0 ? (y_dev - g.content.y0 + p->field_scroll) / g.line_h : 0;
    if (row < 0) row = 0;
    if ((size_t)row >= nl) row = (int)nl - 1;
    size_t off = lines[row].start + offset_at_x(c, g.font, v + lines[row].start, lines[row].end - lines[row].start, x_dev - g.text_x, false);
    free(lines);
    return off;
}

void field_focus(BrCore *c, BrPage *p, WebNode *node, bool at_click, int x_dev, int y_dev) {
    if (p->focus != node) {
        p->focus = node;
        web_doc_set_focus(p->doc, node);
        p->field_scroll = 0;
    }
    const char *v = web_control_value(node);
    size_t len = v ? strlen(v) : 0;
    p->caret = p->anchor = at_click ? caret_from_click(c, p, node, x_dev, y_dev) : len;
    if (p->caret > len) p->caret = p->anchor = len;
    p->caret_t0 = c->now_ms;
    ensure_visible(c, p);
    scroll_into_view(c, p, node);
    p->layout_dirty = true;
    core_invalidate(c);
}

void field_blur(BrCore *c, BrPage *p) {
    if (!p->focus) return;
    p->focus = NULL;
    if (p->doc) web_doc_set_focus(p->doc, NULL);
    p->layout_dirty = true;
    core_invalidate(c);
}

bool field_selection_text(BrPage *p, char **text, size_t *len) {
    if (!p->focus) return false;
    const char *v = web_control_value(p->focus);
    if (!v || is_password(p->focus)) return false;                    /* a password is never copied */
    size_t vl = strlen(v), lo = p->caret < p->anchor ? p->caret : p->anchor, hi = p->caret < p->anchor ? p->anchor : p->caret;
    if (hi > vl) hi = vl;
    if (lo >= hi) return false;
    char *t = malloc(hi - lo + 1);
    if (!t) return false;
    memcpy(t, v + lo, hi - lo);
    t[hi - lo] = 0;
    *text = t;
    *len = hi - lo;
    return true;
}

static void set_value(BrCore *c, BrPage *p, const char *nv, size_t caret) {
    web_control_set_value(p->focus, nv);
    p->caret = p->anchor = caret;
    p->caret_t0 = c->now_ms;
    ensure_visible(c, p);
    p->layout_dirty = true;
    core_invalidate(c);
}

void field_insert(BrCore *c, BrPage *p, const char *s, size_t n) {
    WebNode *node = p->focus;
    if (!node || is_readonly(node) || !n) return;
    bool multi = web_node_is(node, "textarea");
    const char *v = web_control_value(node);
    if (!v) v = "";
    size_t len = strlen(v), lo = p->caret < p->anchor ? p->caret : p->anchor, hi = p->caret < p->anchor ? p->anchor : p->caret;
    if (hi > len) hi = len;
    if (lo > hi) lo = hi;
    char *ins = malloc(n + 1);
    if (!ins) return;
    size_t il = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\r') continue;
        if (ch == '\n' && !multi) ch = ' ';
        if ((unsigned char)ch < 32 && ch != '\n' && ch != '\t') continue;
        ins[il++] = ch;
    }
    const char *ml = web_element_get_attr(node, "maxlength");
    if (ml && *ml) {                                                   /* maxlength counts characters */
        long max = atol(ml);
        long have = 0;
        for (size_t i = 0; i < len; i++) if (((unsigned char)v[i] & 0xC0) != 0x80 && (i < lo || i >= hi)) have++;
        long room = max - have, kept = 0;
        size_t cut = 0;
        for (size_t i = 0; i < il; i++) {
            if (((unsigned char)ins[i] & 0xC0) != 0x80) { if (kept >= room) break; kept++; }
            cut = i + 1;
        }
        il = room <= 0 ? 0 : cut;
    }
    if (il == 0 && lo == hi) { free(ins); return; }
    char *nv = malloc(len - (hi - lo) + il + 1);
    if (!nv) { free(ins); return; }
    memcpy(nv, v, lo);
    memcpy(nv + lo, ins, il);
    memcpy(nv + lo + il, v + hi, len - hi + 1);
    set_value(c, p, nv, lo + il);
    free(nv);
    free(ins);
}

static void delete_range(BrCore *c, BrPage *p, const char *v, size_t len, size_t lo, size_t hi) {
    char *nv = malloc(len - (hi - lo) + 1);                            /* copied before the engine drops the old value that v points into */
    if (!nv) return;
    memcpy(nv, v, lo);
    memcpy(nv + lo, v + hi, len - hi + 1);
    set_value(c, p, nv, lo);
    free(nv);
}

/* the caret one line up or down in a text area, keeping its horizontal position */
static size_t vertical_move(BrCore *c, BrPage *p, const char *v, int dir) {
    BrControlGeom g;
    WebDisplayItem it;
    size_t len = strlen(v);
    if (!page_control_geom(c, p, p->focus, &it, &g)) return p->caret;
    int cw = g.content.x1 - g.content.x0;
    size_t nl = brp_wrap(c->painter, g.font, c->scale, cw, v, len, NULL, 0);
    BrLine *lines = malloc((nl ? nl : 1) * sizeof *lines);
    if (!lines) return p->caret;
    brp_wrap(c->painter, g.font, c->scale, cw, v, len, lines, nl);
    size_t row = 0;
    for (size_t i = 0; i < nl; i++) if (lines[i].start <= p->caret) row = i;
    size_t result = p->caret;
    if ((dir < 0 && row > 0) || (dir > 0 && row + 1 < nl)) {
        char *cur = malloc(lines[row].end - lines[row].start + 1);
        if (cur) {
            size_t cl = lines[row].end - lines[row].start;
            memcpy(cur, v + lines[row].start, cl);
            cur[cl] = 0;
            size_t at = p->caret > lines[row].start ? p->caret - lines[row].start : 0;
            int x = brf_prefix_width(c->fonts, g.font, c->scale, cur, cl, at > cl ? cl : at);
            free(cur);
            size_t to = (size_t)((int)row + dir);
            result = lines[to].start + offset_at_x(c, g.font, v + lines[to].start, lines[to].end - lines[to].start, x, false);
        }
    } else result = dir < 0 ? 0 : len;
    free(lines);
    return result;
}

bool field_key(BrCore *c, BrPage *p, uint8_t code, uint8_t mods, uint32_t ch) {
    WebNode *n = p->focus;
    if (!n) return false;
    bool ctrl = (mods & MOD_CTRL) != 0, alt = (mods & MOD_ALT) != 0, shift = (mods & MOD_SHIFT) != 0;
    bool multi = web_node_is(n, "textarea"), ro = is_readonly(n);
    const char *v = web_control_value(n);
    if (!v) v = "";
    size_t len = strlen(v);
    if (p->caret > len) p->caret = len;
    if (p->anchor > len) p->anchor = len;
    size_t lo = p->caret < p->anchor ? p->caret : p->anchor, hi = p->caret < p->anchor ? p->anchor : p->caret;
    if (alt) return false;
    if (ctrl) {
        switch (code) {
        case KEY_A: brc_select_all(c); return true;
        case KEY_C: brc_copy(c); return true;
        case KEY_X:
            if (hi > lo && !ro) { brc_copy(c); delete_range(c, p, v, len, lo, hi); }
            return true;
        case KEY_V: return false;                                       /* the window code pastes: it owns the clipboard */
        case KEY_LEFT: {
            size_t i = p->caret;
            while (i > 0 && !wordc((unsigned char)v[i - 1])) i = u8_prev(v, i);
            while (i > 0 && wordc((unsigned char)v[i - 1])) i = u8_prev(v, i);
            p->caret = i;
            if (!shift) p->anchor = i;
            p->caret_t0 = c->now_ms;
            ensure_visible(c, p);
            core_invalidate(c);
            return true;
        }
        case KEY_RIGHT: {
            size_t i = p->caret;
            while (i < len && !wordc((unsigned char)v[i])) i = u8_next(v, len, i);
            while (i < len && wordc((unsigned char)v[i])) i = u8_next(v, len, i);
            p->caret = i;
            if (!shift) p->anchor = i;
            p->caret_t0 = c->now_ms;
            ensure_visible(c, p);
            core_invalidate(c);
            return true;
        }
        default: return false;
        }
    }
    size_t to = p->caret;
    bool moved = true;
    switch (code) {
    case KEY_LEFT: to = hi > lo && !shift ? lo : u8_prev(v, p->caret); break;
    case KEY_RIGHT: to = hi > lo && !shift ? hi : u8_next(v, len, p->caret); break;
    case KEY_UP: if (multi) to = vertical_move(c, p, v, -1); else return true; break;
    case KEY_DOWN: if (multi) to = vertical_move(c, p, v, 1); else return true; break;
    case KEY_HOME: to = 0; if (multi) { while (to < p->caret) { size_t nx = to; const char *nl = memchr(v + to, '\n', p->caret - to); if (!nl) break; nx = (size_t)(nl - v) + 1; to = nx; } } break;
    case KEY_END: to = len; if (multi) { const char *nl = memchr(v + p->caret, '\n', len - p->caret); if (nl) to = (size_t)(nl - v); } break;
    case KEY_BACKSPACE:
        if (ro) return true;
        if (hi > lo) delete_range(c, p, v, len, lo, hi);
        else if (p->caret > 0) delete_range(c, p, v, len, u8_prev(v, p->caret), p->caret);
        return true;
    case KEY_DELETE:
        if (ro) return true;
        if (hi > lo) delete_range(c, p, v, len, lo, hi);
        else if (p->caret < len) delete_range(c, p, v, len, p->caret, u8_next(v, len, p->caret));
        return true;
    case KEY_ENTER: case KEY_KPENTER:
        if (multi) { if (!ro) field_insert(c, p, "\n", 1); return true; }
        { WebAction a = forms_implicit_submit(p->doc, n); page_navigate_action(c, p, &a); }
        return true;
    case KEY_TAB: return focus_next(c, p, shift ? -1 : 1);
    case KEY_ESC: field_blur(c, p); return true;
    default:
        moved = false;
        if (ch >= 32 && ch != 127) {
            char enc[4];
            int k = 0;
            if (ch < 0x80) enc[k++] = (char)ch;
            else if (ch < 0x800) { enc[k++] = (char)(0xC0 | ch >> 6); enc[k++] = (char)(0x80 | (ch & 0x3F)); }
            else if (ch < 0x10000) { enc[k++] = (char)(0xE0 | ch >> 12); enc[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); enc[k++] = (char)(0x80 | (ch & 0x3F)); }
            else { enc[k++] = (char)(0xF0 | ch >> 18); enc[k++] = (char)(0x80 | ((ch >> 12) & 0x3F)); enc[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); enc[k++] = (char)(0x80 | (ch & 0x3F)); }
            if (!ro) field_insert(c, p, enc, (size_t)k);
            return true;
        }
        return false;
    }
    if (moved) {
        p->caret = to;
        if (!shift) p->anchor = to;
        p->caret_t0 = c->now_ms;
        ensure_visible(c, p);
        core_invalidate(c);
    }
    return true;
}

/* ---------------------------------------------------------------- walking the controls */

static bool focusable(WebNode *el) {
    const char *name = web_node_name(el);
    if (web_element_get_attr(el, "disabled")) return false;
    if (!strcmp(name, "input")) {
        const char *t = web_element_get_attr(el, "type");
        if (t && !strcasecmp(t, "hidden")) return false;
        return true;
    }
    if (!strcmp(name, "select") || !strcmp(name, "textarea") || !strcmp(name, "button")) return true;
    if (!strcmp(name, "a")) return web_element_get_attr(el, "href") != NULL;
    return false;
}

bool focus_next(BrCore *c, BrPage *p, int dir) {
    if (!p->doc) return false;
    int n = 0;
    WebNode **all = web_get_elements_by_tag(web_doc_root(p->doc), "*", &n);
    if (!all) return false;
    WebNode **list = malloc((size_t)(n ? n : 1) * sizeof *list);
    int m = 0;
    for (int i = 0; list && i < n; i++) {
        WebRect r;
        if (focusable(all[i]) && web_element_rect(all[i], &r) && r.w > 0 && r.h > 0) list[m++] = all[i];
    }
    web_free(p->doc, all);
    if (!list || m == 0) { free(list); return false; }
    WebNode *cur = p->focus ? p->focus : web_doc_focus(p->doc);
    int idx = -1;
    for (int i = 0; i < m; i++) if (list[i] == cur) idx = i;
    int ni = idx < 0 ? (dir > 0 ? 0 : m - 1) : (idx + dir + m) % m;
    WebNode *target = list[ni];
    free(list);
    if (p->dd_open) dropdown_close(c, p);
    if (field_is_text(target)) field_focus(c, p, target, false, 0, 0);
    else {
        if (p->focus) field_blur(c, p);
        web_doc_set_focus(p->doc, target);
        scroll_into_view(c, p, target);
        p->layout_dirty = true;
        core_invalidate(c);
    }
    return true;
}

/* ---------------------------------------------------------------- the drop-down of a select */

void dropdown_close(BrCore *c, BrPage *p) {
    if (!p->dd_open && !p->dd.labels) return;
    for (int i = 0; i < p->dd.n; i++) free(p->dd.labels[i]);
    free(p->dd.labels);
    memset(&p->dd, 0, sizeof p->dd);
    p->dd_open = false;
    if (c) core_invalidate(c);
}

void dropdown_open(BrCore *c, BrPage *p, WebNode *select) {
    dropdown_close(c, p);
    const char *labels[512];
    int n = web_select_options(select, labels, 512);
    if (n > 512) n = 512;
    if (n <= 0) return;
    WebDisplayItem it;
    BrControlGeom g;
    if (!page_control_geom(c, p, select, &it, &g)) return;
    p->dd.labels = calloc((size_t)n, sizeof *p->dd.labels);
    if (!p->dd.labels) return;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(labels[i]);
        p->dd.labels[i] = malloc(l + 1);
        if (p->dd.labels[i]) memcpy(p->dd.labels[i], labels[i], l + 1);
        else { p->dd.labels[i] = calloc(1, 1); }
    }
    p->dd.n = n;
    p->dd.select = select;
    p->dd.sel = p->dd.hover = web_select_index(select);
    p->dd.font = g.font;
    p->dd.box = br_rect(it.rect.x, it.rect.y, it.rect.w, it.rect.h);                  /* relative: moved below */
    WebRect r;
    if (web_element_rect(select, &r)) {
        int x = ((r.x >> 6) - p->scroll_x) * c->scale, y = ((r.y >> 6) - p->scroll_y) * c->scale;
        p->dd.box = br_rect(x, y, it.rect.w, it.rect.h);
    }
    p->dd_open = true;
    core_invalidate(c);
}

/* where the list goes: below the control, above it when there is no room, and how many rows show */
static void dd_geometry(BrCore *c, BrPage *p, BrRect *list, int *row_h, int *first, int *rows) {
    int s = c->scale;
    WebFontMetrics m;
    const WebFontProvider *pr = brf_provider(c->fonts);
    memset(&m, 0, sizeof m);
    if (pr->metrics) pr->metrics(pr->user, p->dd.font, &m);
    *row_h = (int)(((int64_t)(m.ascent + m.descent) * s + 32) / 64) + 4 * s;
    if (*row_h < 14 * s) *row_h = 14 * s;
    int width = p->dd.box.x1 - p->dd.box.x0;
    for (int i = 0; i < p->dd.n; i++) {
        int w = brf_prefix_width(c->fonts, p->dd.font, s, p->dd.labels[i], strlen(p->dd.labels[i]), strlen(p->dd.labels[i])) + 12 * s;
        if (w > width) width = w;
    }
    int maxrows = 12;
    int total = p->dd.n < maxrows ? p->dd.n : maxrows;
    int h = total * *row_h + 2 * s;
    int vh = c->vh * s;
    int y = p->dd.box.y1;
    if (y + h > vh && p->dd.box.y0 - h >= 0) y = p->dd.box.y0 - h;
    else if (y + h > vh) { total = (vh - y - 2 * s) / *row_h; if (total < 1) total = 1; h = total * *row_h + 2 * s; }
    int x = p->dd.box.x0;
    if (x + width > c->vw * s) x = c->vw * s - width;
    if (x < 0) x = 0;
    *list = br_rect(x, y, width, h);
    *rows = total;
    int f = p->dd.hover - total / 2;
    if (f > p->dd.n - total) f = p->dd.n - total;
    if (f < 0) f = 0;
    *first = f;
}

void dropdown_draw(BrCore *c, BrPage *p, BrSurface *dst) {
    if (!p->dd_open) return;
    BrRect list;
    int row_h, first, rows, s = c->scale;
    dd_geometry(c, p, &list, &row_h, &first, &rows);
    BrRect clip = br_rect_and(list, br_rect(0, 0, dst->w, dst->h));
    for (int y = clip.y0; y < clip.y1; y++)
        for (int x = clip.x0; x < clip.x1; x++) {
            bool edge = x < list.x0 + s || x >= list.x1 - s || y < list.y0 + s || y >= list.y1 - s;
            dst->px[(size_t)y * (size_t)dst->stride + (size_t)x] = edge ? 0x767676 : 0xFFFFFF;
        }
    WebFontMetrics m;
    const WebFontProvider *pr = brf_provider(c->fonts);
    memset(&m, 0, sizeof m);
    if (pr->metrics) pr->metrics(pr->user, p->dd.font, &m);
    int asc = (int)(((int64_t)m.ascent * s + 32) / 64);
    for (int i = 0; i < rows && first + i < p->dd.n; i++) {
        BrRect row = { list.x0 + s, list.y0 + s + i * row_h, list.x1 - s, list.y0 + s + (i + 1) * row_h };
        bool on = first + i == p->dd.hover;
        BrRect rc = br_rect_and(row, clip);
        if (on) for (int y = rc.y0; y < rc.y1; y++) for (int x = rc.x0; x < rc.x1; x++) dst->px[(size_t)y * (size_t)dst->stride + (size_t)x] = 0x3390FF;
        brf_draw_text(c->fonts, dst, clip, p->dd.font, s, p->dd.labels[first + i], strlen(p->dd.labels[first + i]), (int32_t)(row.x0 + 5 * s) * 64,
                      (int32_t)(row.y0 + 2 * s + asc) * 64, on ? 0xFFFFFFFFu : 0xFF000000u, 255);
    }
}

bool dropdown_pointer(BrCore *c, BrPage *p, int x, int y, int what) {
    if (!p->dd_open) return false;
    BrRect list;
    int row_h, first, rows, s = c->scale;
    dd_geometry(c, p, &list, &row_h, &first, &rows);
    int dx = x * s, dy = y * s;
    if (dx < list.x0 || dx >= list.x1 || dy < list.y0 + s || dy >= list.y1) return false;
    int row = (dy - list.y0 - s) / row_h;
    if (row >= rows || first + row >= p->dd.n) return true;
    if (what == 0) { if (p->dd.hover != first + row) { p->dd.hover = first + row; core_invalidate(c); } return true; }
    web_select_set_index(p->dd.select, first + row);
    dropdown_close(c, p);
    p->layout_dirty = true;
    core_invalidate(c);
    return true;
}

bool dropdown_key(BrCore *c, BrPage *p, uint8_t code) {
    if (!p->dd_open) return false;
    switch (code) {
    case KEY_UP: if (p->dd.hover > 0) p->dd.hover--; break;
    case KEY_DOWN: if (p->dd.hover + 1 < p->dd.n) p->dd.hover++; break;
    case KEY_HOME: p->dd.hover = 0; break;
    case KEY_END: p->dd.hover = p->dd.n - 1; break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        if (p->dd.hover >= 0) web_select_set_index(p->dd.select, p->dd.hover);
        dropdown_close(c, p);
        p->layout_dirty = true;
        break;
    case KEY_ESC: case KEY_TAB: dropdown_close(c, p); break;
    default: break;
    }
    core_invalidate(c);
    return true;
}
