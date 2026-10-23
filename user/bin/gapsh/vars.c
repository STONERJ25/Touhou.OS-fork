/* Shell variables: a small table with export flags, plus a stack of local scopes for functions. */
#include "gapsh.h"

typedef struct Var {
    char *name;
    char *value;
    int exported;
    struct Var *next;
} Var;

static Var *g_vars;

typedef struct LocalSave {
    char *name;
    char *old_value;       /* NULL if the variable did not exist */
    int old_exported;
    struct LocalSave *next;
} LocalSave;

typedef struct Scope {
    LocalSave *saves;
    struct Scope *up;
} Scope;

static Scope *g_scope;

static Var *find(const char *name) {
    for (Var *v = g_vars; v; v = v->next)
        if (!strcmp(v->name, name)) return v;
    return NULL;
}

const char *var_get(const char *name) {
    Var *v = find(name);
    return v ? v->value : NULL;
}

void var_set(const char *name, const char *value) {
    Var *v = find(name);
    if (v) {
        free(v->value);
        v->value = xstrdup(value);
        return;
    }
    v = xmalloc(sizeof *v);
    v->name = xstrdup(name);
    v->value = xstrdup(value);
    v->exported = 0;
    v->next = g_vars;
    g_vars = v;
}

void var_export(const char *name) {
    Var *v = find(name);
    if (!v) { var_set(name, ""); v = find(name); }
    v->exported = 1;
}

int var_is_exported(const char *name) {
    Var *v = find(name);
    return v && v->exported;
}

void var_unset(const char *name) {
    for (Var **pp = &g_vars; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->name, name)) {
            Var *v = *pp;
            *pp = v->next;
            free(v->name);
            free(v->value);
            free(v);
            return;
        }
    }
}

void vars_init(char **envp) {
    for (; envp && *envp; envp++) {
        char *eq = strchr(*envp, '=');
        if (!eq) continue;
        char *name = xmalloc((size_t)(eq - *envp) + 1);
        memcpy(name, *envp, (size_t)(eq - *envp));
        name[eq - *envp] = 0;
        var_set(name, eq + 1);
        var_export(name);
        free(name);
    }
}

char **vars_environ(void) {
    int n = 0;
    for (Var *v = g_vars; v; v = v->next) if (v->exported) n++;
    char **env = xmalloc((size_t)(n + 1) * sizeof(char *));
    int i = 0;
    for (Var *v = g_vars; v; v = v->next)
        if (v->exported) env[i++] = xasprintf("%s=%s", v->name, v->value);
    env[i] = NULL;
    return env;
}

void vars_each(void (*fn)(const char *name, const char *value, int exported, void *ctx), void *ctx) {
    for (Var *v = g_vars; v; v = v->next) fn(v->name, v->value, v->exported, ctx);
}

/* ---- function-local variables ---- */

void local_push(void) {
    Scope *s = xmalloc(sizeof *s);
    s->saves = NULL;
    s->up = g_scope;
    g_scope = s;
}

void local_declare(const char *name) {
    if (!g_scope) return;
    for (LocalSave *l = g_scope->saves; l; l = l->next)
        if (!strcmp(l->name, name)) return;
    LocalSave *l = xmalloc(sizeof *l);
    Var *v = find(name);
    l->name = xstrdup(name);
    l->old_value = v ? xstrdup(v->value) : NULL;
    l->old_exported = v ? v->exported : 0;
    l->next = g_scope->saves;
    g_scope->saves = l;
}

void local_pop(void) {
    Scope *s = g_scope;
    if (!s) return;
    g_scope = s->up;
    for (LocalSave *l = s->saves; l;) {
        LocalSave *next = l->next;
        if (l->old_value) {
            var_set(l->name, l->old_value);
            if (l->old_exported) var_export(l->name);
        } else {
            var_unset(l->name);
        }
        free(l->name);
        free(l->old_value);
        free(l);
        l = next;
    }
    free(s);
}
