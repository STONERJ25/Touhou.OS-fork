/*
 * HTML tree construction: the WHATWG HTML standard, section 13.2.6, as of the 2025-2026 text (customizable <select>: the
 * "in select" insertion modes are gone and select is a scope boundary; processing instructions become PI nodes).
 *
 * The tree builder receives tokens from the tokenizer (tokenizer.c) and builds the DOM through the "stack of open
 * elements" and the "list of active formatting elements", with all insertion modes, foster parenting, the adoption agency
 * algorithm, foreign content (SVG and MathML) and the fragment case (innerHTML). Not implemented: declarative shadow
 * roots (<template shadowrootmode> stays a template), the sanitizer hooks, custom elements, speculative parsing.
 *
 * Character tokens arrive as runs. Where the insertion mode treats white space and other characters differently, a run is
 * split into pieces that are all white space, all other characters, or a single NUL, and each piece is processed like one
 * character token; since adjacent characters always land in the same Text node, this gives the same tree.
 *
 * Hostile nesting: the DOM is kept at most max_depth (WebConfig.max_depth, default 512) levels deep. Past that depth new
 * nodes are attached to the element at the limit instead of the current node (the stack of open elements itself still
 * follows the algorithm, so the state machine is unaffected); everything recursive downstream (style, layout) is then
 * bounded too.
 */
#include "webint.h"

/* ================================================================================================ categories */

static bool is_special(const WebNode *n) {
    if (n->ns == WEB_NS_HTML) {
        switch (n->name) {
        case AT_address: case AT_applet: case AT_area: case AT_article: case AT_aside: case AT_base: case AT_basefont:
        case AT_bgsound: case AT_blockquote: case AT_body: case AT_br: case AT_button: case AT_caption: case AT_center:
        case AT_col: case AT_colgroup: case AT_dd: case AT_details: case AT_dir: case AT_div: case AT_dl: case AT_dt:
        case AT_embed: case AT_fieldset: case AT_figcaption: case AT_figure: case AT_footer: case AT_form: case AT_frame:
        case AT_frameset: case AT_h1: case AT_h2: case AT_h3: case AT_h4: case AT_h5: case AT_h6: case AT_head:
        case AT_header: case AT_hgroup: case AT_hr: case AT_html: case AT_iframe: case AT_img: case AT_input:
        case AT_keygen: case AT_li: case AT_link: case AT_listing: case AT_main: case AT_marquee: case AT_menu:
        case AT_meta: case AT_nav: case AT_noembed: case AT_noframes: case AT_noscript: case AT_object: case AT_ol:
        case AT_p: case AT_param: case AT_plaintext: case AT_pre: case AT_script: case AT_search: case AT_section:
        case AT_select: case AT_source: case AT_style: case AT_summary: case AT_table: case AT_tbody: case AT_td:
        case AT_template: case AT_textarea: case AT_tfoot: case AT_th: case AT_thead: case AT_title: case AT_tr:
        case AT_track: case AT_ul: case AT_wbr: case AT_xmp:
            return true;
        default:
            return false;
        }
    }
    if (n->ns == WEB_NS_MATHML)
        return n->name == AT_mi || n->name == AT_mo || n->name == AT_mn || n->name == AT_ms || n->name == AT_mtext ||
               n->name == AT_annotation_xml;
    if (n->ns == WEB_NS_SVG) return n->name == AT_foreignObject || n->name == AT_desc || n->name == AT_title;
    return false;
}

static bool is_h(WebAtom a) { return a == AT_h1 || a == AT_h2 || a == AT_h3 || a == AT_h4 || a == AT_h5 || a == AT_h6; }

static bool is_mathml_text_ip(const WebNode *n) {
    return n->ns == WEB_NS_MATHML && (n->name == AT_mi || n->name == AT_mo || n->name == AT_mn || n->name == AT_ms ||
                                      n->name == AT_mtext);
}

static bool is_html_ip(const WebNode *n) {
    if (n->ns == WEB_NS_MATHML && n->name == AT_annotation_xml) return (n->flags & NF_HTML_INTEGRATION) != 0;
    return n->ns == WEB_NS_SVG && (n->name == AT_foreignObject || n->name == AT_desc || n->name == AT_title);
}

enum { SC_DEFAULT, SC_LIST, SC_BUTTON, SC_TABLE };

static bool scope_boundary(const WebNode *n, int kind) {
    if (kind == SC_TABLE)
        return n->ns == WEB_NS_HTML && (n->name == AT_html || n->name == AT_table || n->name == AT_template);
    if (n->ns == WEB_NS_HTML) {
        switch (n->name) {
        case AT_applet: case AT_caption: case AT_html: case AT_table: case AT_td: case AT_th: case AT_marquee:
        case AT_object: case AT_select: case AT_template:
            return true;
        case AT_ol: case AT_ul:
            return kind == SC_LIST;
        case AT_button:
            return kind == SC_BUTTON;
        default:
            return false;
        }
    }
    if (n->ns == WEB_NS_MATHML)
        return n->name == AT_mi || n->name == AT_mo || n->name == AT_mn || n->name == AT_ms || n->name == AT_mtext ||
               n->name == AT_annotation_xml;
    if (n->ns == WEB_NS_SVG) return n->name == AT_foreignObject || n->name == AT_desc || n->name == AT_title;
    return false;
}

/* ================================================================================================ the stack */

static WebNode *cur(HtmlTreeBuilder *tb) { return tb->open.n ? tb->open.v[tb->open.n - 1] : NULL; }

static WebNode *adj_cur(HtmlTreeBuilder *tb) {
    if (tb->context && tb->open.n == 1) return tb->context;
    return cur(tb);
}

static bool cur_is(HtmlTreeBuilder *tb, WebAtom name) { return is_html(cur(tb), name); }

static void push(HtmlTreeBuilder *tb, WebNode *n) {
    if (!vec_push(tb->a, tb->open, n)) tb->doc->status = WEB_E_NOMEM;
}

/* Customizable <select>: when the parser pops an option, the select's <selectedcontent> receives a copy of the selected
 * option's children (the last option with a selected attribute, else the first option): "maybe clone an option into
 * selectedcontent" of the HTML standard's forms chapter, as far as the parser sees it. */
static void option_popped(WebNode *opt) {
    WebNode *sel = NULL;
    for (WebNode *p = opt->parent; p; p = p->parent) {
        if (is_html(p, AT_select)) { sel = p; break; }
        if (is_html(p, AT_datalist)) return;
    }
    if (!sel) return;
    WebNode *sc = NULL, *first = NULL, *chosen = NULL;
    for (WebNode *n = sel->first; n; n = dom_next(n, sel)) {
        if (!sc && is_html(n, AT_selectedcontent)) sc = n;
        if (is_html(n, AT_option)) {
            if (!first) first = n;
            if (dom_attr(n, AT_selected)) chosen = n;
        }
    }
    if (!chosen) chosen = first;
    if (!sc || chosen != opt) return;
    while (sc->first) dom_remove(sc->first);
    for (WebNode *k = opt->first; k; k = k->next) {
        WebNode *c = web_node_clone(k, true);
        if (c) dom_append(sc, c);
    }
}

static void pop(HtmlTreeBuilder *tb) {
    if (!tb->open.n) return;
    WebNode *n = tb->open.v[--tb->open.n];
    if (n->ns == WEB_NS_HTML && n->name == AT_option) option_popped(n);
}

static int stack_index(HtmlTreeBuilder *tb, const WebNode *n) {
    for (int i = (int)tb->open.n - 1; i >= 0; i--)
        if (tb->open.v[i] == n) return i;
    return -1;
}

static void stack_remove(HtmlTreeBuilder *tb, const WebNode *n) {
    int i = stack_index(tb, n);
    if (i < 0) return;
    memmove(&tb->open.v[i], &tb->open.v[i + 1], (tb->open.n - (uint32_t)i - 1) * sizeof(WebNode *));
    tb->open.n--;
}

static void stack_insert(HtmlTreeBuilder *tb, int at, WebNode *n) {
    if (!vec_reserve(tb->a, tb->open, tb->open.n + 1)) return;
    memmove(&tb->open.v[at + 1], &tb->open.v[at], (tb->open.n - (uint32_t)at) * sizeof(WebNode *));
    tb->open.v[at] = n;
    tb->open.n++;
}

static bool has_in_scope(HtmlTreeBuilder *tb, WebAtom name, int kind) {
    for (int i = (int)tb->open.n - 1; i >= 0; i--) {
        WebNode *n = tb->open.v[i];
        if (n->ns == WEB_NS_HTML && n->name == name) return true;
        if (scope_boundary(n, kind)) return false;
    }
    return false;
}

static bool has_node_in_scope(HtmlTreeBuilder *tb, const WebNode *node, int kind) {
    for (int i = (int)tb->open.n - 1; i >= 0; i--) {
        WebNode *n = tb->open.v[i];
        if (n == node) return true;
        if (scope_boundary(n, kind)) return false;
    }
    return false;
}

static bool has_h_in_scope(HtmlTreeBuilder *tb) {
    for (int i = (int)tb->open.n - 1; i >= 0; i--) {
        WebNode *n = tb->open.v[i];
        if (n->ns == WEB_NS_HTML && is_h(n->name)) return true;
        if (scope_boundary(n, SC_DEFAULT)) return false;
    }
    return false;
}

static bool stack_has(HtmlTreeBuilder *tb, WebAtom name) {
    for (uint32_t i = 0; i < tb->open.n; i++)
        if (is_html(tb->open.v[i], name)) return true;
    return false;
}

/* "the parser is parsing template contents" (13.2.4.4) */
static bool parsing_template(HtmlTreeBuilder *tb) {
    return stack_has(tb, AT_template) || is_html(tb->context, AT_template);
}

static void pop_until(HtmlTreeBuilder *tb, WebAtom name) {
    while (tb->open.n) {
        WebNode *n = cur(tb);
        pop(tb);
        if (is_html(n, name)) return;
    }
}

static void pop_until_node(HtmlTreeBuilder *tb, const WebNode *node) {
    while (tb->open.n) {
        WebNode *n = cur(tb);
        pop(tb);
        if (n == node) return;
    }
}

static bool implied(const WebNode *n, bool thorough) {
    if (n->ns != WEB_NS_HTML) return false;
    switch (n->name) {
    case AT_dd: case AT_dt: case AT_li: case AT_optgroup: case AT_option: case AT_p: case AT_rb: case AT_rp:
    case AT_rt: case AT_rtc:
        return true;
    case AT_caption: case AT_colgroup: case AT_tbody: case AT_td: case AT_tfoot: case AT_th: case AT_thead: case AT_tr:
        return thorough;
    default:
        return false;
    }
}

static void gen_implied(HtmlTreeBuilder *tb, WebAtom except) {
    while (tb->open.n && implied(cur(tb), false) && !(except && cur(tb)->name == except)) pop(tb);
}

static void gen_implied_thorough(HtmlTreeBuilder *tb) {
    while (tb->open.n && implied(cur(tb), true)) pop(tb);
}

static void perr(HtmlTreeBuilder *tb, const HtmlToken *t) {
    doc_error(tb->doc, PE_TREE, t ? t->offset : 0);
}

/* ================================================================================================ creating and inserting */

