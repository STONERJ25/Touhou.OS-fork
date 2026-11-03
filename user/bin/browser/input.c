/*
 * The browser core, part two: the pointer, the wheel and the keys, text selection, copying and find in page. Everything arrives as
 * plain numbers in viewport CSS pixels and key codes of shared/keys.h; the window code (main.c) does the translating.
 */
#include "core.h"
#include "urls.h"

#include <keys.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool page_hit(BrPage *p, int x, int y, WebHit *h) {
    return web_doc_hit_test(p->doc, x + p->scroll_x, y + p->scroll_y, h);
}

/* ---------------------------------------------------------------- selecting words and blocks */

static bool word_char(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }

static void select_word(BrPage *p, WebNode *node, int offset) {
    size_t len;
    const char *d = web_node_data(node, &len);
    if (!d || !len) return;
    size_t at = (size_t)offset > len ? len : (size_t)offset, a = at, b = at;
    if (at == len && at > 0) at--, a = at, b = at;
    if (!word_char((unsigned char)d[at])) { a = at; b = at + 1; }
    else {
        while (a > 0 && word_char((unsigned char)d[a - 1])) a--;
        while (b < len && word_char((unsigned char)d[b])) b++;
    }
    web_doc_select(p->doc, node, (int)a, node, (int)b);
}

static void text_nodes_of(WebNode *root, WebNode **first, WebNode **last, int depth) {
    if (depth > 200) return;
    for (WebNode *k = web_node_first_child(root); k; k = web_node_next_sibling(k)) {
        if (web_node_type(k) == WEB_NODE_TEXT) { if (!*first) *first = k; *last = k; }
        else text_nodes_of(k, first, last, depth + 1);
    }
}

static void select_block(BrPage *p, WebNode *node) {
    int err = 0;
    WebNode *el = web_node_type(node) == WEB_NODE_ELEMENT ? node : web_node_parent(node);
    WebNode *blk = el ? web_element_closest(el, "p,div,li,td,th,h1,h2,h3,h4,h5,h6,pre,blockquote,dd,dt,caption,body", &err) : NULL;
    WebNode *first = NULL, *last = NULL;
    if (blk) text_nodes_of(blk, &first, &last, 0);
    if (!first) { first = last = node; }
    size_t ll = 0;
    web_node_data(last, &ll);
    web_doc_select(p->doc, first, 0, last, (int)ll);
}

/* ---------------------------------------------------------------- pointer */

int brc_pointer_move(BrCore *c, int x, int y, unsigned buttons) {
    BrPage *p = c->page;
    if (!p || !p->doc) return WEB_CURSOR_DEFAULT;
    if (p->dd_open && dropdown_pointer(c, p, x, y, 0)) return WEB_CURSOR_DEFAULT;
    WebHit h;
    bool ok = x >= 0 && y >= 0 && x < c->vw && y < c->vh && page_hit(p, x, y, &h);
    if (p->pressed && (buttons & 1)) {                                   /* a drag that began in the page: it selects text */
        if (!p->dragged && abs(x - p->press_x) + abs(y - p->press_y) > 4) p->dragged = true;
        if (p->dragged && p->sel_anchor) {
            if (y < 0 || y >= c->vh) {                                   /* the pointer left the view: keep scrolling towards it */
                brc_scroll_by(c, y < 0 ? y / 2 - 4 : (y - c->vh) / 2 + 4);
                ok = page_hit(p, x < 0 ? 0 : x >= c->vw ? c->vw - 1 : x, y < 0 ? 0 : c->vh - 1, &h);
            }
            if (ok && h.node && web_node_type(h.node) == WEB_NODE_TEXT) web_doc_select(p->doc, p->sel_anchor, p->sel_anchor_off, h.node, h.offset);
            core_invalidate(c);
        }
        return WEB_CURSOR_TEXT;
    }
    WebNode *el = ok ? h.element : NULL;
    if (el != p->hover) { p->hover = el; web_doc_set_hover(p->doc, el); p->layout_dirty = true; core_invalidate(c); }
    const char *u = ok && h.href ? h.href : "";
    if (strcmp(u, p->hover_url) != 0) { snprintf(p->hover_url, sizeof p->hover_url, "%s", u); core_invalidate(c); }
    p->cursor = ok ? (int)h.cursor : WEB_CURSOR_DEFAULT;
    return p->cursor;
}

