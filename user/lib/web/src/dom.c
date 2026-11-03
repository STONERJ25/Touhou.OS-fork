/*
 * The DOM: nodes, attributes, tree mutation, textContent, the HTML fragment serialization algorithm (HTML standard 13.3)
 * and simple lookups. Nodes live in the document's arena and are never freed before the document: a node removed from the
 * tree stays valid (a script may still hold it and insert it again), so pointers handed to the embedder do not dangle.
 *
 * Two layers: dom_* functions are the raw operations the parser uses (no checks, no notifications); web_* functions are
 * the public API that scripts reach through the DOM bridge: they check the DOM's validity rules, keep the id/class caches,
 * mark styles dirty and send mutation records to the WebScriptHost.
 */
#include "webint.h"

/* ================================================================================================ creation */

WebNode *dom_new_node(WebDoc *d, int type) {
    WebNode *n = wa_alloc(&d->arena, sizeof *n);
    if (!n) return NULL;
    n->type = (uint8_t)type;
    n->doc = d;
    return n;
}

WebNode *dom_new_element(WebDoc *d, int ns, WebAtom name) {
    WebNode *n = dom_new_node(d, WEB_NODE_ELEMENT);
    if (!n) return NULL;
    n->ns = (uint8_t)ns;
    n->name = name;
    return n;
}

bool dom_set_data(WebNode *t, const char *s, size_t n) {
    WebArena *a = &t->doc->arena;
    if (n + 1 > t->u.cd.cap) {
        size_t cap = n + 1 < 16 ? 16 : n + 1;
        char *p = wa_alloc(a, cap);
        if (!p) return false;
        if (t->u.cd.data) wa_free(a, t->u.cd.data, t->u.cd.cap);
        t->u.cd.data = p;
        t->u.cd.cap = (uint32_t)cap;
    }
    if (n) memmove(t->u.cd.data, s, n);
    t->u.cd.data[n] = 0;
    t->u.cd.len = (uint32_t)n;
    return true;
}

bool dom_text_append(WebNode *t, const char *s, size_t n) {
    size_t need = (size_t)t->u.cd.len + n + 1;
    if (need > 0x7FFFFFFF) return false;
    if (need > t->u.cd.cap) {
        size_t cap = t->u.cd.cap ? t->u.cd.cap : 16;
        while (cap < need) cap *= 2;
        char *p = wa_realloc(&t->doc->arena, t->u.cd.data, t->u.cd.cap, cap);
        if (!p) return false;
        t->u.cd.data = p;
        t->u.cd.cap = (uint32_t)cap;
    }
    memcpy(t->u.cd.data + t->u.cd.len, s, n);
    t->u.cd.len += (uint32_t)n;
    t->u.cd.data[t->u.cd.len] = 0;
    return true;
}

WebNode *dom_new_text(WebDoc *d, const char *s, size_t n) {
    WebNode *t = dom_new_node(d, WEB_NODE_TEXT);
    if (!t || !dom_set_data(t, s, n)) return NULL;
    return t;
}

WebNode *dom_new_comment(WebDoc *d, const char *s, size_t n) {
    WebNode *t = dom_new_node(d, WEB_NODE_COMMENT);
    if (!t || !dom_set_data(t, s, n)) return NULL;
    return t;
}

/* ================================================================================================ raw tree operations */