static const struct { const char *from, *to; } svg_tags[] = {
    {"altglyph", "altGlyph"}, {"altglyphdef", "altGlyphDef"}, {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"}, {"animatemotion", "animateMotion"}, {"animatetransform", "animateTransform"},
    {"clippath", "clipPath"}, {"feblend", "feBlend"}, {"fecolormatrix", "feColorMatrix"},
    {"fecomponenttransfer", "feComponentTransfer"}, {"fecomposite", "feComposite"},
    {"feconvolvematrix", "feConvolveMatrix"}, {"fediffuselighting", "feDiffuseLighting"},
    {"fedisplacementmap", "feDisplacementMap"}, {"fedistantlight", "feDistantLight"}, {"fedropshadow", "feDropShadow"},
    {"feflood", "feFlood"}, {"fefunca", "feFuncA"}, {"fefuncb", "feFuncB"}, {"fefuncg", "feFuncG"},
    {"fefuncr", "feFuncR"}, {"fegaussianblur", "feGaussianBlur"}, {"feimage", "feImage"}, {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"}, {"femorphology", "feMorphology"}, {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"}, {"fespecularlighting", "feSpecularLighting"}, {"fespotlight", "feSpotLight"},
    {"fetile", "feTile"}, {"feturbulence", "feTurbulence"}, {"foreignobject", "foreignObject"},
    {"glyphref", "glyphRef"}, {"lineargradient", "linearGradient"}, {"radialgradient", "radialGradient"},
    {"textpath", "textPath"},
};

static const struct { const char *from, *to; } svg_attrs[] = {
    {"attributename", "attributeName"}, {"attributetype", "attributeType"}, {"basefrequency", "baseFrequency"},
    {"baseprofile", "baseProfile"}, {"calcmode", "calcMode"}, {"clippathunits", "clipPathUnits"},
    {"diffuseconstant", "diffuseConstant"}, {"edgemode", "edgeMode"}, {"filterunits", "filterUnits"},
    {"glyphref", "glyphRef"}, {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"}, {"keypoints", "keyPoints"},
    {"keysplines", "keySplines"}, {"keytimes", "keyTimes"}, {"lengthadjust", "lengthAdjust"},
    {"limitingconeangle", "limitingConeAngle"}, {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"}, {"maskcontentunits", "maskContentUnits"}, {"maskunits", "maskUnits"},
    {"numoctaves", "numOctaves"}, {"pathlength", "pathLength"}, {"patterncontentunits", "patternContentUnits"},
    {"patterntransform", "patternTransform"}, {"patternunits", "patternUnits"}, {"pointsatx", "pointsAtX"},
    {"pointsaty", "pointsAtY"}, {"pointsatz", "pointsAtZ"}, {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"}, {"primitiveunits", "primitiveUnits"}, {"refx", "refX"},
    {"refy", "refY"}, {"repeatcount", "repeatCount"}, {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"}, {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"}, {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"}, {"startoffset", "startOffset"}, {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"}, {"surfacescale", "surfaceScale"}, {"systemlanguage", "systemLanguage"},
    {"tablevalues", "tableValues"}, {"targetx", "targetX"}, {"targety", "targetY"}, {"textlength", "textLength"},
    {"viewbox", "viewBox"}, {"viewtarget", "viewTarget"}, {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"}, {"zoomandpan", "zoomAndPan"},
};

static const struct { const char *name; WebAtom prefix; const char *local; uint8_t ns; } foreign_attrs[] = {
    {"xlink:actuate", AT_xlink, "actuate", ATTR_NS_XLINK}, {"xlink:arcrole", AT_xlink, "arcrole", ATTR_NS_XLINK},
    {"xlink:href", AT_xlink, "href", ATTR_NS_XLINK}, {"xlink:role", AT_xlink, "role", ATTR_NS_XLINK},
    {"xlink:show", AT_xlink, "show", ATTR_NS_XLINK}, {"xlink:title", AT_xlink, "title", ATTR_NS_XLINK},
    {"xlink:type", AT_xlink, "type", ATTR_NS_XLINK}, {"xml:lang", AT_xml, "lang", ATTR_NS_XML},
    {"xml:space", AT_xml, "space", ATTR_NS_XML}, {"xmlns", AT_NONE, "xmlns", ATTR_NS_XMLNS},
    {"xmlns:xlink", AT_xmlns, "xlink", ATTR_NS_XMLNS},
};

static WebAtom adjust_name(HtmlTreeBuilder *tb, WebAtom a, const void *table, size_t n) {
    const char *s = atom_str(&tb->doc->atoms, a);
    const struct { const char *from, *to; } *t = table;
    for (size_t i = 0; i < n; i++)
        if (strcmp(s, t[i].from) == 0) return atom_get(&tb->doc->atoms, t[i].to, strlen(t[i].to));
    return a;
}

enum { ADJ_NONE = 0, ADJ_SVG = 1, ADJ_MATHML = 2, ADJ_FOREIGN = 4 };

/* "Create an element for a token": the element with the token's attributes (adjusted for SVG/MathML when asked). */
static WebNode *create_element(HtmlTreeBuilder *tb, const HtmlToken *t, int ns, WebAtom name, int adjust) {
    WebDoc *d = tb->doc;
    WebNode *el = dom_new_element(d, ns, name);
    if (!el) return NULL;
    for (int i = 0; t && i < t->nattrs; i++) {
        const HtmlAttr *a = &t->attrs[i];
        WebAtom an = a->name, prefix = AT_NONE;
        int ans = ATTR_NS_NONE;
        if ((adjust & ADJ_MATHML) && an == AT_definitionurl) an = AT_definitionURL;
        if (adjust & ADJ_SVG) an = adjust_name(tb, an, svg_attrs, sizeof svg_attrs / sizeof svg_attrs[0]);
        if (adjust & ADJ_FOREIGN) {
            const char *s = atom_str(&d->atoms, a->name);
            for (size_t k = 0; k < sizeof foreign_attrs / sizeof foreign_attrs[0]; k++)
                if (strcmp(s, foreign_attrs[k].name) == 0) {
                    an = atom_get(&d->atoms, foreign_attrs[k].local, strlen(foreign_attrs[k].local));
                    prefix = foreign_attrs[k].prefix;
                    ans = foreign_attrs[k].ns;
                    break;
                }
        }
        dom_add_attr(el, an, t->vals + a->voff, a->vlen, ans, prefix);
    }
    if (ns == WEB_NS_MATHML && name == AT_annotation_xml) {
        const char *enc = dom_attr_val(el, AT_encoding);
        if (enc && (strcasecmp(enc, "text/html") == 0 || strcasecmp(enc, "application/xhtml+xml") == 0))
            el->flags |= NF_HTML_INTEGRATION;
    }
    if (ns == WEB_NS_HTML && name == AT_template) {
        el->u.el.content = dom_new_node(d, WEB_NODE_FRAGMENT);
    }
    /* form-associated elements remember the form element pointer (the "form owner" set by the parser) */
    if (ns == WEB_NS_HTML && tb->form && !parsing_template(tb) && !tb->context) {
        switch (name) {
        case AT_button: case AT_fieldset: case AT_input: case AT_object: case AT_output: case AT_select:
        case AT_textarea: case AT_img:
            if (!dom_attr(el, AT_form)) { el->u.el.form = tb->form; el->flags |= NF_FORM_INSERTED; }
            break;
        default:
            break;
        }
    }
    return el;
}

/* "The appropriate place for inserting a node" (with foster parenting), and the template contents adjustment. */
static void insertion_place(HtmlTreeBuilder *tb, WebNode *override, WebNode **parent, WebNode **before) {
    WebNode *target = override ? override : cur(tb);
    *before = NULL;
    if (!override && tb->open.n > (uint32_t)tb->max_depth) target = tb->open.v[tb->max_depth - 1];
    if (tb->foster && target && target->ns == WEB_NS_HTML &&
        (target->name == AT_table || target->name == AT_tbody || target->name == AT_tfoot || target->name == AT_thead ||
         target->name == AT_tr)) {
        int lt = -1, ltab = -1;
        for (int i = (int)tb->open.n - 1; i >= 0; i--) {
            WebNode *n = tb->open.v[i];
            if (lt < 0 && is_html(n, AT_template)) lt = i;
            if (ltab < 0 && is_html(n, AT_table)) ltab = i;
        }
        if (lt >= 0 && (ltab < 0 || lt > ltab)) {
            *parent = tb->open.v[lt]->u.el.content;
            return;
        }
        if (ltab < 0) {
            *parent = tb->open.v[0];
            return;
        }
        WebNode *table = tb->open.v[ltab];
        if (table->parent) {
            *parent = table->parent;
            *before = table;
            return;
        }
        *parent = tb->open.v[ltab - 1];
    } else {
        *parent = target;
    }
    if (is_html(*parent, AT_template)) {
        *parent = (*parent)->u.el.content;
        *before = NULL;
    }
}

static void insert_node_at(WebNode *parent, WebNode *before, WebNode *n) {
    if (!parent || !n) return;
    if (before && before->parent == parent) dom_insert_before(parent, n, before);
    else dom_append(parent, n);
}

static WebNode *insert_element(HtmlTreeBuilder *tb, const HtmlToken *t, int ns, WebAtom name, int adjust) {
    WebNode *el = create_element(tb, t, ns, name, adjust);
    if (!el) { tb->doc->status = WEB_E_NOMEM; return NULL; }
    WebNode *parent, *before;
    insertion_place(tb, NULL, &parent, &before);
    insert_node_at(parent, before, el);
    push(tb, el);
    return el;
}

static WebNode *insert_html(HtmlTreeBuilder *tb, const HtmlToken *t) {
    return insert_element(tb, t, WEB_NS_HTML, t->name, ADJ_NONE);
}

static WebNode *insert_html_name(HtmlTreeBuilder *tb, WebAtom name) {
    return insert_element(tb, NULL, WEB_NS_HTML, name, ADJ_NONE);
}

static void insert_text(HtmlTreeBuilder *tb, const char *p, size_t n) {
    if (!n) return;
    WebNode *parent, *before;
    insertion_place(tb, NULL, &parent, &before);
    if (!parent || parent->type == WEB_NODE_DOCUMENT) return;
    WebNode *prev = before ? before->prev : parent->last;
    if (prev && prev->type == WEB_NODE_TEXT) {
        if (!dom_text_append(prev, p, n)) tb->doc->status = WEB_E_NOMEM;
        return;
    }
    WebNode *tx = dom_new_text(tb->doc, p, n);
    if (!tx) { tb->doc->status = WEB_E_NOMEM; return; }
    insert_node_at(parent, before, tx);
}

static void insert_comment(HtmlTreeBuilder *tb, const HtmlToken *t, WebNode *where) {
    WebNode *parent = where, *before = NULL;
    if (!parent) insertion_place(tb, NULL, &parent, &before);
    WebNode *c = dom_new_comment(tb->doc, t->data, t->len);
    if (!c) { tb->doc->status = WEB_E_NOMEM; return; }
    insert_node_at(parent, before, c);
}

static void insert_pi(HtmlTreeBuilder *tb, const HtmlToken *t, WebNode *where) {
    WebNode *parent = where, *before = NULL;
    if (!parent) insertion_place(tb, NULL, &parent, &before);
    WebNode *pi = dom_new_node(tb->doc, WEB_NODE_PI);
    if (!pi) { tb->doc->status = WEB_E_NOMEM; return; }
    pi->name = t->name;
    dom_set_data(pi, t->data, t->len);
    insert_node_at(parent, before, pi);
}

/* ================================================================================================ active formatting elements */

static bool same_attrs(const FmtEntry *e, const WebNode *el) {
    if (e->nattrs != el->u.el.nattrs) return false;
    for (int i = 0; i < e->nattrs; i++) {
        const WebAttr *a = &e->attrs[i];
        bool found = false;
        for (int k = 0; k < el->u.el.nattrs; k++) {
            const WebAttr *b = &el->u.el.attrs[k];
            if (a->name == b->name && a->ns == b->ns && a->len == b->len && memcmp(a->value, b->value, a->len) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static void fmt_push(HtmlTreeBuilder *tb, WebNode *el) {
    /* Noah's Ark: at most three entries with the same name, namespace and attributes after the last marker */
    int count = 0, earliest = -1;
    for (int i = (int)tb->fmt.n - 1; i >= 0; i--) {
        FmtEntry *e = &tb->fmt.v[i];
        if (!e->el) break;
        if (e->name == el->name && e->el->ns == el->ns && same_attrs(e, el)) {
            count++;
            earliest = i;
        }
    }
    if (count >= 3) {
        memmove(&tb->fmt.v[earliest], &tb->fmt.v[earliest + 1], (tb->fmt.n - (uint32_t)earliest - 1) * sizeof(FmtEntry));
        tb->fmt.n--;
    }
    /* The entry keeps its own copy of the token's attributes: "reconstruct" and the adoption agency create new elements from
     * the token, which a script may have changed on the element in the meantime. */
    FmtEntry e = { el, el->name, NULL, el->u.el.nattrs };
    if (e.nattrs) {
        e.attrs = wa_alloc(tb->a, sizeof(WebAttr) * (size_t)e.nattrs);
        if (e.attrs) {
            memcpy(e.attrs, el->u.el.attrs, sizeof(WebAttr) * (size_t)e.nattrs);
            for (int i = 0; i < e.nattrs; i++) e.attrs[i].value = wa_strndup(tb->a, e.attrs[i].value, e.attrs[i].len);
        } else e.nattrs = 0;
    }
    vec_push(tb->a, tb->fmt, e);
}

static void fmt_marker(HtmlTreeBuilder *tb) {
    FmtEntry e = { NULL, AT_NONE, NULL, 0 };
    vec_push(tb->a, tb->fmt, e);
}

static void fmt_clear_to_marker(HtmlTreeBuilder *tb) {
    while (tb->fmt.n) {
        FmtEntry *e = &tb->fmt.v[--tb->fmt.n];
        if (!e->el) break;
    }
}

static int fmt_index(HtmlTreeBuilder *tb, const WebNode *el) {
    for (int i = (int)tb->fmt.n - 1; i >= 0; i--)
        if (tb->fmt.v[i].el == el) return i;
    return -1;
}

static void fmt_remove_at(HtmlTreeBuilder *tb, int i) {
    memmove(&tb->fmt.v[i], &tb->fmt.v[i + 1], (tb->fmt.n - (uint32_t)i - 1) * sizeof(FmtEntry));
    tb->fmt.n--;
}

static WebNode *element_from_entry(HtmlTreeBuilder *tb, const FmtEntry *e) {
    WebNode *el = dom_new_element(tb->doc, WEB_NS_HTML, e->name);
    if (!el) return NULL;
    for (int i = 0; i < e->nattrs; i++)
        dom_add_attr(el, e->attrs[i].name, e->attrs[i].value, e->attrs[i].len, e->attrs[i].ns, e->attrs[i].prefix);
    return el;
}

static void fmt_reconstruct(HtmlTreeBuilder *tb) {
    if (!tb->fmt.n) return;
    FmtEntry *last = &tb->fmt.v[tb->fmt.n - 1];
    if (!last->el || stack_index(tb, last->el) >= 0) return;
    int i = (int)tb->fmt.n - 1;
    while (i > 0) {                                           /* rewind */
        FmtEntry *e = &tb->fmt.v[i - 1];
        if (!e->el || stack_index(tb, e->el) >= 0) break;
        i--;
    }
    for (; i < (int)tb->fmt.n; i++) {                          /* advance and create */
        FmtEntry *e = &tb->fmt.v[i];
        WebNode *el = element_from_entry(tb, e);
        if (!el) { tb->doc->status = WEB_E_NOMEM; return; }
        WebNode *parent, *before;
        insertion_place(tb, NULL, &parent, &before);
        insert_node_at(parent, before, el);
        push(tb, el);
        tb->fmt.v[i].el = el;
    }
}

/* ================================================================================================ the parsing algorithms */

static void generic_text(HtmlTreeBuilder *tb, const HtmlToken *t, TokState st) {
    insert_html(tb, t);
    tb->tok->state = st;
    tb->orig_mode = tb->mode;
    tb->mode = IM_TEXT;
}

static void close_p(HtmlTreeBuilder *tb) {
    gen_implied(tb, AT_p);
    if (!cur_is(tb, AT_p)) perr(tb, NULL);
    pop_until(tb, AT_p);
}

static void close_p_if_in_button_scope(HtmlTreeBuilder *tb) {
    if (has_in_scope(tb, AT_p, SC_BUTTON)) close_p(tb);
}

static void reset_mode(HtmlTreeBuilder *tb) {
    bool last = false;
    for (int i = (int)tb->open.n - 1; i >= 0; i--) {
        WebNode *node = tb->open.v[i];
        if (i == 0) {
            last = true;
            if (tb->context) node = tb->context;
        }
        if (node->ns == WEB_NS_HTML) {
            switch (node->name) {
            case AT_td: case AT_th: if (!last) { tb->mode = IM_IN_CELL; return; } break;
            case AT_tr: tb->mode = IM_IN_ROW; return;
            case AT_tbody: case AT_thead: case AT_tfoot: tb->mode = IM_IN_TABLE_BODY; return;
            case AT_caption: tb->mode = IM_IN_CAPTION; return;
            case AT_colgroup: tb->mode = IM_IN_COLUMN_GROUP; return;
            case AT_table: tb->mode = IM_IN_TABLE; return;
            case AT_template: tb->mode = tb->tmpl_modes.n ? tb->tmpl_modes.v[tb->tmpl_modes.n - 1] : IM_IN_TEMPLATE; return;
            case AT_head: if (!last) { tb->mode = IM_IN_HEAD; return; } break;
            case AT_body: tb->mode = IM_IN_BODY; return;
            case AT_frameset: tb->mode = IM_IN_FRAMESET; return;
            case AT_html: tb->mode = tb->head ? IM_AFTER_HEAD : IM_BEFORE_HEAD; return;
            default: break;
            }
        }
        if (last) { tb->mode = IM_IN_BODY; return; }
    }
    tb->mode = IM_IN_BODY;
}

static void mode_dispatch(HtmlTreeBuilder *tb, HtmlToken *t);
static void m_in_body(HtmlTreeBuilder *tb, HtmlToken *t);
static void m_in_head(HtmlTreeBuilder *tb, HtmlToken *t);
static void m_in_template(HtmlTreeBuilder *tb, HtmlToken *t);
static void any_other_end_tag(HtmlTreeBuilder *tb, HtmlToken *t);

static bool is_start(const HtmlToken *t, WebAtom name) { return t->type == HT_START && t->name == name; }
static bool is_end(const HtmlToken *t, WebAtom name) { return t->type == HT_END && t->name == name; }

static bool all_ws(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\f' && p[i] != '\r') return false;
    return true;
}

static bool is_ws_token(const HtmlToken *t) { return t->type == HT_CHARS && t->len && all_ws(t->data, t->len); }

static const char *tok_attr(const HtmlToken *t, WebAtom name, uint32_t *len) {
    for (int i = 0; i < t->nattrs; i++)
        if (t->attrs[i].name == name) {
            if (len) *len = t->attrs[i].vlen;
            return t->vals + t->attrs[i].voff;
        }
    return NULL;
}

/* ---------------------------------------------------------------------------------------- the adoption agency algorithm */

static bool adoption(HtmlTreeBuilder *tb, HtmlToken *t) {
    WebAtom subject = t->name;
    WebNode *c = cur(tb);
    if (is_html(c, subject) && fmt_index(tb, c) < 0) {
        pop(tb);
        return true;
    }
    for (int outer = 0; outer < 8; outer++) {
        int fi = -1;
        for (int i = (int)tb->fmt.n - 1; i >= 0; i--) {
            if (!tb->fmt.v[i].el) break;
            if (tb->fmt.v[i].el->name == subject) { fi = i; break; }
        }
        if (fi < 0) return false;                             /* act as "any other end tag" */
        WebNode *fe = tb->fmt.v[fi].el;
        int si = stack_index(tb, fe);
        if (si < 0) {
            perr(tb, t);
            fmt_remove_at(tb, fi);
            return true;
        }
        if (!has_node_in_scope(tb, fe, SC_DEFAULT)) {
            perr(tb, t);
            return true;
        }
        if (fe != cur(tb)) perr(tb, t);
        int fbi = -1;
        for (int i = si + 1; i < (int)tb->open.n; i++)
            if (is_special(tb->open.v[i])) { fbi = i; break; }
        if (fbi < 0) {
            while (tb->open.n > (uint32_t)si) pop(tb);
            fmt_remove_at(tb, fmt_index(tb, fe));
            return true;
        }
        WebNode *fb = tb->open.v[fbi];
        WebNode *common = tb->open.v[si - 1];
        int bookmark = fi;
        WebNode *node = fb, *last = fb;
        int ni = fbi;
        bool moved = false;
        for (int inner = 1;; inner++) {
            /* the element above node in the stack (node may have been removed: then the one that was above it) */
            int idx = stack_index(tb, node);
            ni = (idx >= 0 ? idx : ni) - 1;
            node = tb->open.v[ni];
            if (node == fe) break;
            int nfi = fmt_index(tb, node);
            if (inner > 3 && nfi >= 0) {
                fmt_remove_at(tb, nfi);
                if (nfi < bookmark) bookmark--;
                nfi = -1;
            }
            if (nfi < 0) {
                stack_remove(tb, node);
                continue;
            }
            WebNode *ne = element_from_entry(tb, &tb->fmt.v[nfi]);
            if (!ne) { tb->doc->status = WEB_E_NOMEM; return true; }
            tb->fmt.v[nfi].el = ne;
            tb->open.v[ni] = ne;
            node = ne;
            if (!moved) { bookmark = nfi + 1; moved = true; }
            if (last->parent) dom_remove(last);
            dom_append(node, last);
            last = node;
        }
        WebNode *parent, *before;
        insertion_place(tb, common, &parent, &before);
        if (last->parent) dom_remove(last);
        if (!web_node_contains(last, parent)) insert_node_at(parent, before, last);
        FmtEntry fentry = tb->fmt.v[fmt_index(tb, fe)];
        WebNode *ne = element_from_entry(tb, &fentry);
        if (!ne) { tb->doc->status = WEB_E_NOMEM; return true; }
        while (fb->first) {
            WebNode *k = fb->first;
            dom_remove(k);
            dom_append(ne, k);
        }
        dom_append(is_html(fb, AT_template) ? fb->u.el.content : fb, ne);   /* "the adjusted insertion location" */
        int old = fmt_index(tb, fe);
        fentry.el = ne;
        fmt_remove_at(tb, old);
        if (old < bookmark) bookmark--;
        if (bookmark > (int)tb->fmt.n) bookmark = (int)tb->fmt.n;
        if (vec_reserve(tb->a, tb->fmt, tb->fmt.n + 1)) {
            memmove(&tb->fmt.v[bookmark + 1], &tb->fmt.v[bookmark], (tb->fmt.n - (uint32_t)bookmark) * sizeof(FmtEntry));
            tb->fmt.v[bookmark] = fentry;
            tb->fmt.n++;
        }
        stack_remove(tb, fe);
        stack_insert(tb, stack_index(tb, fb) + 1, ne);
    }
    return true;
}

/* ================================================================================================ insertion modes */

/* The quirks mode tables of the "initial" insertion mode */
static const char *const quirks_exact_pub[] = { "-//w3o//dtd w3 html strict 3.0//en//", "-/w3c/dtd html 4.0 transitional/en", "html" };
static const char *const quirks_prefix_pub[] = {
    "+//silmaril//dtd html pro v0r11 19970101//", "-//as//dtd html 3.0 aswedit + extensions//",
    "-//advasoft ltd//dtd html 3.0 aswedit + extensions//", "-//ietf//dtd html 2.0 level 1//",
    "-//ietf//dtd html 2.0 level 2//", "-//ietf//dtd html 2.0 strict level 1//", "-//ietf//dtd html 2.0 strict level 2//",
    "-//ietf//dtd html 2.0 strict//", "-//ietf//dtd html 2.0//", "-//ietf//dtd html 2.1e//", "-//ietf//dtd html 3.0//",
    "-//ietf//dtd html 3.2 final//", "-//ietf//dtd html 3.2//", "-//ietf//dtd html 3//", "-//ietf//dtd html level 0//",
    "-//ietf//dtd html level 1//", "-//ietf//dtd html level 2//", "-//ietf//dtd html level 3//",
    "-//ietf//dtd html strict level 0//", "-//ietf//dtd html strict level 1//", "-//ietf//dtd html strict level 2//",
    "-//ietf//dtd html strict level 3//", "-//ietf//dtd html strict//", "-//ietf//dtd html//",
    "-//metrius//dtd metrius presentational//", "-//microsoft//dtd internet explorer 2.0 html strict//",
    "-//microsoft//dtd internet explorer 2.0 html//", "-//microsoft//dtd internet explorer 2.0 tables//",
    "-//microsoft//dtd internet explorer 3.0 html strict//", "-//microsoft//dtd internet explorer 3.0 html//",
    "-//microsoft//dtd internet explorer 3.0 tables//", "-//netscape comm. corp.//dtd html//",
    "-//netscape comm. corp.//dtd strict html//", "-//o'reilly and associates//dtd html 2.0//",
    "-//o'reilly and associates//dtd html extended 1.0//", "-//o'reilly and associates//dtd html extended relaxed 1.0//",
    "-//sq//dtd html 2.0 hotmetal + extensions//",
    "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
    "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//", "-//spyglass//dtd html 2.0 extended//",
    "-//sun microsystems corp.//dtd hotjava html//", "-//sun microsystems corp.//dtd hotjava strict html//",
    "-//w3c//dtd html 3 1995-03-24//", "-//w3c//dtd html 3.2 draft//", "-//w3c//dtd html 3.2 final//",
    "-//w3c//dtd html 3.2//", "-//w3c//dtd html 3.2s draft//", "-//w3c//dtd html 4.0 frameset//",
    "-//w3c//dtd html 4.0 transitional//", "-//w3c//dtd html experimental 19960712//",
    "-//w3c//dtd html experimental 970421//", "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html 3.0//",
    "-//webtechs//dtd mozilla html 2.0//", "-//webtechs//dtd mozilla html//",
};

static bool prefix_ci(const char *s, size_t n, const char *pre) {
    size_t k = strlen(pre);
    return n >= k && strncasecmp(s, pre, k) == 0;
}

static int doctype_quirks(const HtmlToken *t) {
    if (t->force_quirks) return 2;
    if (t->dt_name_missing || t->len != 4 || memcmp(t->data, "html", 4) != 0) return 2;
    const char *pub = t->dt_pub_missing ? NULL : t->dt_pub;
    size_t pn = t->dt_pub_len;
    bool sys_missing_or_empty = t->dt_sys_missing || t->dt_sys_len == 0;
    if (!t->dt_sys_missing && t->dt_sys_len == 58 &&
        strncasecmp(t->dt_sys, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd", 58) == 0) return 2;
    if (pub) {
        for (size_t i = 0; i < sizeof quirks_exact_pub / sizeof quirks_exact_pub[0]; i++)
            if (pn == strlen(quirks_exact_pub[i]) && strncasecmp(pub, quirks_exact_pub[i], pn) == 0) return 2;
        for (size_t i = 0; i < sizeof quirks_prefix_pub / sizeof quirks_prefix_pub[0]; i++)
            if (prefix_ci(pub, pn, quirks_prefix_pub[i])) return 2;
        if (sys_missing_or_empty && (prefix_ci(pub, pn, "-//w3c//dtd html 4.01 frameset//") ||
                                     prefix_ci(pub, pn, "-//w3c//dtd html 4.01 transitional//"))) return 2;
        if (prefix_ci(pub, pn, "-//w3c//dtd xhtml 1.0 frameset//") || prefix_ci(pub, pn, "-//w3c//dtd xhtml 1.0 transitional//"))
            return 1;
        if (!sys_missing_or_empty && (prefix_ci(pub, pn, "-//w3c//dtd html 4.01 frameset//") ||
                                      prefix_ci(pub, pn, "-//w3c//dtd html 4.01 transitional//"))) return 1;
    }
    return 0;
}

static void m_initial(HtmlTreeBuilder *tb, HtmlToken *t) {
    WebDoc *d = tb->doc;
    if (is_ws_token(t)) return;
    if (t->type == HT_COMMENT) { insert_comment(tb, t, d->root); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, d->root); return; }
    if (t->type == HT_DOCTYPE) {
        bool ok = !t->dt_name_missing && t->len == 4 && memcmp(t->data, "html", 4) == 0 && t->dt_pub_missing &&
                  (t->dt_sys_missing || (t->dt_sys_len == 19 && memcmp(t->dt_sys, "about:legacy-compat", 19) == 0));
        if (!ok) perr(tb, t);
        bool has = false;
        for (WebNode *k = d->root->first; k; k = k->next)
            if (k->type == WEB_NODE_DOCTYPE || k->type == WEB_NODE_ELEMENT) has = true;
        if (!has) {
            WebNode *dt = dom_new_node(d, WEB_NODE_DOCTYPE);
            if (dt) {
                dom_set_data(dt, t->dt_name_missing ? "" : t->data, t->dt_name_missing ? 0 : t->len);
                dt->u.cd.public_id = wa_strndup(&d->arena, t->dt_pub_missing ? "" : t->dt_pub, t->dt_pub_missing ? 0 : t->dt_pub_len);
                dt->u.cd.system_id = wa_strndup(&d->arena, t->dt_sys_missing ? "" : t->dt_sys, t->dt_sys_missing ? 0 : t->dt_sys_len);
                dom_append(d->root, dt);
            }
        }
        d->quirks = doctype_quirks(t);
        tb->mode = IM_BEFORE_HTML;
        return;
    }
    perr(tb, t);
    d->quirks = 2;
    tb->mode = IM_BEFORE_HTML;
    mode_dispatch(tb, t);
}

static void m_before_html(HtmlTreeBuilder *tb, HtmlToken *t) {
    WebDoc *d = tb->doc;
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, d->root); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, d->root); return; }
    if (is_ws_token(t)) return;
    if (is_start(t, AT_html)) {
        WebNode *el = create_element(tb, t, WEB_NS_HTML, AT_html, ADJ_NONE);
        if (!el) { d->status = WEB_E_NOMEM; return; }
        dom_append(d->root, el);
        push(tb, el);
        tb->mode = IM_BEFORE_HEAD;
        return;
    }
    if (t->type == HT_END && !(t->name == AT_head || t->name == AT_body || t->name == AT_html || t->name == AT_br)) {
        perr(tb, t);
        return;
    }
    WebNode *el = dom_new_element(d, WEB_NS_HTML, AT_html);
    if (!el) { d->status = WEB_E_NOMEM; return; }
    dom_append(d->root, el);
    push(tb, el);
    tb->mode = IM_BEFORE_HEAD;
    mode_dispatch(tb, t);
}

static void m_before_head(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_ws_token(t)) return;
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_start(t, AT_head)) {
        tb->head = insert_html(tb, t);
        tb->mode = IM_IN_HEAD;
        return;
    }
    if (t->type == HT_END && !(t->name == AT_head || t->name == AT_body || t->name == AT_html || t->name == AT_br)) {
        perr(tb, t);
        return;
    }
    tb->head = insert_html_name(tb, AT_head);
    tb->mode = IM_IN_HEAD;
    mode_dispatch(tb, t);
}

void parser_meta_charset(WebDoc *d, WebNode *meta);        /* parser.c: change the encoding if still tentative */

static void m_in_head(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_ws_token(t)) { insert_text(tb, t->data, t->len); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (t->type == HT_START) {
        switch (t->name) {
        case AT_html: m_in_body(tb, t); return;
        case AT_base: case AT_basefont: case AT_bgsound: case AT_link:
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            return;
        case AT_meta: {
            WebNode *m = insert_html(tb, t);
            pop(tb);
            t->ack = true;
            if (m) parser_meta_charset(tb->doc, m);
            return;
        }
        case AT_title: generic_text(tb, t, TS_RCDATA); return;
        case AT_noscript:
            if (tb->scripting) { generic_text(tb, t, TS_RAWTEXT); return; }
            insert_html(tb, t);
            tb->mode = IM_IN_HEAD_NOSCRIPT;
            return;
        case AT_noframes: case AT_style: generic_text(tb, t, TS_RAWTEXT); return;
        case AT_script: {
            WebNode *parent, *before;
            insertion_place(tb, NULL, &parent, &before);
            WebNode *s = create_element(tb, t, WEB_NS_HTML, AT_script, ADJ_NONE);
            if (!s) { tb->doc->status = WEB_E_NOMEM; return; }
            s->flags |= NF_PARSER_INSERTED;
            if (tb->fragment) s->flags |= NF_STARTED;
            insert_node_at(parent, before, s);
            push(tb, s);
            tb->tok->state = TS_SCRIPT_DATA;
            tb->orig_mode = tb->mode;
            tb->mode = IM_TEXT;
            return;
        }
        case AT_template:
            insert_html(tb, t);
            fmt_marker(tb);
            tb->frameset_ok = false;
            tb->mode = IM_IN_TEMPLATE;
            vec_push(tb->a, tb->tmpl_modes, IM_IN_TEMPLATE);
            return;
        case AT_head: perr(tb, t); return;
        default: break;
        }
    } else if (t->type == HT_END) {
        if (t->name == AT_head) {
            pop(tb);
            tb->mode = IM_AFTER_HEAD;
            return;
        }
        if (t->name == AT_template) {
            if (!stack_has(tb, AT_template)) { perr(tb, t); return; }
            gen_implied_thorough(tb);
            if (!cur_is(tb, AT_template)) perr(tb, t);
            pop_until(tb, AT_template);
            fmt_clear_to_marker(tb);
            if (tb->tmpl_modes.n) tb->tmpl_modes.n--;
            reset_mode(tb);
            return;
        }
        if (!(t->name == AT_body || t->name == AT_html || t->name == AT_br)) { perr(tb, t); return; }
    }
    pop(tb);
    tb->mode = IM_AFTER_HEAD;
    mode_dispatch(tb, t);
}

static void m_in_head_noscript(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_end(t, AT_noscript)) {
        pop(tb);
        tb->mode = IM_IN_HEAD;
        return;
    }
    if (is_ws_token(t) || t->type == HT_COMMENT || t->type == HT_PI ||
        (t->type == HT_START && (t->name == AT_basefont || t->name == AT_bgsound || t->name == AT_link ||
                                 t->name == AT_meta || t->name == AT_noframes || t->name == AT_style))) {
        m_in_head(tb, t);
        return;
    }
    if ((t->type == HT_START && (t->name == AT_head || t->name == AT_noscript)) || (t->type == HT_END && t->name != AT_br)) {
        perr(tb, t);
        return;
    }
    perr(tb, t);
    pop(tb);
    tb->mode = IM_IN_HEAD;
    mode_dispatch(tb, t);
}

static void m_after_head(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_ws_token(t)) { insert_text(tb, t->data, t->len); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (t->type == HT_START) {
        switch (t->name) {
        case AT_html: m_in_body(tb, t); return;
        case AT_body:
            insert_html(tb, t);
            tb->frameset_ok = false;
            tb->mode = IM_IN_BODY;
            return;
        case AT_frameset:
            insert_html(tb, t);
            tb->mode = IM_IN_FRAMESET;
            return;
        case AT_base: case AT_basefont: case AT_bgsound: case AT_link: case AT_meta: case AT_noframes:
        case AT_script: case AT_style: case AT_template: case AT_title: {
            perr(tb, t);
            WebNode *head = tb->head;
            if (!head) break;
            push(tb, head);
            m_in_head(tb, t);
            stack_remove(tb, head);
            return;
        }
        case AT_head: perr(tb, t); return;
        default: break;
        }
    } else if (t->type == HT_END) {
        if (t->name == AT_template) { m_in_head(tb, t); return; }
        if (!(t->name == AT_body || t->name == AT_html || t->name == AT_br)) { perr(tb, t); return; }
    }
    insert_html_name(tb, AT_body);
    tb->frameset_ok = true;                                  /* an implied body: a <template> in the head does not count */
    tb->mode = IM_IN_BODY;
    mode_dispatch(tb, t);
}

static bool body_end_ok(HtmlTreeBuilder *tb) {
    for (uint32_t i = 0; i < tb->open.n; i++) {
        WebNode *n = tb->open.v[i];
        if (n->ns != WEB_NS_HTML) return false;
        switch (n->name) {
        case AT_dd: case AT_dt: case AT_li: case AT_optgroup: case AT_option: case AT_p: case AT_rb: case AT_rp:
        case AT_rt: case AT_rtc: case AT_tbody: case AT_td: case AT_tfoot: case AT_th: case AT_thead: case AT_tr:
        case AT_body: case AT_html:
            break;
        default:
            return false;
        }
    }
    return true;
}

static void body_chars(HtmlTreeBuilder *tb, const char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && p[i] != 0) i++;
        if (i > s) {
            fmt_reconstruct(tb);
            insert_text(tb, p + s, i - s);
            if (!all_ws(p + s, i - s)) tb->frameset_ok = false;
        }
        while (i < n && p[i] == 0) { perr(tb, NULL); i++; }
    }
}

static void m_in_body(HtmlTreeBuilder *tb, HtmlToken *t) {
    WebDoc *d = tb->doc;
    switch (t->type) {
    case HT_CHARS: body_chars(tb, t->data, t->len); return;
    case HT_COMMENT: insert_comment(tb, t, NULL); return;
    case HT_PI: insert_pi(tb, t, NULL); return;
    case HT_DOCTYPE: perr(tb, t); return;
    case HT_EOF:
        if (tb->tmpl_modes.n) { m_in_template(tb, t); return; }
        if (!body_end_ok(tb)) perr(tb, t);
        tb->done = true;
        return;
    case HT_START:
        switch (t->name) {
        case AT_html:
            perr(tb, t);
            if (stack_has(tb, AT_template) || !tb->open.n) return;
            for (int i = 0; i < t->nattrs; i++)
                if (!dom_attr(tb->open.v[0], t->attrs[i].name))
                    dom_add_attr(tb->open.v[0], t->attrs[i].name, t->vals + t->attrs[i].voff, t->attrs[i].vlen, ATTR_NS_NONE, AT_NONE);
            return;
        case AT_base: case AT_basefont: case AT_bgsound: case AT_link: case AT_meta: case AT_noframes: case AT_script:
        case AT_style: case AT_template: case AT_title:
            m_in_head(tb, t);
            return;
        case AT_body: {
            perr(tb, t);
            if (tb->open.n < 2 || !is_html(tb->open.v[1], AT_body) || stack_has(tb, AT_template)) return;
            tb->frameset_ok = false;
            WebNode *body = tb->open.v[1];
            for (int i = 0; i < t->nattrs; i++)
                if (!dom_attr(body, t->attrs[i].name))
                    dom_add_attr(body, t->attrs[i].name, t->vals + t->attrs[i].voff, t->attrs[i].vlen, ATTR_NS_NONE, AT_NONE);
            return;
        }
        case AT_frameset:
            perr(tb, t);
            if (tb->open.n < 2 || !is_html(tb->open.v[1], AT_body)) return;
            if (!tb->frameset_ok) return;
            if (tb->open.v[1]->parent) dom_remove(tb->open.v[1]);
            while (tb->open.n > 1) pop(tb);
            insert_html(tb, t);
            tb->mode = IM_IN_FRAMESET;
            return;
        case AT_address: case AT_article: case AT_aside: case AT_blockquote: case AT_center: case AT_details:
        case AT_dialog: case AT_dir: case AT_div: case AT_dl: case AT_fieldset: case AT_figcaption: case AT_figure:
        case AT_footer: case AT_header: case AT_hgroup: case AT_main: case AT_menu: case AT_nav: case AT_ol: case AT_p:
        case AT_search: case AT_section: case AT_summary: case AT_ul:
            close_p_if_in_button_scope(tb);
            insert_html(tb, t);
            return;
        case AT_h1: case AT_h2: case AT_h3: case AT_h4: case AT_h5: case AT_h6:
            close_p_if_in_button_scope(tb);
            if (cur(tb) && cur(tb)->ns == WEB_NS_HTML && is_h(cur(tb)->name)) { perr(tb, t); pop(tb); }
            insert_html(tb, t);
            return;
        case AT_pre: case AT_listing:
            close_p_if_in_button_scope(tb);
            insert_html(tb, t);
            tb->ignore_lf = true;
            tb->frameset_ok = false;
            return;
        case AT_form: {
            bool in_template = parsing_template(tb);
            if (tb->form && !in_template) { perr(tb, t); return; }
            close_p_if_in_button_scope(tb);
            WebNode *f = insert_html(tb, t);
            if (!in_template) tb->form = f;
            return;
        }
        case AT_li: case AT_dd: case AT_dt: {
            tb->frameset_ok = false;
            for (int i = (int)tb->open.n - 1; i >= 0; i--) {
                WebNode *node = tb->open.v[i];
                WebAtom hit = AT_NONE;
                if (t->name == AT_li && is_html(node, AT_li)) hit = AT_li;
                if (t->name != AT_li && (is_html(node, AT_dd) || is_html(node, AT_dt))) hit = node->name;
                if (hit) {
                    gen_implied(tb, hit);
                    if (!cur_is(tb, hit)) perr(tb, t);
                    pop_until(tb, hit);
                    break;
                }
                if (is_special(node) && !(is_html(node, AT_address) || is_html(node, AT_div) || is_html(node, AT_p))) break;
            }
            close_p_if_in_button_scope(tb);
            insert_html(tb, t);
            return;
        }
        case AT_plaintext:
            close_p_if_in_button_scope(tb);
            insert_html(tb, t);
            tb->tok->state = TS_PLAINTEXT;
            return;
        case AT_button:
            if (has_in_scope(tb, AT_button, SC_DEFAULT)) {
                perr(tb, t);
                gen_implied(tb, AT_NONE);
                pop_until(tb, AT_button);
            }
            fmt_reconstruct(tb);
            insert_html(tb, t);
            tb->frameset_ok = false;
            return;
        case AT_a: {
            for (int i = (int)tb->fmt.n - 1; i >= 0; i--) {
                WebNode *e = tb->fmt.v[i].el;
                if (!e) break;
                if (e->name == AT_a) {
                    perr(tb, t);
                    HtmlToken end = *t;
                    end.type = HT_END;
                    end.nattrs = 0;
                    if (!adoption(tb, &end)) any_other_end_tag(tb, &end);
                    int fi = fmt_index(tb, e);
                    if (fi >= 0) fmt_remove_at(tb, fi);
                    stack_remove(tb, e);
                    break;
                }
            }
            fmt_reconstruct(tb);
            WebNode *el = insert_html(tb, t);
            if (el) fmt_push(tb, el);
            return;
        }
        case AT_b: case AT_big: case AT_code: case AT_em: case AT_font: case AT_i: case AT_s: case AT_small:
        case AT_strike: case AT_strong: case AT_tt: case AT_u: {
            fmt_reconstruct(tb);
            WebNode *el = insert_html(tb, t);
            if (el) fmt_push(tb, el);
            return;
        }
        case AT_nobr: {
            fmt_reconstruct(tb);
            if (has_in_scope(tb, AT_nobr, SC_DEFAULT)) {
                perr(tb, t);
                HtmlToken end = *t;
                end.type = HT_END;
                end.nattrs = 0;
                if (!adoption(tb, &end)) any_other_end_tag(tb, &end);
                fmt_reconstruct(tb);
            }
            WebNode *el = insert_html(tb, t);
            if (el) fmt_push(tb, el);
            return;
        }
        case AT_applet: case AT_marquee: case AT_object:
            fmt_reconstruct(tb);
            insert_html(tb, t);
            fmt_marker(tb);
            tb->frameset_ok = false;
            return;
        case AT_table:
            if (d->quirks != 2) close_p_if_in_button_scope(tb);
            insert_html(tb, t);
            tb->frameset_ok = false;
            tb->mode = IM_IN_TABLE;
            return;
        case AT_area: case AT_br: case AT_embed: case AT_img: case AT_keygen: case AT_wbr:
            fmt_reconstruct(tb);
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            tb->frameset_ok = false;
            return;
        case AT_input: {
            if (is_html(tb->context, AT_select)) { perr(tb, t); return; }
            if (has_in_scope(tb, AT_select, SC_DEFAULT)) { perr(tb, t); pop_until(tb, AT_select); }
            fmt_reconstruct(tb);
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            uint32_t tl;
            const char *type = tok_attr(t, AT_type, &tl);
            if (!type || !(tl == 6 && strncasecmp(type, "hidden", 6) == 0)) tb->frameset_ok = false;
            return;
        }
        case AT_param: case AT_source: case AT_track:
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            return;
        case AT_hr:
            close_p_if_in_button_scope(tb);
            if (has_in_scope(tb, AT_select, SC_DEFAULT)) {
                gen_implied(tb, AT_NONE);
                if (has_in_scope(tb, AT_option, SC_DEFAULT) || has_in_scope(tb, AT_optgroup, SC_DEFAULT)) perr(tb, t);
            }
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            tb->frameset_ok = false;
            return;
        case AT_image:
            perr(tb, t);
            t->name = AT_img;
            m_in_body(tb, t);
            return;
        case AT_textarea:
            insert_html(tb, t);
            tb->ignore_lf = true;
            tb->tok->state = TS_RCDATA;
            tb->orig_mode = tb->mode;
            tb->frameset_ok = false;
            tb->mode = IM_TEXT;
            return;
        case AT_xmp:
            close_p_if_in_button_scope(tb);
            fmt_reconstruct(tb);
            tb->frameset_ok = false;
            generic_text(tb, t, TS_RAWTEXT);
            return;
        case AT_iframe:
            tb->frameset_ok = false;
            generic_text(tb, t, TS_RAWTEXT);
            return;
        case AT_noembed:
            generic_text(tb, t, TS_RAWTEXT);
            return;
        case AT_noscript:
            if (tb->scripting) { generic_text(tb, t, TS_RAWTEXT); return; }
            break;
        case AT_select:
            if (is_html(tb->context, AT_select)) { perr(tb, t); return; }
            if (has_in_scope(tb, AT_select, SC_DEFAULT)) {
                perr(tb, t);
                pop_until(tb, AT_select);
                return;
            }
            fmt_reconstruct(tb);
            insert_html(tb, t);
            tb->frameset_ok = false;
            return;
        case AT_option:
            if (has_in_scope(tb, AT_select, SC_DEFAULT)) {
                gen_implied(tb, AT_optgroup);
                if (has_in_scope(tb, AT_option, SC_DEFAULT)) perr(tb, t);
            } else if (cur_is(tb, AT_option)) pop(tb);
            fmt_reconstruct(tb);
            insert_html(tb, t);
            return;
        case AT_optgroup:
            if (has_in_scope(tb, AT_select, SC_DEFAULT)) {
                gen_implied(tb, AT_NONE);
                if (has_in_scope(tb, AT_option, SC_DEFAULT) || has_in_scope(tb, AT_optgroup, SC_DEFAULT)) perr(tb, t);
            } else if (cur_is(tb, AT_option)) pop(tb);
            fmt_reconstruct(tb);
            insert_html(tb, t);
            return;
        case AT_rb: case AT_rtc:
            if (has_in_scope(tb, AT_ruby, SC_DEFAULT)) {
                gen_implied(tb, AT_NONE);
                if (!cur_is(tb, AT_ruby)) perr(tb, t);
            }
            insert_html(tb, t);
            return;
        case AT_rp: case AT_rt:
            if (has_in_scope(tb, AT_ruby, SC_DEFAULT)) {
                gen_implied(tb, AT_rtc);
                if (!cur_is(tb, AT_rtc) && !cur_is(tb, AT_ruby)) perr(tb, t);
            }
            insert_html(tb, t);
            return;
        case AT_math:
            fmt_reconstruct(tb);
            insert_element(tb, t, WEB_NS_MATHML, AT_math, ADJ_MATHML | ADJ_FOREIGN);
            if (t->self_closing) { pop(tb); t->ack = true; }
            return;
        case AT_svg:
            fmt_reconstruct(tb);
            insert_element(tb, t, WEB_NS_SVG, AT_svg, ADJ_SVG | ADJ_FOREIGN);
            if (t->self_closing) { pop(tb); t->ack = true; }
            return;
        case AT_caption: case AT_col: case AT_colgroup: case AT_frame: case AT_head: case AT_tbody: case AT_td:
        case AT_tfoot: case AT_th: case AT_thead: case AT_tr:
            perr(tb, t);
            return;
        default:
            break;
        }
        fmt_reconstruct(tb);
        insert_html(tb, t);
        return;
    case HT_END:
        switch (t->name) {
        case AT_template: m_in_head(tb, t); return;
        case AT_body:
        case AT_html:
            if (!has_in_scope(tb, AT_body, SC_DEFAULT)) { perr(tb, t); return; }
            if (!body_end_ok(tb)) perr(tb, t);
            tb->mode = IM_AFTER_BODY;
            if (t->name == AT_html) mode_dispatch(tb, t);
            return;
        case AT_address: case AT_article: case AT_aside: case AT_blockquote: case AT_button: case AT_center:
        case AT_details: case AT_dialog: case AT_dir: case AT_div: case AT_dl: case AT_fieldset: case AT_figcaption:
        case AT_figure: case AT_footer: case AT_header: case AT_hgroup: case AT_listing: case AT_main: case AT_menu:
        case AT_nav: case AT_ol: case AT_pre: case AT_search: case AT_section: case AT_select: case AT_summary:
        case AT_ul:
            if (!has_in_scope(tb, t->name, SC_DEFAULT)) { perr(tb, t); return; }
            gen_implied(tb, AT_NONE);
            if (!cur_is(tb, t->name)) perr(tb, t);
            pop_until(tb, t->name);
            return;
        case AT_form:
            if (!parsing_template(tb)) {
                WebNode *node = tb->form;
                tb->form = NULL;
                if (!node || !has_node_in_scope(tb, node, SC_DEFAULT)) { perr(tb, t); return; }
                gen_implied(tb, AT_NONE);
                if (cur(tb) != node) perr(tb, t);
                stack_remove(tb, node);
            } else {
                if (!has_in_scope(tb, AT_form, SC_DEFAULT)) { perr(tb, t); return; }
                gen_implied(tb, AT_NONE);
                if (!cur_is(tb, AT_form)) perr(tb, t);
                pop_until(tb, AT_form);
            }
            return;
        case AT_p:
            if (!has_in_scope(tb, AT_p, SC_BUTTON)) {
                perr(tb, t);
                insert_html_name(tb, AT_p);
            }
            close_p(tb);
            return;
        case AT_li:
            if (!has_in_scope(tb, AT_li, SC_LIST)) { perr(tb, t); return; }
            gen_implied(tb, AT_li);
            if (!cur_is(tb, AT_li)) perr(tb, t);
            pop_until(tb, AT_li);
            return;
        case AT_dd: case AT_dt:
            if (!has_in_scope(tb, t->name, SC_DEFAULT)) { perr(tb, t); return; }
            gen_implied(tb, t->name);
            if (!cur_is(tb, t->name)) perr(tb, t);
            pop_until(tb, t->name);
            return;
        case AT_h1: case AT_h2: case AT_h3: case AT_h4: case AT_h5: case AT_h6:
            if (!has_h_in_scope(tb)) { perr(tb, t); return; }
            gen_implied(tb, AT_NONE);
            if (!cur_is(tb, t->name)) perr(tb, t);
            while (tb->open.n) {
                WebNode *n = cur(tb);
                pop(tb);
                if (n->ns == WEB_NS_HTML && is_h(n->name)) break;
            }
            return;
        case AT_a: case AT_b: case AT_big: case AT_code: case AT_em: case AT_font: case AT_i: case AT_nobr: case AT_s:
        case AT_small: case AT_strike: case AT_strong: case AT_tt: case AT_u:
            if (!adoption(tb, t)) any_other_end_tag(tb, t);
            return;
        case AT_applet: case AT_marquee: case AT_object:
            if (!has_in_scope(tb, t->name, SC_DEFAULT)) { perr(tb, t); return; }
            gen_implied(tb, AT_NONE);
            if (!cur_is(tb, t->name)) perr(tb, t);
            pop_until(tb, t->name);
            fmt_clear_to_marker(tb);
            return;
        case AT_br: {
            perr(tb, t);
            HtmlToken br = *t;
            br.type = HT_START;
            br.nattrs = 0;
            m_in_body(tb, &br);
            return;
        }
        default:
            any_other_end_tag(tb, t);
            return;
        }
    }
}

static void any_other_end_tag(HtmlTreeBuilder *tb, HtmlToken *t) {
    for (int i = (int)tb->open.n - 1; i >= 0; i--) {
        WebNode *node = tb->open.v[i];
        if (is_html(node, t->name)) {
            gen_implied(tb, t->name);
            if (node != cur(tb)) perr(tb, t);
            pop_until_node(tb, node);
            return;
        }
        if (is_special(node)) {
            perr(tb, t);
            return;
        }
    }
}

void parser_script_end(HtmlTreeBuilder *tb, WebNode *script);   /* parser.c: prepare (run or fetch) a parser-inserted script */

static void m_text(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS) { insert_text(tb, t->data, t->len); return; }
    if (t->type == HT_EOF) {
        perr(tb, t);
        if (cur_is(tb, AT_script)) cur(tb)->flags |= NF_STARTED;
        pop(tb);
        tb->mode = tb->orig_mode;
        mode_dispatch(tb, t);
        return;
    }
    if (is_end(t, AT_script)) {
        WebNode *s = cur(tb);
        pop(tb);
        tb->mode = tb->orig_mode;
        if (s) parser_script_end(tb, s);
        return;
    }
    if (t->type == HT_END) {
        pop(tb);
        tb->mode = tb->orig_mode;
    }
}

static void clear_to_table_context(HtmlTreeBuilder *tb) {
    while (tb->open.n && !(cur_is(tb, AT_table) || cur_is(tb, AT_template) || cur_is(tb, AT_html))) pop(tb);
}

static void clear_to_tbody_context(HtmlTreeBuilder *tb) {
    while (tb->open.n && !(cur_is(tb, AT_tbody) || cur_is(tb, AT_tfoot) || cur_is(tb, AT_thead) ||
                           cur_is(tb, AT_template) || cur_is(tb, AT_html))) pop(tb);
}

static void clear_to_row_context(HtmlTreeBuilder *tb) {
    while (tb->open.n && !(cur_is(tb, AT_tr) || cur_is(tb, AT_template) || cur_is(tb, AT_html))) pop(tb);
}

static void table_anything_else(HtmlTreeBuilder *tb, HtmlToken *t) {
    perr(tb, t);
    bool was = tb->foster;
    tb->foster = true;
    m_in_body(tb, t);
    tb->foster = was;
}

static void m_in_table(HtmlTreeBuilder *tb, HtmlToken *t) {
    WebNode *c = cur(tb);
    if (t->type == HT_CHARS && c && c->ns == WEB_NS_HTML &&
        (c->name == AT_table || c->name == AT_tbody || c->name == AT_template || c->name == AT_tfoot ||
         c->name == AT_thead || c->name == AT_tr)) {
        buf_clear(&tb->pending);
        tb->pending_nonspace = false;
        tb->orig_mode = tb->mode;
        tb->mode = IM_IN_TABLE_TEXT;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (t->type == HT_START) {
        switch (t->name) {
        case AT_caption:
            clear_to_table_context(tb);
            fmt_marker(tb);
            insert_html(tb, t);
            tb->mode = IM_IN_CAPTION;
            return;
        case AT_colgroup:
            clear_to_table_context(tb);
            insert_html(tb, t);
            tb->mode = IM_IN_COLUMN_GROUP;
            return;
        case AT_col:
            clear_to_table_context(tb);
            insert_html_name(tb, AT_colgroup);
            tb->mode = IM_IN_COLUMN_GROUP;
            mode_dispatch(tb, t);
            return;
        case AT_tbody: case AT_tfoot: case AT_thead:
            clear_to_table_context(tb);
            insert_html(tb, t);
            tb->mode = IM_IN_TABLE_BODY;
            return;
        case AT_td: case AT_th: case AT_tr:
            clear_to_table_context(tb);
            insert_html_name(tb, AT_tbody);
            tb->mode = IM_IN_TABLE_BODY;
            mode_dispatch(tb, t);
            return;
        case AT_table:
            perr(tb, t);
            if (!has_in_scope(tb, AT_table, SC_TABLE)) return;
            pop_until(tb, AT_table);
            reset_mode(tb);
            mode_dispatch(tb, t);
            return;
        case AT_style: case AT_script: case AT_template:
            m_in_head(tb, t);
            return;
        case AT_input: {
            uint32_t tl;
            const char *type = tok_attr(t, AT_type, &tl);
            if (!type || !(tl == 6 && strncasecmp(type, "hidden", 6) == 0)) break;
            perr(tb, t);
            insert_html(tb, t);
            pop(tb);
            t->ack = true;
            return;
        }
        case AT_form: {
            perr(tb, t);
            bool in_template = parsing_template(tb);
            if (tb->form && !in_template) return;
            WebNode *f = insert_html(tb, t);
            if (!in_template) tb->form = f;
            pop(tb);
            return;
        }
        default:
            break;
        }
    } else if (t->type == HT_END) {
        switch (t->name) {
        case AT_table:
            if (!has_in_scope(tb, AT_table, SC_TABLE)) { perr(tb, t); return; }
            pop_until(tb, AT_table);
            reset_mode(tb);
            return;
        case AT_body: case AT_caption: case AT_col: case AT_colgroup: case AT_html: case AT_tbody: case AT_td:
        case AT_tfoot: case AT_th: case AT_thead: case AT_tr:
            perr(tb, t);
            return;
        case AT_template:
            m_in_head(tb, t);
            return;
        default:
            break;
        }
    } else if (t->type == HT_EOF) {
        m_in_body(tb, t);
        return;
    }
    table_anything_else(tb, t);
}

static void m_in_table_text(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS) {
        for (uint32_t i = 0; i < t->len; i++) {
            char ch = t->data[i];
            if (ch == 0) { perr(tb, t); continue; }
            buf_putc(&tb->pending, ch);
            if (!(ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r')) tb->pending_nonspace = true;
        }
        return;
    }
    if (tb->pending.n) {
        if (tb->pending_nonspace) {
            HtmlToken ct;
            memset(&ct, 0, sizeof ct);
            ct.type = HT_CHARS;
            ct.data = tb->pending.p;
            ct.len = tb->pending.n;
            table_anything_else(tb, &ct);
        } else {
            insert_text(tb, tb->pending.p, tb->pending.n);
        }
        buf_clear(&tb->pending);
    }
    tb->pending_nonspace = false;
    tb->mode = tb->orig_mode;
    mode_dispatch(tb, t);
}

static bool caption_close(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (!has_in_scope(tb, AT_caption, SC_TABLE)) { perr(tb, t); return false; }
    gen_implied(tb, AT_NONE);
    if (!cur_is(tb, AT_caption)) perr(tb, t);
    pop_until(tb, AT_caption);
    fmt_clear_to_marker(tb);
    tb->mode = IM_IN_TABLE;
    return true;
}

static void m_in_caption(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_end(t, AT_caption)) { caption_close(tb, t); return; }
    if ((t->type == HT_START && (t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                                 t->name == AT_tbody || t->name == AT_td || t->name == AT_tfoot || t->name == AT_th ||
                                 t->name == AT_thead || t->name == AT_tr)) || is_end(t, AT_table)) {
        if (caption_close(tb, t)) mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_body || t->name == AT_col || t->name == AT_colgroup || t->name == AT_html ||
                              t->name == AT_tbody || t->name == AT_td || t->name == AT_tfoot || t->name == AT_th ||
                              t->name == AT_thead || t->name == AT_tr)) {
        perr(tb, t);
        return;
    }
    m_in_body(tb, t);
}

static void m_in_column_group(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_ws_token(t)) { insert_text(tb, t->data, t->len); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_start(t, AT_col)) { insert_html(tb, t); pop(tb); t->ack = true; return; }
    if (is_end(t, AT_colgroup)) {
        if (!cur_is(tb, AT_colgroup)) { perr(tb, t); return; }
        pop(tb);
        tb->mode = IM_IN_TABLE;
        return;
    }
    if (is_end(t, AT_col)) { perr(tb, t); return; }
    if (is_start(t, AT_template) || is_end(t, AT_template)) { m_in_head(tb, t); return; }
    if (t->type == HT_EOF) { m_in_body(tb, t); return; }
    if (!cur_is(tb, AT_colgroup)) { perr(tb, t); return; }
    pop(tb);
    tb->mode = IM_IN_TABLE;
    mode_dispatch(tb, t);
}

