/*
 * Text dumps for the tests and for debugging: the DOM in the html5lib tree-construction test format, e.g.
 *
 *     | <!DOCTYPE html>
 *     | <html>
 *     |   <head>
 *     |   <body>
 *     |     <svg svg>
 *     |       xlink href="x"
 *     |     "text"
 *
 * (attributes sorted by name, one level deeper than their element; foreign elements and attributes with their namespace
 * prefix; template contents under a "content" line). The layout dump is in layout_dump.c.
 */
#include "webint.h"

#include <stdlib.h>

typedef struct { WebWriter w; void *user; } Out;

static void put(Out *o, const char *s, size_t n) { o->w(o->user, s, n); }
static void puts_(Out *o, const char *s) { o->w(o->user, s, strlen(s)); }

static void indent(Out *o, int depth) {
    put(o, "| ", 2);
    for (int i = 0; i < depth; i++) put(o, "  ", 2);
}

typedef struct { const char *prefix, *name; const char *value; uint32_t vlen; } SortAttr;

/* attributes are ordered by their printed form, prefix and name together ("xml lang" < "xml:base") */
static int cmp_attr(const void *a, const void *b) {
    const SortAttr *x = a, *y = b;
    const char *p = x->prefix, *q = y->prefix;
    bool pn = false, qn = false;
    for (;;) {
        if (!*p && !pn) { p = x->name; pn = true; }
        if (!*q && !qn) { q = y->name; qn = true; }
        unsigned char c = (unsigned char)*p, d = (unsigned char)*q;
        if (c != d || !c) return (int)c - (int)d;
        p++;
        q++;
    }
}

static void dump(Out *o, WebNode *n, int depth) {
    WebDoc *d = n->doc;
    for (WebNode *k = n->first; k; k = k->next) {
        indent(o, depth);
        switch (k->type) {
        case WEB_NODE_DOCTYPE: {
            puts_(o, "<!DOCTYPE ");
            put(o, k->u.cd.data, k->u.cd.len);
            const char *pub = k->u.cd.public_id ? k->u.cd.public_id : "", *sys = k->u.cd.system_id ? k->u.cd.system_id : "";
            if (*pub || *sys) {
                puts_(o, " \"");
                puts_(o, pub);
                puts_(o, "\" \"");
                puts_(o, sys);
                puts_(o, "\"");
            }
            puts_(o, ">\n");
            break;
        }
        case WEB_NODE_ELEMENT: {
            puts_(o, "<");
            if (k->ns == WEB_NS_SVG) puts_(o, "svg ");
            else if (k->ns == WEB_NS_MATHML) puts_(o, "math ");
            puts_(o, atom_str(&d->atoms, k->name));
            puts_(o, ">\n");
            int na = k->u.el.nattrs;
            SortAttr *sa = na ? wa_alloc(&d->arena, sizeof(SortAttr) * (size_t)na) : NULL;
            for (int i = 0; i < na && sa; i++) {
                const WebAttr *a = &k->u.el.attrs[i];
                sa[i].prefix = a->ns == ATTR_NS_XLINK ? "xlink " : a->ns == ATTR_NS_XML ? "xml " : a->ns == ATTR_NS_XMLNS ? "xmlns " : "";
                sa[i].name = atom_str(&d->atoms, a->name);
                sa[i].value = a->value;
                sa[i].vlen = a->len;
            }
            if (sa) {
                qsort(sa, (size_t)na, sizeof(SortAttr), cmp_attr);
                for (int i = 0; i < na; i++) {
                    indent(o, depth + 1);
                    puts_(o, sa[i].prefix);
                    puts_(o, sa[i].name);
                    puts_(o, "=\"");
                    put(o, sa[i].value, sa[i].vlen);
                    puts_(o, "\"\n");
                }
                wa_free(&d->arena, sa, sizeof(SortAttr) * (size_t)na);
            }
            if (is_html(k, AT_template) && k->u.el.content) {
                indent(o, depth + 1);
                puts_(o, "content\n");
                dump(o, k->u.el.content, depth + 2);
            }
            dump(o, k, depth + 1);
            break;
        }
        case WEB_NODE_TEXT: case WEB_NODE_CDATA:
            puts_(o, "\"");
            put(o, k->u.cd.data, k->u.cd.len);
            puts_(o, "\"\n");
            break;
        case WEB_NODE_COMMENT:
            puts_(o, "<!-- ");
            put(o, k->u.cd.data, k->u.cd.len);
            puts_(o, " -->\n");
            break;
        case WEB_NODE_PI:
            puts_(o, "<?");
            puts_(o, atom_str(&d->atoms, k->name));
            puts_(o, " ");
            put(o, k->u.cd.data, k->u.cd.len);
            puts_(o, "?>\n");
            break;
        default:
            puts_(o, "?\n");
            break;
        }
    }
}

void web_dump_dom(WebNode *root, WebWriter w, void *user) {
    Out o = { w, user };
    dump(&o, root, 0);
}