void dom_append(WebNode *parent, WebNode *child) {
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

void dom_insert_before(WebNode *parent, WebNode *child, WebNode *ref) {
    if (!ref) { dom_append(parent, child); return; }
    child->parent = parent;
    child->next = ref;
    child->prev = ref->prev;
    if (ref->prev) ref->prev->next = child;
    else parent->first = child;
    ref->prev = child;
}

void dom_remove(WebNode *child) {
    WebNode *p = child->parent;
    if (!p) return;
    if (child->prev) child->prev->next = child->next;
    else p->first = child->next;
    if (child->next) child->next->prev = child->prev;
    else p->last = child->prev;
    child->parent = child->prev = child->next = NULL;
}

WebNode *dom_next(const WebNode *n, const WebNode *root) {
    if (n->first) return n->first;
    return dom_next_skip(n, root);
}

WebNode *dom_next_skip(const WebNode *n, const WebNode *root) {
    while (n && n != root) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

/* ================================================================================================ attributes */

static bool ascii_ieq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

WebAttr *dom_attr(const WebNode *el, WebAtom name) {
    if (!is_el(el)) return NULL;
    for (int i = 0; i < el->u.el.nattrs; i++)
        if (el->u.el.attrs[i].name == name && el->u.el.attrs[i].ns == ATTR_NS_NONE) return &el->u.el.attrs[i];
    return NULL;
}

const char *dom_attr_val(const WebNode *el, WebAtom name) {
    WebAttr *a = dom_attr(el, name);
    return a ? a->value : NULL;
}

bool dom_attr_is(const WebNode *el, WebAtom name, const char *lowercase_value) {
    WebAttr *a = dom_attr(el, name);
    return a && ascii_ieq(a->value, a->len, lowercase_value);
}

static bool is_ascii_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

/* id and class are looked up by every selector match: keep their atoms on the element */
static void update_caches(WebNode *el, WebAtom name) {
    WebDoc *d = el->doc;
    if (el->ns != WEB_NS_HTML && el->ns != WEB_NS_SVG && el->ns != WEB_NS_MATHML) return;
    if (name == AT_id) {
        WebAttr *a = dom_attr(el, AT_id);
        el->u.el.id = (a && a->len) ? atom_get(&d->atoms, a->value, a->len) : AT_NONE;
    } else if (name == AT_class) {
        WebAttr *a = dom_attr(el, AT_class);
        if (el->u.el.classes) wa_free(&d->arena, el->u.el.classes, (size_t)el->u.el.cclasses * sizeof(WebAtom));
        el->u.el.classes = NULL;
        el->u.el.nclasses = el->u.el.cclasses = 0;
        if (!a) return;
        int count = 0;
        for (uint32_t i = 0; i < a->len;) {
            while (i < a->len && is_ascii_ws(a->value[i])) i++;
            if (i < a->len) count++;
            while (i < a->len && !is_ascii_ws(a->value[i])) i++;
        }
        if (!count) return;
        if (count > 0xFFFF) count = 0xFFFF;
        el->u.el.classes = wa_alloc(&d->arena, (size_t)count * sizeof(WebAtom));
        if (!el->u.el.classes) return;
        el->u.el.cclasses = (uint16_t)count;
        for (uint32_t i = 0; i < a->len && el->u.el.nclasses < count;) {
            while (i < a->len && is_ascii_ws(a->value[i])) i++;
            uint32_t s = i;
            while (i < a->len && !is_ascii_ws(a->value[i])) i++;
            if (i > s) el->u.el.classes[el->u.el.nclasses++] = atom_get(&d->atoms, a->value + s, i - s);
        }
    }
}

bool dom_add_attr(WebNode *el, WebAtom name, const char *v, size_t n, int ns, WebAtom prefix) {
    WebArena *a = &el->doc->arena;
    if (el->u.el.nattrs == el->u.el.cattrs) {
        if (el->u.el.cattrs >= 0x7FFF) return false;
        int nc = el->u.el.cattrs ? el->u.el.cattrs * 2 : 4;
        WebAttr *at = wa_realloc(a, el->u.el.attrs, sizeof(WebAttr) * el->u.el.cattrs, sizeof(WebAttr) * (size_t)nc);
        if (!at) return false;
        el->u.el.attrs = at;
        el->u.el.cattrs = (uint16_t)nc;
    }
    char *val = wa_strndup(a, v, n);
    if (!val) return false;
    WebAttr *at = &el->u.el.attrs[el->u.el.nattrs++];
    at->name = name;
    at->prefix = prefix;
    at->ns = (uint8_t)ns;
    at->value = val;
    at->len = (uint32_t)n;
    if (ns == ATTR_NS_NONE && (name == AT_id || name == AT_class)) update_caches(el, name);
    return true;
}

bool dom_set_attr(WebNode *el, WebAtom name, const char *v, size_t n) {
    WebAttr *at = dom_attr(el, name);
    if (!at) return dom_add_attr(el, name, v, n, ATTR_NS_NONE, AT_NONE);
    char *val = wa_strndup(&el->doc->arena, v, n);
    if (!val) return false;
    wa_free(&el->doc->arena, at->value, (size_t)at->len + 1);
    at->value = val;
    at->len = (uint32_t)n;
    if (name == AT_id || name == AT_class) update_caches(el, name);
    return true;
}

/* ================================================================================================ dirty marking, notifications */

void dom_mark_style_dirty(WebNode *n) {
    if (!n) return;
    WebDoc *d = n->doc;
    if (n->type != WEB_NODE_ELEMENT) n = n->parent;
    if (n && n->type == WEB_NODE_ELEMENT) {
        n->flags |= NF_STYLE_DIRTY;
        for (WebNode *p = n->parent; p && !(p->flags & NF_CHILD_DIRTY); p = p->parent) p->flags |= NF_CHILD_DIRTY;
    }
    d->style_dirty = true;
    d->layout_dirty = true;
}

void dom_mark_subtree_dirty(WebNode *n) {
    dom_mark_style_dirty(n);
}

void dom_notify(WebDoc *d, const WebMutation *m) {
    d->dom_version++;
    if (d->in_mutation) return;
    const WebScriptHost *h = d->cfg.scripts;
    if (h && h->mutation) h->mutation(h->user, d, m);
}

void dom_attr_changed(WebNode *el, WebAtom name) {
    if (name == AT_id || name == AT_class) update_caches(el, name);
    /* the element and its siblings (sibling combinators, :nth-child) */
    dom_mark_style_dirty(el->parent && el->parent->type == WEB_NODE_ELEMENT ? el->parent : el);
}

/* ================================================================================================ text content */

void dom_text_of(const WebNode *n, WebBuf *out) {
    if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_CDATA) {
        buf_put(out, n->u.cd.data, n->u.cd.len);
        return;
    }
    if (n->type == WEB_NODE_COMMENT || n->type == WEB_NODE_PI) {
        buf_put(out, n->u.cd.data, n->u.cd.len);
        return;
    }
    for (const WebNode *k = n->first; k; k = dom_next(k, n))
        if (k->type == WEB_NODE_TEXT || k->type == WEB_NODE_CDATA) buf_put(out, k->u.cd.data, k->u.cd.len);
}

/* ================================================================================================ serialization (13.3) */

static bool is_void(const WebNode *n) {
    if (n->ns != WEB_NS_HTML) return false;
    switch (n->name) {
    case AT_area: case AT_base: case AT_basefont: case AT_bgsound: case AT_br: case AT_col: case AT_embed:
    case AT_frame: case AT_hr: case AT_img: case AT_input: case AT_keygen: case AT_link: case AT_meta: case AT_param:
    case AT_source: case AT_track: case AT_wbr:
        return true;
    default:
        return false;
    }
}

static void escape(WebBuf *out, const char *s, size_t n, bool attr) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        size_t skip = 1;
        unsigned char c = (unsigned char)s[i];
        if (c == '&') rep = "&amp;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == '"' && attr) rep = "&quot;";
        else if (c == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) { rep = "&nbsp;"; skip = 2; }
        if (rep) {
            buf_put(out, s + start, i - start);
            buf_puts(out, rep);
            i += skip - 1;
            start = i + 1;
        }
    }
    buf_put(out, s + start, n - start);
}