static void m_in_table_body(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_start(t, AT_tr)) {
        clear_to_tbody_context(tb);
        insert_html(tb, t);
        tb->mode = IM_IN_ROW;
        return;
    }
    if (is_start(t, AT_th) || is_start(t, AT_td)) {
        perr(tb, t);
        clear_to_tbody_context(tb);
        insert_html_name(tb, AT_tr);
        tb->mode = IM_IN_ROW;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_tbody || t->name == AT_tfoot || t->name == AT_thead)) {
        if (!has_in_scope(tb, t->name, SC_TABLE)) { perr(tb, t); return; }
        clear_to_tbody_context(tb);
        pop(tb);
        tb->mode = IM_IN_TABLE;
        return;
    }
    if ((t->type == HT_START && (t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                                 t->name == AT_tbody || t->name == AT_tfoot || t->name == AT_thead)) || is_end(t, AT_table)) {
        if (!has_in_scope(tb, AT_tbody, SC_TABLE) && !has_in_scope(tb, AT_thead, SC_TABLE) &&
            !has_in_scope(tb, AT_tfoot, SC_TABLE)) { perr(tb, t); return; }
        clear_to_tbody_context(tb);
        pop(tb);
        tb->mode = IM_IN_TABLE;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_body || t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                              t->name == AT_html || t->name == AT_td || t->name == AT_th || t->name == AT_tr)) {
        perr(tb, t);
        return;
    }
    m_in_table(tb, t);
}

