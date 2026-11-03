/*
 * Form controls: the state a user changes (the text typed into an input or textarea, checkboxes and radio buttons,
 * the selected option), the HTML rules that tie it to the markup (the value attribute is the default until the user
 * types; "checked" is the default checkedness; radio groups; the selectedness of options), activation (what a click does:
 * links, submit buttons, labels, <details>), and building a form submission (application/x-www-form-urlencoded, HTML
 * 4.10.21 "constructing the entry list"; files and multipart/form-data are not supported).
 */
#include "css.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct WebControl {
    char *value;                    /* NULL: the default value (the attribute, or the textarea's text) */
    uint32_t vlen, vcap;
    bool checked, checked_set;      /* checked_set: the user (or a script) changed it: the attribute no longer decides */
    int selected;                   /* <select>: the selected option's index, -1 = by the markup */
    bool selected_set;
} WebControl;

static WebControl *ctl(WebNode *el) {
    if (!el->u.el.ctl) {
        el->u.el.ctl = wa_alloc(&el->doc->arena, sizeof(WebControl));
        if (el->u.el.ctl) el->u.el.ctl->selected = -1;
    }
    return el->u.el.ctl;
}

static const char *input_type(const WebNode *el) {
    const char *t = dom_attr_val(el, AT_type);
    if (!t) return "text";
    static const char *const known[] = { "hidden", "text", "search", "tel", "url", "email", "password", "date", "month",
        "week", "time", "datetime-local", "number", "range", "color", "checkbox", "radio", "file", "submit", "image",
        "reset", "button" };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strcasecmp(t, known[i]) == 0) return known[i];
    return "text";
}

static bool is_checkable(const WebNode *el) {
    if (!is_html(el, AT_input)) return false;
    const char *t = input_type(el);
    return !strcmp(t, "checkbox") || !strcmp(t, "radio");
}

/* ------------------------------------------------------------------------------------------------ options */

static WebNode *option_select(const WebNode *opt) {
    for (WebNode *p = opt->parent; p; p = p->parent) {
        if (is_html(p, AT_select)) return p;
        if (!is_html(p, AT_optgroup) && p->type == WEB_NODE_ELEMENT && !is_html(p, AT_div) && !is_html(p, AT_hr)) return NULL;
    }
    return NULL;
}

static int collect_options(WebNode *sel, WebNode **out, int max) {
    int n = 0;
    for (WebNode *k = sel->first; k; k = dom_next(k, sel)) {
        if (is_html(k, AT_option)) { if (out && n < max) out[n] = k; n++; }
        if (is_html(k, AT_select) && k != sel) k = (WebNode *)dom_next_skip(k, sel);
        if (!k) break;
    }
    return n;
}

/* the selectedness of the options (HTML 4.10.7 "selectedness setting algorithm", single-select display) */
static int selected_index(WebNode *sel) {
    WebControl *c = sel->u.el.ctl;
    WebNode *opts[1024];
    int n = collect_options(sel, opts, 1024);
    if (n > 1024) n = 1024;
    if (c && c->selected_set) return c->selected < n ? c->selected : -1;
    int last = -1;
    for (int i = 0; i < n; i++) if (dom_attr(opts[i], AT_selected)) last = i;
    if (last >= 0) return last;
    if (dom_attr(sel, AT_multiple)) return -1;
    for (int i = 0; i < n; i++) if (!dom_attr(opts[i], AT_disabled)) return i;
    return -1;
}

/* ------------------------------------------------------------------------------------------------ the state */