static void serialize_attr_name(WebBuf *out, const WebNode *el, const WebAttr *a) {
    const AtomTable *at = &el->doc->atoms;
    switch (a->ns) {
    case ATTR_NS_XML: buf_puts(out, "xml:"); break;
    case ATTR_NS_XMLNS: if (a->name != AT_xmlns) buf_puts(out, "xmlns:"); break;
    case ATTR_NS_XLINK: buf_puts(out, "xlink:"); break;
    default: break;
    }
    buf_put(out, atom_str(at, a->name), atom_len(at, a->name));
}

static void serialize_children(WebNode *n, WebBuf *out, int depth);

static void serialize_node(WebNode *k, WebBuf *out, int depth) {
    WebDoc *d = k->doc;
    switch (k->type) {
    case WEB_NODE_ELEMENT: {
        const char *name = atom_str(&d->atoms, k->name);
        buf_putc(out, '<');
        buf_puts(out, name);
        for (int i = 0; i < k->u.el.nattrs; i++) {
            const WebAttr *a = &k->u.el.attrs[i];
            buf_putc(out, ' ');
            serialize_attr_name(out, k, a);
            buf_puts(out, "=\"");
            escape(out, a->value, a->len, true);
            buf_putc(out, '"');
        }
        buf_putc(out, '>');
        if (is_void(k)) return;
        if (depth < 4096) serialize_children(is_html(k, AT_template) && k->u.el.content ? k->u.el.content : k, out, depth + 1);
        buf_puts(out, "</");
        buf_puts(out, name);
        buf_putc(out, '>');
        return;
    }
    case WEB_NODE_TEXT: case WEB_NODE_CDATA: {
        const WebNode *p = k->parent;
        bool raw = p && p->ns == WEB_NS_HTML &&
                   (p->name == AT_style || p->name == AT_script || p->name == AT_xmp || p->name == AT_iframe ||
                    p->name == AT_noembed || p->name == AT_noframes || p->name == AT_plaintext ||
                    (p->name == AT_noscript && d->scripting));
        if (raw) buf_put(out, k->u.cd.data, k->u.cd.len);
        else escape(out, k->u.cd.data, k->u.cd.len, false);
        return;
    }
    case WEB_NODE_COMMENT:
        buf_puts(out, "<!--");
        buf_put(out, k->u.cd.data, k->u.cd.len);
        buf_puts(out, "-->");
        return;
    case WEB_NODE_PI:
        buf_puts(out, "<?");
        buf_puts(out, atom_str(&d->atoms, k->name));
        buf_putc(out, ' ');
        buf_put(out, k->u.cd.data, k->u.cd.len);
        buf_puts(out, "?>");
        return;
    case WEB_NODE_DOCTYPE:
        buf_puts(out, "<!DOCTYPE ");
        buf_put(out, k->u.cd.data, k->u.cd.len);
        buf_putc(out, '>');
        return;
    default:
        serialize_children(k, out, depth);
        return;
    }
}

