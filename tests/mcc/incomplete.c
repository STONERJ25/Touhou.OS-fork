/* A qualified pointer to a struct that is still incomplete when the qualifier is written (const struct Node *, const Node *, volatile Node *)
 * must see the struct's members and size once it is defined.  The kernel's vfs.h has exactly this shape (const VNodeOps *ops before
 * struct VNodeOps); mcc used to refuse it with "member access in an incomplete type". */
#include <stdio.h>

struct Node;
typedef struct Node Node;
struct Holder {
    const struct Node *c;
    const Node *d;
    volatile Node *v;
};
struct Node {
    int val;
    struct Node *next;
    const struct Node *cnext;
};

static int sum(const struct Holder *h) {
    return h->c->val + h->d->next->val + h->v->val + h->c->cnext->val;
}

int main(void) {
    struct Node a = {1, 0, 0}, b = {20, 0, &a}, c = {300, &b, &b};
    struct Holder h = {&c, &c, &b};
    printf("%d\n", sum(&h));
    const Node *p = &c;
    Node copy = *p;
    printf("%d %d %d\n", copy.val, (int)sizeof(*p), (int)sizeof(Node));
    return 0;
}