bool control_checked(const WebNode *cel) {
    WebNode *el = (WebNode *)cel;
    if (is_html(el, AT_option)) {
        WebNode *sel = option_select(el);
        if (!sel) return dom_attr(el, AT_selected) != NULL;
        WebNode *opts[1024];
        int n = collect_options(sel, opts, 1024);
        int idx = selected_index(sel);
        if (dom_attr(sel, AT_multiple)) return el->u.el.ctl && el->u.el.ctl->checked_set ? el->u.el.ctl->checked : dom_attr(el, AT_selected) != NULL;
        return idx >= 0 && idx < n && idx < 1024 && opts[idx] == el;
    }
    if (!is_checkable(el)) return false;
    if (el->u.el.ctl && el->u.el.ctl->checked_set) return el->u.el.ctl->checked;
    return dom_attr(el, AT_checked) != NULL;
}

bool control_value_empty(const WebNode *el) {
    const char *v = web_control_value((WebNode *)el);
    return !v || !*v;
}

const char *web_control_value(WebNode *el) {
    if (!is_el(el)) return NULL;
    WebControl *c = el->u.el.ctl;
    if (c && c->value) return c->value;
    if (is_html(el, AT_textarea)) {
        WebBuf b;
        buf_init(&b, &el->doc->arena);
        dom_text_of(el, &b);
        WebControl *cc = ctl(el);
        if (!cc) { buf_free(&b); return ""; }
        cc->value = wa_strndup(&el->doc->arena, b.p ? b.p : "", b.n);
        cc->vlen = b.n;
        cc->vcap = b.n + 1;
        buf_free(&b);
        return cc->value ? cc->value : "";
    }
    if (is_html(el, AT_select)) {
        int i = selected_index(el);
        WebNode *opts[1024];
        int n = collect_options(el, opts, 1024);
        if (i < 0 || i >= n || i >= 1024) return "";
        const char *v = dom_attr_val(opts[i], AT_value);
        if (v) return v;
        size_t len;
        char *t = web_node_text_content(opts[i], &len);
        WebControl *cc = ctl(el);
        if (cc && t) {
            cc->value = NULL;
            /* the option's text: cached in the arena (it lives as long as the page) */
            char *copy = wa_strndup(&el->doc->arena, t, len);
            web_free(el->doc, t);
            return copy ? copy : "";
        }
        if (t) web_free(el->doc, t);
        return "";
    }
    const char *v = dom_attr_val(el, AT_value);
    if (!v && is_checkable(el)) return "on";
    return v ? v : "";
}

int web_control_set_value(WebNode *el, const char *s) {
    if (!is_el(el)) return WEB_E_ARG;
    WebControl *c = ctl(el);
    if (!c) return WEB_E_NOMEM;
    size_t n = strlen(s);
    if (c->value) wa_free(&el->doc->arena, c->value, c->vcap);
    c->value = wa_strndup(&el->doc->arena, s, n);
    c->vlen = (uint32_t)n;
    c->vcap = (uint32_t)n + 1;
    dom_mark_style_dirty(el);                                /* :placeholder-shown, and the control's look */
    return c->value ? WEB_OK : WEB_E_NOMEM;
}

bool web_control_checked(WebNode *el) { return control_checked(el); }

static WebNode *form_of(WebNode *el) {
    if (el->u.el.form) return el->u.el.form;
    const char *fid = dom_attr_val(el, AT_form);
    if (fid) {
        WebNode *f = web_doc_get_element_by_id(el->doc, fid);
        return is_html(f, AT_form) ? f : NULL;
    }
    for (WebNode *p = el->parent; p; p = p->parent)
        if (is_html(p, AT_form)) return p;
    return NULL;
}