static void serialize_children(WebNode *n, WebBuf *out, int depth) {
    if (is_void(n)) return;
    for (WebNode *k = n->first; k; k = k->next) serialize_node(k, out, depth);
}

void dom_serialize(WebNode *n, WebBuf *out, bool outer) {
    if (outer) serialize_node(n, out, 0);
    else serialize_children(is_html(n, AT_template) && n->u.el.content ? n->u.el.content : n, out, 0);
}

/* ================================================================================================ the public API */

WebNodeType web_node_type(const WebNode *n) { return (WebNodeType)n->type; }
WebNamespace web_node_namespace(const WebNode *n) { return n->type == WEB_NODE_ELEMENT ? (WebNamespace)n->ns : WEB_NS_NONE; }
WebDoc *web_node_doc(const WebNode *n) { return n->doc; }
WebNode *web_node_parent(const WebNode *n) { return n->parent; }
WebNode *web_node_first_child(const WebNode *n) { return n->first; }
WebNode *web_node_last_child(const WebNode *n) { return n->last; }
WebNode *web_node_next_sibling(const WebNode *n) { return n->next; }
WebNode *web_node_prev_sibling(const WebNode *n) { return n->prev; }
WebNode *web_template_content(const WebNode *t) { return is_html(t, AT_template) ? t->u.el.content : NULL; }
void *web_node_user(const WebNode *n) { return n->user; }
void web_node_set_user(WebNode *n, void *user) { n->user = user; }

bool web_node_is(const WebNode *n, const char *tag) {
    if (!is_html_el(n)) return false;
    return strcmp(atom_str(&n->doc->atoms, n->name), tag) == 0;
}

const char *web_node_name(const WebNode *n) {
    switch (n->type) {
    case WEB_NODE_ELEMENT: case WEB_NODE_PI: return atom_str(&n->doc->atoms, n->name);
    case WEB_NODE_TEXT: return "#text";
    case WEB_NODE_CDATA: return "#cdata-section";
    case WEB_NODE_COMMENT: return "#comment";
    case WEB_NODE_DOCUMENT: return "#document";
    case WEB_NODE_FRAGMENT: return "#document-fragment";
    case WEB_NODE_DOCTYPE: return n->u.cd.data ? n->u.cd.data : "";
    default: return "";
    }
}

const char *web_node_data(const WebNode *n, size_t *len) {
    if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_COMMENT || n->type == WEB_NODE_PI || n->type == WEB_NODE_CDATA) {
        if (len) *len = n->u.cd.len;
        return n->u.cd.data ? n->u.cd.data : "";
    }
    if (len) *len = 0;
    return NULL;
}

int web_node_set_data(WebNode *n, const char *s, size_t len) {
    if (!(n->type == WEB_NODE_TEXT || n->type == WEB_NODE_COMMENT || n->type == WEB_NODE_PI || n->type == WEB_NODE_CDATA))
        return WEB_E_ARG;
    char *old = n->doc->cfg.scripts ? wa_strndup(&n->doc->arena, n->u.cd.data ? n->u.cd.data : "", n->u.cd.len) : NULL;
    if (!dom_set_data(n, s, len)) return WEB_E_NOMEM;
    WebMutation m = { WEB_MUT_CHARACTER_DATA, n, NULL, NULL, NULL, NULL, NULL, old };
    dom_notify(n->doc, &m);
    if (old) wa_free(&n->doc->arena, old, strlen(old) + 1);
    if (n->type == WEB_NODE_TEXT) dom_mark_style_dirty(n->parent);
    return WEB_OK;
}

char *web_node_text_content(WebNode *n, size_t *len) {
    WebDoc *d = n->doc;
    WebBuf b;
    buf_init(&b, &d->arena);
    dom_text_of(n, &b);
    char *r = web_raw_alloc(&d->budget, (size_t)b.n + 1);
    if (r && b.n) memcpy(r, b.p, b.n);
    if (len) *len = b.n;
    buf_free(&b);
    return r;
}