static void m_in_row(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_start(t, AT_th) || is_start(t, AT_td)) {
        clear_to_row_context(tb);
        insert_html(tb, t);
        tb->mode = IM_IN_CELL;
        fmt_marker(tb);
        return;
    }
    if (is_end(t, AT_tr)) {
        if (!has_in_scope(tb, AT_tr, SC_TABLE)) { perr(tb, t); return; }
        clear_to_row_context(tb);
        pop(tb);
        tb->mode = IM_IN_TABLE_BODY;
        return;
    }
    if ((t->type == HT_START && (t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                                 t->name == AT_tbody || t->name == AT_tfoot || t->name == AT_thead || t->name == AT_tr)) ||
        is_end(t, AT_table)) {
        if (!has_in_scope(tb, AT_tr, SC_TABLE)) { perr(tb, t); return; }
        clear_to_row_context(tb);
        pop(tb);
        tb->mode = IM_IN_TABLE_BODY;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_tbody || t->name == AT_tfoot || t->name == AT_thead)) {
        if (!has_in_scope(tb, t->name, SC_TABLE)) { perr(tb, t); return; }
        if (!has_in_scope(tb, AT_tr, SC_TABLE)) return;
        clear_to_row_context(tb);
        pop(tb);
        tb->mode = IM_IN_TABLE_BODY;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_body || t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                              t->name == AT_html || t->name == AT_td || t->name == AT_th)) {
        perr(tb, t);
        return;
    }
    m_in_table(tb, t);
}