int web_control_set_checked(WebNode *el, bool on) {
    if (is_html(el, AT_option)) {
        WebNode *sel = option_select(el);
        if (!sel) return WEB_E_ARG;
        if (dom_attr(sel, AT_multiple)) {
            WebControl *c = ctl(el);
            if (!c) return WEB_E_NOMEM;
            c->checked = on;
            c->checked_set = true;
        } else if (on) {
            WebNode *opts[1024];
            int n = collect_options(sel, opts, 1024);
            for (int i = 0; i < n && i < 1024; i++) if (opts[i] == el) web_select_set_index(sel, i);
        }
        dom_mark_style_dirty(sel);
        return WEB_OK;
    }
    if (!is_checkable(el)) return WEB_E_ARG;
    WebControl *c = ctl(el);
    if (!c) return WEB_E_NOMEM;
    c->checked = on;
    c->checked_set = true;
    if (on && !strcmp(input_type(el), "radio")) {
        /* the other radio buttons of the group (same name, same form owner) are unchecked */
        const char *name = dom_attr_val(el, AT_name);
        WebNode *form = form_of(el);
        if (name && *name) {
            WebNode *root = form ? form : el->doc->root;
            for (WebNode *n = root; n; n = dom_next(n, root)) {
                if (n == el || !is_html(n, AT_input) || strcmp(input_type(n), "radio")) continue;
                const char *nn = dom_attr_val(n, AT_name);
                if (!nn || strcmp(nn, name) || form_of(n) != form) continue;
                WebControl *oc = ctl(n);
                if (oc) { oc->checked = false; oc->checked_set = true; dom_mark_style_dirty(n); }
            }
        }
    }
    dom_mark_style_dirty(el);
    return WEB_OK;
}

int web_select_index(WebNode *sel) { return is_html(sel, AT_select) ? selected_index(sel) : -1; }

int web_select_set_index(WebNode *sel, int index) {
    if (!is_html(sel, AT_select)) return WEB_E_ARG;
    WebControl *c = ctl(sel);
    if (!c) return WEB_E_NOMEM;
    c->selected = index;
    c->selected_set = true;
    c->value = NULL;
    dom_mark_style_dirty(sel);
    return WEB_OK;
}