/* ---------------------------------------------------------------------------------------------- mutation */

bool web_node_contains(const WebNode *a, const WebNode *b) {
    for (; b; b = b->parent)
        if (b == a) return true;
    return false;
}

static bool can_have_children(const WebNode *p) {
    return p->type == WEB_NODE_ELEMENT || p->type == WEB_NODE_DOCUMENT || p->type == WEB_NODE_FRAGMENT;
}

/* The DOM's "ensure pre-insert validity", simplified: no cycles, only nodes that can be children, at most one element
 * and one doctype under a document. */
static bool insert_ok(const WebNode *parent, const WebNode *child, const WebNode *ref) {
    if (!can_have_children(parent) || !child || child->doc != parent->doc) return false;
    if (web_node_contains(child, parent)) return false;
    if (ref && ref->parent != parent) return false;
    if (child->type == WEB_NODE_DOCUMENT) return false;
    if (parent->type == WEB_NODE_DOCUMENT) {
        if (child->type == WEB_NODE_TEXT) return false;
        if (child->type == WEB_NODE_ELEMENT)
            for (WebNode *k = parent->first; k; k = k->next)
                if (k->type == WEB_NODE_ELEMENT && k != child) return false;
    } else if (child->type == WEB_NODE_DOCTYPE) return false;
    return true;
}

static void notify_child(WebDoc *d, WebNode *parent, WebNode *added, WebNode *removed, WebNode *prev, WebNode *next) {
    WebMutation m = { WEB_MUT_CHILD_LIST, parent, added, removed, prev, next, NULL, NULL };
    dom_notify(d, &m);
}

int web_node_remove_child(WebNode *parent, WebNode *child) {
    if (!child || child->parent != parent) return WEB_E_ARG;
    WebDoc *d = parent->doc;
    WebNode *prev = child->prev, *next = child->next;
    dom_remove(child);
    if (d->focus && web_node_contains(child, d->focus)) d->focus = NULL;
    if (d->hover && web_node_contains(child, d->hover)) d->hover = NULL;
    notify_child(d, parent, NULL, child, prev, next);
    dom_mark_style_dirty(parent);
    return WEB_OK;
}

int web_node_insert_before(WebNode *parent, WebNode *child, WebNode *ref) {
    if (!parent || !child) return WEB_E_ARG;
    if (ref == child) ref = child->next;
    if (child->type == WEB_NODE_FRAGMENT) {
        for (WebNode *k = child->first; k; k = k->next)
            if (!insert_ok(parent, k, ref)) return WEB_E_ARG;
        while (child->first) {
            WebNode *k = child->first;
            dom_remove(k);
            dom_insert_before(parent, k, ref);
            notify_child(parent->doc, parent, k, NULL, k->prev, k->next);
        }
        dom_mark_style_dirty(parent);
        return WEB_OK;
    }
    if (!insert_ok(parent, child, ref)) return WEB_E_ARG;
    if (child->parent) web_node_remove_child(child->parent, child);
    dom_insert_before(parent, child, ref);
    notify_child(parent->doc, parent, child, NULL, child->prev, child->next);
    dom_mark_style_dirty(parent);
    return WEB_OK;
}

int web_node_append_child(WebNode *parent, WebNode *child) {
    return web_node_insert_before(parent, child, NULL);
}

int web_node_replace_child(WebNode *parent, WebNode *newc, WebNode *oldc) {
    if (!oldc || oldc->parent != parent || !newc) return WEB_E_ARG;
    if (newc == oldc) return WEB_OK;
    WebNode *ref = oldc->next == newc ? newc->next : oldc->next;
    int rc = web_node_remove_child(parent, oldc);
    if (rc) return rc;
    return web_node_insert_before(parent, newc, ref);
}

WebNode *web_doc_create_element_ns(WebDoc *d, WebNamespace ns, const char *tag) {
    if (!tag || !*tag) return NULL;
    WebAtom a = ns == WEB_NS_HTML ? atom_get_lower(&d->atoms, tag, strlen(tag)) : atom_get(&d->atoms, tag, strlen(tag));
    WebNode *el = dom_new_element(d, ns, a);
    if (el && ns == WEB_NS_HTML && a == AT_template) el->u.el.content = dom_new_node(d, WEB_NODE_FRAGMENT);
    return el;
}

WebNode *web_doc_create_element(WebDoc *d, const char *tag) {
    return web_doc_create_element_ns(d, WEB_NS_HTML, tag);
}