static void close_cell(HtmlTreeBuilder *tb, HtmlToken *t) {
    gen_implied(tb, AT_NONE);
    if (!cur_is(tb, AT_td) && !cur_is(tb, AT_th)) perr(tb, t);
    while (tb->open.n) {
        WebNode *n = cur(tb);
        pop(tb);
        if (is_html(n, AT_td) || is_html(n, AT_th)) break;
    }
    fmt_clear_to_marker(tb);
    tb->mode = IM_IN_ROW;
}

static void m_in_cell(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_END && (t->name == AT_td || t->name == AT_th)) {
        if (!has_in_scope(tb, t->name, SC_TABLE)) { perr(tb, t); return; }
        gen_implied(tb, AT_NONE);
        if (!cur_is(tb, t->name)) perr(tb, t);
        pop_until(tb, t->name);
        fmt_clear_to_marker(tb);
        tb->mode = IM_IN_ROW;
        return;
    }
    if (t->type == HT_START && (t->name == AT_caption || t->name == AT_col || t->name == AT_colgroup ||
                                t->name == AT_tbody || t->name == AT_td || t->name == AT_tfoot || t->name == AT_th ||
                                t->name == AT_thead || t->name == AT_tr)) {
        if (!has_in_scope(tb, AT_td, SC_TABLE) && !has_in_scope(tb, AT_th, SC_TABLE)) { perr(tb, t); return; }
        close_cell(tb, t);
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_body || t->name == AT_caption || t->name == AT_col ||
                              t->name == AT_colgroup || t->name == AT_html)) {
        perr(tb, t);
        return;
    }
    if (t->type == HT_END && (t->name == AT_table || t->name == AT_tbody || t->name == AT_tfoot ||
                              t->name == AT_thead || t->name == AT_tr)) {
        if (!has_in_scope(tb, t->name, SC_TABLE)) { perr(tb, t); return; }
        close_cell(tb, t);
        mode_dispatch(tb, t);
        return;
    }
    m_in_body(tb, t);
}