int web_select_options(WebNode *sel, const char **labels, int max) {
    if (!is_html(sel, AT_select)) return 0;
    WebNode *opts[1024];
    int n = collect_options(sel, opts, 1024);
    if (n > 1024) n = 1024;
    for (int i = 0; i < n && i < max; i++) {
        const char *l = dom_attr_val(opts[i], AT_label);
        if (!l) {
            size_t len;
            char *t = web_node_text_content(opts[i], &len);
            /* strip and collapse white space, like the option's label */
            char *o = wa_alloc(&sel->doc->arena, len + 1);
            size_t k = 0;
            bool sp = false;
            for (size_t j = 0; t && o && j < len; j++) {
                char ch = t[j];
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') { sp = k > 0; continue; }
                if (sp) o[k++] = ' ';
                sp = false;
                o[k++] = ch;
            }
            if (o) o[k] = 0;
            if (t) web_free(sel->doc, t);
            l = o ? o : "";
        }
        labels[i] = l;
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------ submission */

static void urlencode(WebBuf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_')
            buf_putc(b, (char)c);
        else if (c == ' ') buf_putc(b, '+');
        else if (c == '\n') buf_puts(b, "%0D%0A");           /* newlines are normalized to CR LF */
        else { buf_putc(b, '%'); buf_putc(b, hex[c >> 4]); buf_putc(b, hex[c & 15]); }
    }
}

static void entry(WebBuf *b, const char *name, const char *value) {
    if (b->n) buf_putc(b, '&');
    urlencode(b, name);
    buf_putc(b, '=');
    urlencode(b, value);
}

static bool is_disabled_control(WebNode *el) {
    if (dom_attr(el, AT_disabled)) return true;
    for (WebNode *p = el->parent; p; p = p->parent)
        if (is_html(p, AT_fieldset) && dom_attr(p, AT_disabled)) return true;
    return false;
}

/* "constructing the entry list" for application/x-www-form-urlencoded */
static void entries(WebNode *form, WebNode *submitter, WebBuf *b) {
    for (WebNode *n = form->doc->root; n; n = dom_next(n, form->doc->root)) {
        if (n->type != WEB_NODE_ELEMENT || n->ns != WEB_NS_HTML) continue;
        if (n->name != AT_input && n->name != AT_select && n->name != AT_textarea && n->name != AT_button) continue;
        if (form_of(n) != form || is_disabled_control(n)) continue;
        const char *name = dom_attr_val(n, AT_name);
        if (n->name == AT_button) {
            if (n != submitter || !name || !*name) continue;
            entry(b, name, dom_attr_val(n, AT_value) ? dom_attr_val(n, AT_value) : "");
            continue;
        }
        if (n->name == AT_input) {
            const char *t = input_type(n);
            if (!strcmp(t, "image")) {
                if (n == submitter) {
                    char k[300];
                    snprintf(k, sizeof k, "%s%sx", name ? name : "", name && *name ? "." : "");
                    entry(b, k, "0");
                    snprintf(k, sizeof k, "%s%sy", name ? name : "", name && *name ? "." : "");
                    entry(b, k, "0");
                }
                continue;
            }
            if (!name || !*name) continue;
            if ((!strcmp(t, "submit") || !strcmp(t, "reset") || !strcmp(t, "button")) && n != submitter) continue;
            if (!strcmp(t, "reset") || !strcmp(t, "button") || !strcmp(t, "file")) continue;
            if ((!strcmp(t, "checkbox") || !strcmp(t, "radio")) && !control_checked(n)) continue;
            entry(b, name, web_control_value(n));
            continue;
        }
        if (!name || !*name) continue;
        if (n->name == AT_select) {
            WebNode *opts[1024];
            int cnt = collect_options(n, opts, 1024);
            for (int i = 0; i < cnt && i < 1024; i++) {
                if (!control_checked(opts[i]) || dom_attr(opts[i], AT_disabled)) continue;
                const char *v = dom_attr_val(opts[i], AT_value);
                if (v) { entry(b, name, v); continue; }
                size_t len;
                char *t = web_node_text_content(opts[i], &len);
                entry(b, name, t ? t : "");
                if (t) web_free(form->doc, t);
            }
            continue;
        }
        entry(b, name, web_control_value(n));                 /* textarea */
    }
}

static char *raw_copy(WebDoc *d, const char *s, size_t n) {
    char *r = web_raw_alloc(&d->budget, n + 1);
    if (r && n) memcpy(r, s, n);
    return r;
}

static WebAction submit(WebDoc *d, WebNode *form, WebNode *submitter) {
    WebAction a;
    memset(&a, 0, sizeof a);
    const char *action = submitter && dom_attr_val(submitter, AT_formaction) ? dom_attr_val(submitter, AT_formaction) : dom_attr_val(form, AT_action);
    const char *method = submitter && dom_attr_val(submitter, AT_formmethod) ? dom_attr_val(submitter, AT_formmethod) : dom_attr_val(form, AT_method);
    bool post = method && !strcasecmp(method, "post");
    if (method && !strcasecmp(method, "dialog")) return a;
    const char *url = doc_resolve_url(d, action && *action ? action : d->url, action && *action ? strlen(action) : strlen(d->url), &d->arena);
    if (!url) return a;
    WebBuf body;
    buf_init(&body, &d->arena);
    entries(form, submitter, &body);
    a.kind = WEB_ACT_SUBMIT;
    a.post = post;
    a.target = dom_attr_val(form, AT_target);
    a.node = form;
    if (post) {
        a.url = raw_copy(d, url, strlen(url));
        a.body = raw_copy(d, body.p ? body.p : "", body.n);
        a.body_len = body.n;
    } else {
        /* GET: the query of the action URL is replaced by the entries */
        WebBuf u;
        buf_init(&u, &d->arena);
        const char *q = strchr(url, '?'), *h = strchr(url, '#');
        size_t base = q ? (size_t)(q - url) : h ? (size_t)(h - url) : strlen(url);
        buf_put(&u, url, base);
        buf_putc(&u, '?');
        buf_put(&u, body.p ? body.p : "", body.n);
        a.url = raw_copy(d, u.p, u.n);
        buf_free(&u);
    }
    buf_free(&body);
    return a;
}

WebAction web_doc_activate(WebDoc *d, WebNode *node) {
    WebAction a;
    memset(&a, 0, sizeof a);
    for (WebNode *n = node; n; n = n->parent) {
        if (n->type != WEB_NODE_ELEMENT || n->ns != WEB_NS_HTML) continue;
        if ((n->name == AT_a || n->name == AT_area) && dom_attr(n, AT_href)) {
            const char *href = dom_attr_val(n, AT_href);
            const char *url = doc_resolve_url(d, href, strlen(href), &d->arena);
            if (!url) return a;
            a.kind = WEB_ACT_NAVIGATE;
            a.url = raw_copy(d, url, strlen(url));
            a.target = dom_attr_val(n, AT_target);
            a.node = n;
            return a;
        }
        if (n->name == AT_input) {
            const char *t = input_type(n);
            if (is_disabled_control(n)) return a;
            if (!strcmp(t, "checkbox")) { web_control_set_checked(n, !control_checked(n)); a.kind = WEB_ACT_TOGGLED; a.node = n; return a; }
            if (!strcmp(t, "radio")) { web_control_set_checked(n, true); a.kind = WEB_ACT_TOGGLED; a.node = n; return a; }
            if (!strcmp(t, "submit") || !strcmp(t, "image")) {
                WebNode *f = form_of(n);
                if (f) return submit(d, f, n);
                return a;
            }
            if (!strcmp(t, "reset") || !strcmp(t, "button") || !strcmp(t, "hidden")) return a;
            a.kind = WEB_ACT_FOCUS;
            a.node = n;
            return a;
        }
        if (n->name == AT_button) {
            if (is_disabled_control(n)) return a;
            const char *t = dom_attr_val(n, AT_type);
            if (!t || !strcasecmp(t, "submit")) {
                WebNode *f = form_of(n);
                if (f) return submit(d, f, n);
            }
            return a;
        }
        if (n->name == AT_textarea) { a.kind = WEB_ACT_FOCUS; a.node = n; return a; }
        if (n->name == AT_select) { a.kind = WEB_ACT_OPEN_SELECT; a.node = n; return a; }
        if (n->name == AT_label) {
            const char *f = dom_attr_val(n, AT_for);
            WebNode *target = NULL;
            if (f) target = web_doc_get_element_by_id(d, f);
            else for (WebNode *k = n->first; k; k = dom_next(k, n))
                if (is_html(k, AT_input) || is_html(k, AT_select) || is_html(k, AT_textarea) || is_html(k, AT_button)) { target = k; break; }
            if (target && target != node) return web_doc_activate(d, target);
            return a;
        }
        if (n->name == AT_summary && is_html(n->parent, AT_details)) {
            WebNode *det = n->parent;
            if (dom_attr(det, AT_open)) web_element_remove_attr(det, "open");
            else web_element_set_attr(det, "open", "");
            a.kind = WEB_ACT_TOGGLED;
            a.node = det;
            return a;
        }
    }
    return a;
}

/* a text input's implicit submission (Enter): the form's first submit button, or the form itself */
WebAction forms_implicit_submit(WebDoc *d, WebNode *field) {
    WebAction a;
    memset(&a, 0, sizeof a);
    WebNode *f = form_of(field);
    if (!f) return a;
    for (WebNode *n = f; n; n = dom_next(n, f))
        if ((is_html(n, AT_button) && (!dom_attr_val(n, AT_type) || !strcasecmp(dom_attr_val(n, AT_type), "submit"))) ||
            (is_html(n, AT_input) && (!strcmp(input_type(n), "submit") || !strcmp(input_type(n), "image"))))
            return submit(d, f, n);
    return submit(d, f, NULL);
}

const char *forms_input_type(const WebNode *el) { return input_type(el); }