WebNode *web_doc_create_text(WebDoc *d, const char *s, size_t n) { return dom_new_text(d, s, n); }
WebNode *web_doc_create_comment(WebDoc *d, const char *s, size_t n) { return dom_new_comment(d, s, n); }
WebNode *web_doc_create_fragment(WebDoc *d) { return dom_new_node(d, WEB_NODE_FRAGMENT); }

int web_node_set_text_content(WebNode *n, const char *s, size_t len) {
    if (n->type != WEB_NODE_ELEMENT && n->type != WEB_NODE_FRAGMENT) return web_node_set_data(n, s, len);
    while (n->first) web_node_remove_child(n, n->first);
    if (len) {
        WebNode *t = dom_new_text(n->doc, s, len);
        if (!t) return WEB_E_NOMEM;
        return web_node_append_child(n, t);
    }
    return WEB_OK;
}

WebNode *web_node_clone(WebNode *n, bool deep) {
    WebDoc *d = n->doc;
    WebNode *c = dom_new_node(d, n->type);
    if (!c) return NULL;
    c->ns = n->ns;
    c->name = n->name;
    if (n->type == WEB_NODE_ELEMENT) {
        for (int i = 0; i < n->u.el.nattrs; i++) {
            const WebAttr *a = &n->u.el.attrs[i];
            if (!dom_add_attr(c, a->name, a->value, a->len, a->ns, a->prefix)) return NULL;
        }
        if (is_html(n, AT_template)) {
            c->u.el.content = dom_new_node(d, WEB_NODE_FRAGMENT);
            if (deep && n->u.el.content && c->u.el.content)
                for (WebNode *k = n->u.el.content->first; k; k = k->next) {
                    WebNode *kc = web_node_clone(k, true);
                    if (kc) dom_append(c->u.el.content, kc);
                }
        }
        if (n->flags & NF_HTML_INTEGRATION) c->flags |= NF_HTML_INTEGRATION;
    } else if (n->type != WEB_NODE_DOCUMENT && n->type != WEB_NODE_FRAGMENT) {
        if (!dom_set_data(c, n->u.cd.data ? n->u.cd.data : "", n->u.cd.len)) return NULL;
        if (n->type == WEB_NODE_DOCTYPE) {
            c->u.cd.public_id = n->u.cd.public_id ? wa_strdup(&d->arena, n->u.cd.public_id) : NULL;
            c->u.cd.system_id = n->u.cd.system_id ? wa_strdup(&d->arena, n->u.cd.system_id) : NULL;
        }
    }
    if (deep)
        for (WebNode *k = n->first; k; k = k->next) {
            WebNode *kc = web_node_clone(k, true);
            if (!kc) return NULL;
            dom_append(c, kc);
        }
    return c;
}

int web_node_compare_position(const WebNode *a, const WebNode *b) {
    if (a == b) return 0;
    /* depth of each, then walk up to a common ancestor */
    int da = 0, db = 0;
    for (const WebNode *x = a; x->parent; x = x->parent) da++;
    for (const WebNode *x = b; x->parent; x = x->parent) db++;
    const WebNode *x = a, *y = b;
    while (da > db) { if (x->parent == b) return 1; x = x->parent; da--; }
    while (db > da) { if (y->parent == a) return -1; y = y->parent; db--; }
    while (x->parent != y->parent) { x = x->parent; y = y->parent; }
    if (!x->parent) return a < b ? -1 : 1;                     /* different trees: any stable order */
    for (const WebNode *k = x; k; k = k->next)
        if (k == y) return -1;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- attributes (public) */

static WebAttr *find_qualified(const WebNode *el, const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < el->u.el.nattrs; i++) {
        WebAttr *a = &el->u.el.attrs[i];
        const AtomTable *at = &el->doc->atoms;
        const char *ln = atom_str(at, a->name);
        size_t lnn = atom_len(at, a->name);
        if (a->ns == ATTR_NS_NONE) {
            if (el->ns == WEB_NS_HTML ? ascii_ieq(name, n, ln) && lnn == n : (lnn == n && memcmp(ln, name, n) == 0)) return a;
            continue;
        }
        const char *pre = a->ns == ATTR_NS_XML ? "xml" : a->ns == ATTR_NS_XLINK ? "xlink" : (a->name == AT_xmlns ? NULL : "xmlns");
        if (!pre) { if (n == 5 && memcmp(name, "xmlns", 5) == 0) return a; continue; }
        size_t pn = strlen(pre);
        if (n == pn + 1 + lnn && memcmp(name, pre, pn) == 0 && name[pn] == ':' && memcmp(name + pn + 1, ln, lnn) == 0) return a;
    }
    return NULL;
}