static void m_in_template(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS || t->type == HT_COMMENT || t->type == HT_PI || t->type == HT_DOCTYPE) { m_in_body(tb, t); return; }
    if (t->type == HT_START) {
        InsertionMode nm;
        switch (t->name) {
        case AT_base: case AT_basefont: case AT_bgsound: case AT_link: case AT_meta: case AT_noframes: case AT_script:
        case AT_style: case AT_template: case AT_title:
            m_in_head(tb, t);
            return;
        case AT_caption: case AT_colgroup: case AT_tbody: case AT_tfoot: case AT_thead: nm = IM_IN_TABLE; break;
        case AT_col: nm = IM_IN_COLUMN_GROUP; break;
        case AT_tr: nm = IM_IN_TABLE_BODY; break;
        case AT_td: case AT_th: nm = IM_IN_ROW; break;
        default: nm = IM_IN_BODY; break;
        }
        if (tb->tmpl_modes.n) tb->tmpl_modes.n--;
        vec_push(tb->a, tb->tmpl_modes, nm);
        tb->mode = nm;
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_END) {
        if (t->name == AT_template) { m_in_head(tb, t); return; }
        perr(tb, t);
        return;
    }
    if (t->type == HT_EOF) {
        if (!stack_has(tb, AT_template)) { tb->done = true; return; }
        perr(tb, t);
        pop_until(tb, AT_template);
        fmt_clear_to_marker(tb);
        if (tb->tmpl_modes.n) tb->tmpl_modes.n--;
        reset_mode(tb);
        mode_dispatch(tb, t);
    }
}

