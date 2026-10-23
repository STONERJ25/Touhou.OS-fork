/* data structures and a small expression evaluator */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- singly linked list */
struct node {
    int value;
    struct node *next;
};

static struct node *push(struct node *head, int v) {
    struct node *n = malloc(sizeof *n);
    n->value = v;
    n->next = head;
    return n;
}

static struct node *reverse(struct node *head) {
    struct node *prev = NULL;
    while (head) {
        struct node *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

/* ---- binary search tree */
struct tree {
    int key;
    struct tree *left, *right;
};

static struct tree *insert(struct tree *t, int key) {
    if (!t) {
        t = calloc(1, sizeof *t);
        t->key = key;
        return t;
    }
    if (key < t->key) t->left = insert(t->left, key);
    else if (key > t->key) t->right = insert(t->right, key);
    return t;
}

static void inorder(struct tree *t, int *out, int *n) {
    if (!t) return;
    inorder(t->left, out, n);
    out[(*n)++] = t->key;
    inorder(t->right, out, n);
}

static int height(struct tree *t) {
    if (!t) return 0;
    int l = height(t->left), r = height(t->right);
    return 1 + (l > r ? l : r);
}

/* ---- hash table with chaining */
#define BUCKETS 31
struct entry {
    char *key;
    int count;
    struct entry *next;
};
static struct entry *table[BUCKETS];

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = (h << 5) + h + (unsigned char)*s++;
    return h;
}

static void count_word(const char *w) {
    unsigned b = hash(w) % BUCKETS;
    for (struct entry *e = table[b]; e; e = e->next)
        if (!strcmp(e->key, w)) {
            e->count++;
            return;
        }
    struct entry *e = malloc(sizeof *e);
    e->key = strdup(w);
    e->count = 1;
    e->next = table[b];
    table[b] = e;
}

static int cmp_entries(const void *a, const void *b) {
    const struct entry *x = *(const struct entry *const *)a, *y = *(const struct entry *const *)b;
    if (x->count != y->count) return y->count - x->count;
    return strcmp(x->key, y->key);
}

/* ---- dynamic array */
struct vec {
    int *data;
    int len, cap;
};

static void vec_push(struct vec *v, int x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 2;
        v->data = realloc(v->data, (size_t)v->cap * sizeof(int));
    }
    v->data[v->len++] = x;
}

/* ---- recursive descent calculator */
static const char *src;
static double parse_expr(void);

static void skip_ws(void) {
    while (isspace((unsigned char)*src)) src++;
}

static double parse_primary(void) {
    skip_ws();
    if (*src == '(') {
        src++;
        double v = parse_expr();
        skip_ws();
        if (*src == ')') src++;
        return v;
    }
    if (*src == '-') {
        src++;
        return -parse_primary();
    }
    char *end;
    double v = strtod(src, &end);
    src = end;
    return v;
}

static double parse_term(void) {
    double v = parse_primary();
    for (;;) {
        skip_ws();
        if (*src == '*') {
            src++;
            v *= parse_primary();
        } else if (*src == '/') {
            src++;
            v /= parse_primary();
        } else {
            return v;
        }
    }
}

static double parse_expr(void) {
    double v = parse_term();
    for (;;) {
        skip_ws();
        if (*src == '+') {
            src++;
            v += parse_term();
        } else if (*src == '-') {
            src++;
            v -= parse_term();
        } else {
            return v;
        }
    }
}

static double calc(const char *s) {
    src = s;
    return parse_expr();
}

int main(void) {
    struct node *list = NULL;
    for (int i = 1; i <= 8; i++) list = push(list, i * i);
    printf("list:");
    for (struct node *n = list; n; n = n->next) printf(" %d", n->value);
    list = reverse(list);
    printf("\nreversed:");
    for (struct node *n = list; n; n = n->next) printf(" %d", n->value);
    printf("\n");
    while (list) {
        struct node *n = list->next;
        free(list);
        list = n;
    }

    struct tree *root = NULL;
    int keys[] = {50, 30, 70, 20, 40, 60, 80, 35, 45, 65, 30, 50};
    for (int i = 0; i < 12; i++) root = insert(root, keys[i]);
    int sorted[16], n = 0;
    inorder(root, sorted, &n);
    printf("tree: %d keys, height %d:", n, height(root));
    for (int i = 0; i < n; i++) printf(" %d", sorted[i]);
    printf("\n");

    char text[] = "the quick brown fox jumps over the lazy dog the fox";
    for (char *w = strtok(text, " "); w; w = strtok(NULL, " ")) count_word(w);
    struct entry *all[64];
    int na = 0;
    for (int b = 0; b < BUCKETS; b++)
        for (struct entry *e = table[b]; e; e = e->next) all[na++] = e;
    qsort(all, (size_t)na, sizeof all[0], cmp_entries);
    for (int i = 0; i < 4; i++) printf("%s=%d ", all[i]->key, all[i]->count);
    printf("(%d distinct)\n", na);

    struct vec v = {0};
    for (int i = 0; i < 100; i++) vec_push(&v, i * 3);
    long sum = 0;
    for (int i = 0; i < v.len; i++) sum += v.data[i];
    printf("vec: len=%d cap=%d sum=%ld\n", v.len, v.cap, sum);
    free(v.data);

    const char *exprs[] = {"1+2*3", "(1+2)*3", "10/4-1", "-(2+3)*2", "2*(3+4)*(5-1)", "100/(2*5)/5", " 3.5 * 2 "};
    for (int i = 0; i < 7; i++) printf("%s = %g\n", exprs[i], calc(exprs[i]));
    return 0;
}