int web_element_attr_count(const WebNode *el) { return is_el(el) ? el->u.el.nattrs : 0; }

bool web_element_attr_at(const WebNode *el, int i, const char **name, const char **value) {
    if (!is_el(el) || i < 0 || i >= el->u.el.nattrs) return false;
    const WebAttr *a = &el->u.el.attrs[i];
    char *qbuf = el->doc->qname;                               /* valid until the next call on this document */
    const size_t qcap = sizeof el->doc->qname;
    const AtomTable *at = &el->doc->atoms;
    if (a->ns == ATTR_NS_NONE || (a->ns == ATTR_NS_XMLNS && a->name == AT_xmlns)) {
        *name = atom_str(at, a->name);
    } else {
        const char *pre = a->ns == ATTR_NS_XML ? "xml:" : a->ns == ATTR_NS_XLINK ? "xlink:" : "xmlns:";
        size_t pn = strlen(pre), ln = atom_len(at, a->name);
        if (pn + ln >= qcap) ln = qcap - pn - 1;
        memcpy(qbuf, pre, pn);
        memcpy(qbuf + pn, atom_str(at, a->name), ln);
        qbuf[pn + ln] = 0;
        *name = qbuf;
    }
    *value = a->value;
    return true;
}

const char *web_element_get_attr(const WebNode *el, const char *name) {
    if (!is_el(el)) return NULL;
    WebAttr *a = find_qualified(el, name);
    return a ? a->value : NULL;
}

int web_element_set_attr(WebNode *el, const char *name, const char *value) {
    if (!is_el(el) || !name || !*name) return WEB_E_ARG;
    WebDoc *d = el->doc;
    size_t vn = value ? strlen(value) : 0;
    WebAttr *a = find_qualified(el, name);
    char *old = NULL;
    if (a) {
        if (d->cfg.scripts) old = wa_strndup(&d->arena, a->value, a->len);
        char *v = wa_strndup(&d->arena, value ? value : "", vn);
        if (!v) return WEB_E_NOMEM;
        wa_free(&d->arena, a->value, (size_t)a->len + 1);
        a->value = v;
        a->len = (uint32_t)vn;
    } else {
        WebAtom an = el->ns == WEB_NS_HTML ? atom_get_lower(&d->atoms, name, strlen(name)) : atom_get(&d->atoms, name, strlen(name));
        if (!dom_add_attr(el, an, value ? value : "", vn, ATTR_NS_NONE, AT_NONE)) return WEB_E_NOMEM;
        a = &el->u.el.attrs[el->u.el.nattrs - 1];
    }
    WebAtom an = a->name;
    WebMutation m = { WEB_MUT_ATTRIBUTE, el, NULL, NULL, NULL, NULL, name, old };
    dom_notify(d, &m);
    if (old) wa_free(&d->arena, old, strlen(old) + 1);
    if (an == AT_style && el->ns == WEB_NS_HTML) el->u.el.inline_decls = NULL;      /* re-parsed by the style system */
    dom_attr_changed(el, an);
    return WEB_OK;
}

int web_element_remove_attr(WebNode *el, const char *name) {
    if (!is_el(el)) return WEB_E_ARG;
    WebAttr *a = find_qualified(el, name);
    if (!a) return WEB_OK;
    WebDoc *d = el->doc;
    WebAtom an = a->name;
    const char *old = a->value;
    WebMutation m = { WEB_MUT_ATTRIBUTE, el, NULL, NULL, NULL, NULL, name, old };
    int i = (int)(a - el->u.el.attrs);
    memmove(&el->u.el.attrs[i], &el->u.el.attrs[i + 1], sizeof(WebAttr) * (size_t)(el->u.el.nattrs - i - 1));
    el->u.el.nattrs--;
    dom_notify(d, &m);
    if (an == AT_style) el->u.el.inline_decls = NULL;
    dom_attr_changed(el, an);
    return WEB_OK;
}

const char *web_element_id(const WebNode *el) {
    const char *v = is_el(el) ? dom_attr_val(el, AT_id) : NULL;
    return v ? v : "";
}

bool web_element_has_class(const WebNode *el, const char *cls) {
    if (!is_el(el)) return false;
    WebAtom a = atom_find(&el->doc->atoms, cls, strlen(cls));
    if (!a) return false;
    for (int i = 0; i < el->u.el.nclasses; i++)
        if (el->u.el.classes[i] == a) return true;
    return false;
}

/* ---------------------------------------------------------------------------------------------- serialization (public) */