static void m_after_body(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (is_ws_token(t)) { m_in_body(tb, t); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, tb->open.n ? tb->open.v[0] : tb->doc->root); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, tb->open.n ? tb->open.v[0] : tb->doc->root); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_end(t, AT_html)) {
        if (tb->fragment) { perr(tb, t); return; }
        tb->mode = IM_AFTER_AFTER_BODY;
        return;
    }
    if (t->type == HT_EOF) { tb->done = true; return; }
    perr(tb, t);
    tb->mode = IM_IN_BODY;
    mode_dispatch(tb, t);
}

static void frameset_ws(HtmlTreeBuilder *tb, HtmlToken *t) {
    /* only the white space of a mixed run is inserted; the rest is a parse error, ignored */
    size_t k = 0;
    char tmp[256];
    for (uint32_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') {
            tmp[k++] = c;
            if (k == sizeof tmp) { insert_text(tb, tmp, k); k = 0; }
        } else perr(tb, t);
    }
    insert_text(tb, tmp, k);
}

static void m_in_frameset(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS) { frameset_ws(tb, t); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_start(t, AT_frameset)) { insert_html(tb, t); return; }
    if (is_end(t, AT_frameset)) {
        if (tb->open.n == 1) { perr(tb, t); return; }
        pop(tb);
        if (!tb->fragment && !cur_is(tb, AT_frameset)) tb->mode = IM_AFTER_FRAMESET;
        return;
    }
    if (is_start(t, AT_frame)) { insert_html(tb, t); pop(tb); t->ack = true; return; }
    if (is_start(t, AT_noframes)) { m_in_head(tb, t); return; }
    if (t->type == HT_EOF) {
        if (tb->open.n != 1) perr(tb, t);
        tb->done = true;
        return;
    }
    perr(tb, t);
}

static void m_after_frameset(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS) { frameset_ws(tb, t); return; }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    if (is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (is_end(t, AT_html)) { tb->mode = IM_AFTER_AFTER_FRAMESET; return; }
    if (is_start(t, AT_noframes)) { m_in_head(tb, t); return; }
    if (t->type == HT_EOF) { tb->done = true; return; }
    perr(tb, t);
}