void brc_pointer_down(BrCore *c, int x, int y, int button, int clicks) {
    BrPage *p = c->page;
    if (!p || !p->doc) return;
    if (p->dd_open) {
        if (!dropdown_pointer(c, p, x, y, 1)) dropdown_close(c, p);       /* a click outside only closes the list */
        return;
    }
    if (button != 1) return;
    WebHit h;
    bool ok = x >= 0 && y >= 0 && x < c->vw && y < c->vh && page_hit(p, x, y, &h);
    p->pressed = ok;
    p->dragged = false;
    p->press_x = x;
    p->press_y = y;
    p->press_link = ok ? h.link : NULL;
    p->press_control = ok ? h.control : NULL;
    p->press_node = ok ? h.node : NULL;
    p->sel_anchor = NULL;
    if (!ok) { if (p->focus) field_blur(c, p); return; }
    if (h.control && field_is_text(h.control)) {                       /* a text field: it takes the caret, and the click is its own */
        field_focus(c, p, h.control, true, x * c->scale, y * c->scale);
        return;
    }
    if (p->focus) field_blur(c, p);
    if (h.control) {
        web_doc_set_focus(p->doc, h.control);
        web_doc_set_active(p->doc, h.element);                           /* the pressed look */
        p->layout_dirty = true;
        core_invalidate(c);
        return;
    }
    web_doc_select_none(p->doc);
    if (h.node && web_node_type(h.node) == WEB_NODE_TEXT) {
        if (clicks == 2) select_word(p, h.node, h.offset);
        else if (clicks >= 3) select_block(p, h.node);
        else { p->sel_anchor = h.node; p->sel_anchor_off = h.offset; }
    }
    core_invalidate(c);
}

void brc_pointer_up(BrCore *c, int x, int y, int button) {
    BrPage *p = c->page;
    if (!p || !p->doc || button != 1) return;
    bool was = p->pressed;
    p->pressed = false;
    web_doc_set_active(p->doc, NULL);
    p->layout_dirty = true;
    core_invalidate(c);
    if (!was || p->dragged) { p->dragged = false; return; }
    WebHit h;
    if (!(x >= 0 && y >= 0 && x < c->vw && y < c->vh && page_hit(p, x, y, &h))) return;
    if (p->press_control && field_is_text(p->press_control)) return;
    bool same = (h.link && h.link == p->press_link) || (h.control && h.control == p->press_control) || (!h.link && !h.control && h.node == p->press_node);
    if (!same) return;
    WebAction a = web_doc_activate(p->doc, h.node);
    page_navigate_action(c, p, &a);
}

void brc_wheel(BrCore *c, int x, int y, int ticks) {
    BrPage *p = c->page;
    if (!p || !p->doc || !ticks) return;
    if (p->dd_open) return;
    int amount = ticks * 48;
    WebNode *s = x >= 0 && y >= 0 ? web_doc_scroller_at(p->doc, x + p->scroll_x, y + p->scroll_y) : NULL;
    int ex, ey, mx, my;
    if (s && web_element_scroll(s, &ex, &ey, &mx, &my)) {
        int ny = ey - amount;
        bool can = (amount > 0 && ey > 0) || (amount < 0 && ey < my);
        if (can) {
            web_element_set_scroll(s, ex, ny < 0 ? 0 : ny > my ? my : ny);
            p->layout_dirty = true;
            core_invalidate(c);
            return;
        }
    }
    brc_scroll_by(c, -amount);
}

void brc_focus_out(BrCore *c) {
    BrPage *p = c->page;
    if (!p) return;
    p->pressed = false;
    if (p->hover && p->doc) { p->hover = NULL; web_doc_set_hover(p->doc, NULL); }
    p->hover_url[0] = 0;
    core_invalidate(c);
}

/* ---------------------------------------------------------------- keys */

bool brc_has_field_focus(const BrCore *c) { return c->page && c->page->focus != NULL; }

void brc_select_all(BrCore *c) {
    BrPage *p = c->page;
    if (!p || !p->doc) return;
    if (p->focus) {
        const char *v = web_control_value(p->focus);
        p->anchor = 0;
        p->caret = v ? strlen(v) : 0;
    } else web_doc_select_all(p->doc);
    p->caret_t0 = c->now_ms;
    core_invalidate(c);
}