static char *buf_result(WebDoc *d, WebBuf *b, size_t *len) {
    char *r = web_raw_alloc(&d->budget, (size_t)b->n + 1);
    if (r && b->n) memcpy(r, b->p, b->n);
    if (len) *len = r ? b->n : 0;
    buf_free(b);
    return r;
}

char *web_node_inner_html(WebNode *n, size_t *len) {
    WebBuf b;
    buf_init(&b, &n->doc->arena);
    dom_serialize(n, &b, false);
    return buf_result(n->doc, &b, len);
}

char *web_node_outer_html(WebNode *n, size_t *len) {
    WebBuf b;
    buf_init(&b, &n->doc->arena);
    dom_serialize(n, &b, true);
    return buf_result(n->doc, &b, len);
}

int web_node_set_inner_html(WebNode *el, const char *s, size_t len) {
    if (!is_el(el)) return WEB_E_ARG;
    WebDoc *d = el->doc;
    WebNode *frag = dom_new_node(d, WEB_NODE_FRAGMENT);
    if (!frag) return WEB_E_NOMEM;
    int rc = parse_fragment(d, el, s, len, frag);
    if (rc) return rc;
    WebNode *target = is_html(el, AT_template) ? el->u.el.content : el;
    while (target->first) web_node_remove_child(target, target->first);
    return web_node_insert_before(target, frag, NULL);
}

/* ---------------------------------------------------------------------------------------------- lookups */

void web_free(WebDoc *d, void *p) {
    web_raw_free(&d->budget, p);
}

WebNode *web_doc_get_element_by_id(WebDoc *d, const char *id) {
    if (!id || !*id) return NULL;
    WebAtom a = atom_find(&d->atoms, id, strlen(id));
    if (!a) return NULL;
    for (WebNode *n = d->root->first; n; n = dom_next(n, d->root))
        if (n->type == WEB_NODE_ELEMENT && n->u.el.id == a) return n;
    return NULL;
}

typedef struct { WebNode **v; int n, cap; WebDoc *d; bool fail; } NodeList;

static void nl_push(NodeList *l, WebNode *n) {
    if (l->fail) return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        WebNode **v = web_raw_alloc(&l->d->budget, sizeof(WebNode *) * (size_t)nc);
        if (!v) { l->fail = true; return; }
        if (l->v) { memcpy(v, l->v, sizeof(WebNode *) * (size_t)l->n); web_raw_free(&l->d->budget, l->v); }
        l->v = v;
        l->cap = nc;
    }
    l->v[l->n++] = n;
}

WebNode **web_get_elements_by_tag(WebNode *root, const char *tag, int *count) {
    WebDoc *d = root->doc;
    NodeList l = { NULL, 0, 0, d, false };
    bool all = strcmp(tag, "*") == 0;
    WebAtom lower = atom_get_lower(&d->atoms, tag, strlen(tag));
    WebAtom exact = atom_find(&d->atoms, tag, strlen(tag));
    for (WebNode *n = root->first; n; n = dom_next(n, root))
        if (n->type == WEB_NODE_ELEMENT && (all || (n->ns == WEB_NS_HTML ? n->name == lower : n->name == exact))) nl_push(&l, n);
    *count = l.fail ? 0 : l.n;
    if (l.fail) { web_raw_free(&d->budget, l.v); return NULL; }
    return l.v;
}

WebNode **web_get_elements_by_class(WebNode *root, const char *classes, int *count) {
    WebDoc *d = root->doc;
    NodeList l = { NULL, 0, 0, d, false };
    WebAtom want[32];
    int nw = 0;
    for (const char *p = classes; *p && nw < 32;) {
        while (*p && is_ascii_ws(*p)) p++;
        const char *s = p;
        while (*p && !is_ascii_ws(*p)) p++;
        if (p > s) {
            want[nw] = atom_find(&d->atoms, s, (size_t)(p - s));
            if (!want[nw]) { *count = 0; return NULL; }
            nw++;
        }
    }
    if (!nw) { *count = 0; return NULL; }
    for (WebNode *n = root->first; n; n = dom_next(n, root)) {
        if (n->type != WEB_NODE_ELEMENT) continue;
        bool ok = true;
        for (int i = 0; i < nw && ok; i++) {
            bool f = false;
            for (int k = 0; k < n->u.el.nclasses; k++)
                if (n->u.el.classes[k] == want[i]) { f = true; break; }
            ok = f;
        }
        if (ok) nl_push(&l, n);
    }
    *count = l.fail ? 0 : l.n;
    if (l.fail) { web_raw_free(&d->budget, l.v); return NULL; }
    return l.v;
}