static void m_after_after_body(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_COMMENT) { insert_comment(tb, t, tb->doc->root); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, tb->doc->root); return; }
    if (t->type == HT_DOCTYPE || is_ws_token(t) || is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (t->type == HT_EOF) { tb->done = true; return; }
    perr(tb, t);
    tb->mode = IM_IN_BODY;
    mode_dispatch(tb, t);
}

static void m_after_after_frameset(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_COMMENT) { insert_comment(tb, t, tb->doc->root); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, tb->doc->root); return; }
    if (t->type == HT_DOCTYPE || is_start(t, AT_html)) { m_in_body(tb, t); return; }
    if (t->type == HT_CHARS) { frameset_ws(tb, t); return; }
    if (t->type == HT_EOF) { tb->done = true; return; }
    if (is_start(t, AT_noframes)) { m_in_head(tb, t); return; }
    perr(tb, t);
}

static void mode_dispatch(HtmlTreeBuilder *tb, HtmlToken *t) {
    switch (tb->mode) {
    case IM_INITIAL: m_initial(tb, t); break;
    case IM_BEFORE_HTML: m_before_html(tb, t); break;
    case IM_BEFORE_HEAD: m_before_head(tb, t); break;
    case IM_IN_HEAD: m_in_head(tb, t); break;
    case IM_IN_HEAD_NOSCRIPT: m_in_head_noscript(tb, t); break;
    case IM_AFTER_HEAD: m_after_head(tb, t); break;
    case IM_IN_BODY: m_in_body(tb, t); break;
    case IM_TEXT: m_text(tb, t); break;
    case IM_IN_TABLE: m_in_table(tb, t); break;
    case IM_IN_TABLE_TEXT: m_in_table_text(tb, t); break;
    case IM_IN_CAPTION: m_in_caption(tb, t); break;
    case IM_IN_COLUMN_GROUP: m_in_column_group(tb, t); break;
    case IM_IN_TABLE_BODY: m_in_table_body(tb, t); break;
    case IM_IN_ROW: m_in_row(tb, t); break;
    case IM_IN_CELL: m_in_cell(tb, t); break;
    case IM_IN_TEMPLATE: m_in_template(tb, t); break;
    case IM_AFTER_BODY: m_after_body(tb, t); break;
    case IM_IN_FRAMESET: m_in_frameset(tb, t); break;
    case IM_AFTER_FRAMESET: m_after_frameset(tb, t); break;
    case IM_AFTER_AFTER_BODY: m_after_after_body(tb, t); break;
    case IM_AFTER_AFTER_FRAMESET: m_after_after_frameset(tb, t); break;
    }
}

/* ================================================================================================ foreign content */

static void foreign(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (t->type == HT_CHARS) {
        size_t i = 0;
        while (i < t->len) {
            size_t s = i;
            while (i < t->len && t->data[i] != 0) i++;
            if (i > s) {
                insert_text(tb, t->data + s, i - s);
                if (!all_ws(t->data + s, i - s)) tb->frameset_ok = false;
            }
            while (i < t->len && t->data[i] == 0) {            /* U+FFFD in its place, frameset-ok untouched */
                perr(tb, t);
                insert_text(tb, "\xEF\xBF\xBD", 3);
                i++;
            }
        }
        return;
    }
    if (t->type == HT_COMMENT) { insert_comment(tb, t, NULL); return; }
    if (t->type == HT_PI) { insert_pi(tb, t, NULL); return; }
    if (t->type == HT_DOCTYPE) { perr(tb, t); return; }
    bool breakout = false;
    if (t->type == HT_START) {
        switch (t->name) {
        case AT_b: case AT_big: case AT_blockquote: case AT_body: case AT_br: case AT_center: case AT_code: case AT_dd:
        case AT_div: case AT_dl: case AT_dt: case AT_em: case AT_embed: case AT_h1: case AT_h2: case AT_h3: case AT_h4:
        case AT_h5: case AT_h6: case AT_head: case AT_hr: case AT_i: case AT_img: case AT_li: case AT_listing:
        case AT_menu: case AT_meta: case AT_nobr: case AT_ol: case AT_p: case AT_pre: case AT_ruby: case AT_s:
        case AT_small: case AT_span: case AT_strong: case AT_strike: case AT_sub: case AT_sup: case AT_table: case AT_tt:
        case AT_u: case AT_ul: case AT_var:
            breakout = true;
            break;
        case AT_font:
            breakout = tok_attr(t, AT_color, NULL) || tok_attr(t, AT_face, NULL) || tok_attr(t, AT_size, NULL);
            break;
        default:
            break;
        }
    } else if (t->type == HT_END && (t->name == AT_br || t->name == AT_p)) {
        breakout = true;
    }
    if (breakout) {
        perr(tb, t);
        while (tb->open.n > 1) {
            WebNode *c = cur(tb);
            if (is_mathml_text_ip(c) || is_html_ip(c) || c->ns == WEB_NS_HTML) break;
            pop(tb);
        }
        mode_dispatch(tb, t);
        return;
    }
    if (t->type == HT_START) {
        WebNode *ac = adj_cur(tb);
        int ns = ac->ns;
        WebAtom name = t->name;
        int adj = ADJ_FOREIGN;
        if (ns == WEB_NS_MATHML) adj |= ADJ_MATHML;
        if (ns == WEB_NS_SVG) {
            name = adjust_name(tb, name, svg_tags, sizeof svg_tags / sizeof svg_tags[0]);
            adj |= ADJ_SVG;
        }
        insert_element(tb, t, ns, name, adj);
        if (t->self_closing) {
            t->ack = true;
            if (t->name == AT_script && cur(tb) && cur(tb)->ns == WEB_NS_SVG) pop(tb);   /* SVG scripts are not run */
            else pop(tb);
        }
        return;
    }
    if (t->type == HT_END) {
        WebNode *c = cur(tb);
        if (t->name == AT_script && c && c->ns == WEB_NS_SVG && c->name == AT_script) {
            pop(tb);
            return;
        }
        int i = (int)tb->open.n - 1;
        WebNode *node = tb->open.v[i];
        const char *tn = atom_str(&tb->doc->atoms, t->name);
        if (strcasecmp(atom_str(&tb->doc->atoms, node->name), tn) != 0) perr(tb, t);
        for (;;) {
            if (i == 0) return;
            if (strcasecmp(atom_str(&tb->doc->atoms, node->name), tn) == 0) {
                pop_until_node(tb, node);
                return;
            }
            i--;
            node = tb->open.v[i];
            if (node->ns != WEB_NS_HTML) continue;
            mode_dispatch(tb, t);
            return;
        }
    }
    mode_dispatch(tb, t);
}

/* ================================================================================================ the dispatcher */

static bool use_html_rules(HtmlTreeBuilder *tb, const HtmlToken *t) {
    WebNode *ac = adj_cur(tb);
    if (!ac || ac->ns == WEB_NS_HTML) return true;
    if (t->type == HT_EOF) return true;
    if (is_mathml_text_ip(ac)) {
        if (t->type == HT_START && t->name != AT_mglyph && t->name != AT_malignmark) return true;
        if (t->type == HT_CHARS) return true;
    }
    if (ac->ns == WEB_NS_MATHML && ac->name == AT_annotation_xml && t->type == HT_START && t->name == AT_svg) return true;
    if (is_html_ip(ac) && (t->type == HT_START || t->type == HT_CHARS)) return true;
    return false;
}

static void process(HtmlTreeBuilder *tb, HtmlToken *t) {
    if (use_html_rules(tb, t)) mode_dispatch(tb, t);
    else foreign(tb, t);
}

/* Character runs: white space, other characters and NULs are split where the mode needs it (see the header comment). */
static size_t piece_len(const char *p, size_t n) {
    if (p[0] == 0) return 1;
    bool ws = p[0] == ' ' || p[0] == '\t' || p[0] == '\n' || p[0] == '\f' || p[0] == '\r';
    size_t i = 1;
    while (i < n) {
        char c = p[i];
        if (c == 0) break;
        bool w = c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
        if (w != ws) break;
        i++;
    }
    return i;
}

void tb_token(void *user, HtmlToken *t) {
    HtmlTreeBuilder *tb = user;
    if (tb->done || tb->doc->status == WEB_E_NOMEM || tb->doc->budget.exceeded) {
        if (tb->doc->budget.exceeded) tb->doc->status = WEB_E_NOMEM;
        tb->tok->stop = true;
        return;
    }
    if (tb->ignore_lf) {
        tb->ignore_lf = false;
        if (t->type == HT_CHARS && t->len && t->data[0] == '\n') {
            if (t->len == 1) return;
            HtmlToken rest = *t;
            rest.data++;
            rest.len--;
            tb_token(user, &rest);
            return;
        }
    }
    if (t->type != HT_CHARS) {
        process(tb, t);
        return;
    }
    const char *p = t->data;
    size_t n = t->len;
    while (n && !tb->done) {
        HtmlToken k = *t;
        k.data = p;
        k.len = (uint32_t)n;
        if (!use_html_rules(tb, &k)) { foreign(tb, &k); return; }
        if (tb->mode == IM_IN_BODY || tb->mode == IM_TEXT || tb->mode == IM_IN_CELL || tb->mode == IM_IN_CAPTION) {
            mode_dispatch(tb, &k);                     /* these modes insert every character alike: no need to split */
            return;
        }
        size_t m = piece_len(p, n);
        k.len = (uint32_t)m;
        mode_dispatch(tb, &k);
        p += m;
        n -= m;
    }
}

bool tb_cdata_allowed(void *user) {
    HtmlTreeBuilder *tb = user;
    WebNode *ac = adj_cur(tb);
    return ac && ac->ns != WEB_NS_HTML;
}

void tb_init(HtmlTreeBuilder *tb, WebDoc *d, HtmlTokenizer *t, WebArena *a) {
    memset(tb, 0, sizeof *tb);
    tb->doc = d;
    tb->tok = t;
    tb->a = a;
    tb->mode = IM_INITIAL;
    tb->frameset_ok = true;
    tb->scripting = d->scripting;
    tb->max_depth = d->cfg.max_depth > 8 ? d->cfg.max_depth : 512;
    buf_init(&tb->pending, a);
}

/* The fragment case (innerHTML, html5lib "#document-fragment" tests): section 13.4 steps 4 to 13. */
void tb_init_fragment(HtmlTreeBuilder *tb, WebNode *context) {
    WebDoc *d = tb->doc;
    tb->fragment = true;
    tb->context = context;
    if (context->ns == WEB_NS_HTML) {
        switch (context->name) {
        case AT_title: case AT_textarea: tb->tok->state = TS_RCDATA; break;
        case AT_style: case AT_xmp: case AT_iframe: case AT_noembed: case AT_noframes: tb->tok->state = TS_RAWTEXT; break;
        case AT_script: tb->tok->state = TS_SCRIPT_DATA; break;
        case AT_noscript: if (tb->scripting) tb->tok->state = TS_RAWTEXT; break;
        case AT_plaintext: tb->tok->state = TS_PLAINTEXT; break;
        default: break;
        }
    }
    WebNode *root = dom_new_element(d, WEB_NS_HTML, AT_html);
    tb->fragment_root = root;
    push(tb, root);
    if (is_html(context, AT_template)) vec_push(tb->a, tb->tmpl_modes, IM_IN_TEMPLATE);
    reset_mode(tb);
    for (WebNode *n = context; n; n = n->parent)
        if (is_html(n, AT_form)) { tb->form = n; break; }
}

void tb_finish(HtmlTreeBuilder *tb) {
    while (tb->open.n) pop(tb);                              /* "stop parsing": pop all the nodes */
    tb->done = true;
}