bool brc_copy(BrCore *c) {
    BrPage *p = c->page;
    if (!p || !p->doc) return false;
    char *text = NULL;
    size_t len = 0;
    bool field = p->focus && field_selection_text(p, &text, &len);
    if (!field) text = web_doc_selection_text(p->doc, &len);
    if (!text || !len) { if (text && !field) web_free(p->doc, text); if (field) free(text); return false; }
    if (c->host.clipboard) c->host.clipboard(c->host.user, text, len);
    if (field) free(text); else web_free(p->doc, text);
    return true;
}

void brc_paste(BrCore *c, const char *text, size_t len) {
    BrPage *p = c->page;
    if (!p || !p->focus || !text) return;
    field_insert(c, p, text, len);
}

bool brc_key(BrCore *c, uint8_t code, uint8_t mods, uint32_t ch) {
    BrPage *p = c->page;
    if (!p) return false;
    bool ctrl = (mods & MOD_CTRL) != 0, alt = (mods & MOD_ALT) != 0, shift = (mods & MOD_SHIFT) != 0;
    if (p->dd_open) return dropdown_key(c, p, code);
    if (p->focus && field_key(c, p, code, mods, ch)) return true;
    if (!p->doc) return false;
    if (alt) return false;
    int page = c->vh > 80 ? c->vh - 40 : c->vh;
    if (ctrl) {
        switch (code) {
        case KEY_A: brc_select_all(c); return true;
        case KEY_C: return brc_copy(c);
        case KEY_HOME: brc_scroll_to(c, 0); return true;
        case KEY_END: brc_scroll_to(c, INT_MAX / 2); return true;
        default: return false;
        }
    }
    WebNode *fe = web_doc_focus(p->doc);
    if (fe && !p->focus && (code == KEY_SPACE || code == KEY_ENTER || code == KEY_KPENTER)) {
        WebAction a = web_doc_activate(p->doc, fe);
        page_navigate_action(c, p, &a);
        return true;
    }
    switch (code) {
    case KEY_UP: brc_scroll_by(c, -48); return true;
    case KEY_DOWN: brc_scroll_by(c, 48); return true;
    case KEY_PAGEUP: brc_scroll_by(c, -page); return true;
    case KEY_PAGEDOWN: brc_scroll_by(c, page); return true;
    case KEY_SPACE: brc_scroll_by(c, shift ? -page : page); return true;
    case KEY_HOME: brc_scroll_to(c, 0); return true;
    case KEY_END: brc_scroll_to(c, INT_MAX / 2); return true;
    case KEY_LEFT: case KEY_RIGHT: {
        int dx = code == KEY_LEFT ? -48 : 48;
        p->scroll_x += dx;
        page_scroll_clamp(c, p);
        core_invalidate(c);
        return true;
    }
    case KEY_TAB: return focus_next(c, p, shift ? -1 : 1);
    case KEY_ESC:
        if (p->focus || fe) { if (p->focus) field_blur(c, p); web_doc_set_focus(p->doc, NULL); core_invalidate(c); return true; }
        return false;
    default: return false;
    }
}

/* ---------------------------------------------------------------- find in page */

int brc_find(BrCore *c, const char *text, int direction) {
    BrPage *p = c->page;
    if (!p || !p->doc) return 0;
    if (!text || !*text) { web_doc_select_none(p->doc); p->find_text[0] = 0; p->find_count = p->find_index = 0; core_invalidate(c); return 0; }
    bool same = strcmp(text, p->find_text) == 0 && p->find_count > 0;
    WebRect r;
    int count;
    if (!same) {
        snprintf(p->find_text, sizeof p->find_text, "%s", text);
        count = web_doc_find(p->doc, text, 0, &r);
        p->find_index = 0;
    } else {
        int n = p->find_count, idx = direction == 0 ? p->find_index : (p->find_index + direction + n) % n;
        count = web_doc_find(p->doc, text, idx, &r);
        p->find_index = idx;
    }
    p->find_count = count;
    if (count > 0) {
        int top = r.y / 64, bottom = (r.y + r.h) / 64;
        if (top < p->scroll_y || bottom > p->scroll_y + c->vh) { p->scroll_y = top - c->vh / 3; page_scroll_clamp(c, p); }
    } else p->find_index = 0;
    core_invalidate(c);
    return count;
}

int brc_find_index(const BrCore *c) { return c->page && c->page->find_count ? c->page->find_index + 1 : 0; }

void brc_find_done(BrCore *c) {
    BrPage *p = c->page;
    if (!p) return;
    p->find_text[0] = 0;
    p->find_count = p->find_index = 0;
    if (p->doc) web_doc_select_none(p->doc);
    core_invalidate(c);
}
