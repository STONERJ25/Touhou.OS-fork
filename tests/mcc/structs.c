/* structs, unions, bit-fields, by-value passing and returning, initializers */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

struct point { int x, y; };
struct rect { struct point min, max; };
struct small { char c; };
struct pair8 { int a, b; };
struct three { char a, b, c; };
struct big { long a, b, c, d; char tag[8]; };
struct fl2 { float x, y; };
struct dbl2 { double x, y; };
struct mixed { int i; double d; };
struct mixed2 { double d; int i; };
struct f3 { float x, y, z; };
struct tail { int n; char data[]; };

union val {
    int i;
    float f;
    unsigned char bytes[4];
};

struct flags {
    unsigned a : 3;
    unsigned b : 5;
    int c : 4;
    unsigned d : 1;
    unsigned long long e : 40;
};

struct anon {
    int kind;
    union {
        int num;
        char *str;
    };
    struct {
        int lo, hi;
    };
};

static struct point make_point(int x, int y) {
    struct point p = {x, y};
    return p;
}

static int point_sum(struct point p) { return p.x + p.y; }
static struct rect make_rect(void) {
    struct rect r = {{1, 2}, {30, 40}};
    return r;
}
static int rect_area(struct rect r) { return (r.max.x - r.min.x) * (r.max.y - r.min.y); }
static struct small make_small(char c) { struct small s = {c}; return s; }
static struct three make_three(void) { struct three t = {1, 2, 3}; return t; }
static int three_sum(struct three t) { return t.a + t.b + t.c; }
static struct big make_big(long base) {
    struct big b;
    b.a = base;
    b.b = base * 2;
    b.c = base * 3;
    b.d = base * 4;
    strcpy(b.tag, "bigtag");
    return b;
}
static long big_sum(struct big b) { return b.a + b.b + b.c + b.d + b.tag[0]; }
static struct fl2 make_fl2(float a, float b) { struct fl2 f = {a, b}; return f; }
static float fl2_sum(struct fl2 f) { return f.x + f.y; }
static struct dbl2 make_dbl2(double a, double b) { struct dbl2 d = {a, b}; return d; }
static double dbl2_sum(struct dbl2 d) { return d.x + d.y; }
static struct mixed make_mixed(int i, double d) { struct mixed m = {i, d}; return m; }
static double mixed_sum(struct mixed m) { return m.i + m.d; }
static struct mixed2 make_mixed2(double d, int i) { struct mixed2 m = {d, i}; return m; }
static double mixed2_sum(struct mixed2 m) { return m.i * 2 + m.d; }
static struct f3 make_f3(void) { struct f3 f = {1.5f, 2.5f, 3.5f}; return f; }
static float f3_sum(struct f3 f) { return f.x + f.y + f.z; }

static long many_structs(struct point a, struct point b, struct pair8 c, struct big d, struct point e, struct point f, struct point g) {
    return a.x + b.x + c.a + d.a + e.x + f.x + g.x + a.y + g.y;
}

static void modify_param(struct point p, struct point *out) {
    p.x = 1000;
    *out = p;
}

int main(void) {
    struct point p = make_point(3, 4);
    printf("p=(%d,%d) sum=%d\n", p.x, p.y, point_sum(p));
    struct rect r = make_rect();
    printf("rect (%d,%d)-(%d,%d) area=%d\n", r.min.x, r.min.y, r.max.x, r.max.y, rect_area(r));
    struct point *pp = &r.max;
    pp->x += 5;
    printf("max.x=%d\n", r.max.x);
    struct small sm = make_small('Z');
    printf("small=%c size=%d\n", sm.c, (int)sizeof(sm));
    struct three t = make_three();
    printf("three=%d size=%d\n", three_sum(t), (int)sizeof(t));
    struct big b = make_big(10);
    printf("big=%ld tag=%s size=%d\n", big_sum(b), b.tag, (int)sizeof(b));
    printf("fl2=%.1f dbl2=%.2f\n", fl2_sum(make_fl2(1.0f, 2.5f)), dbl2_sum(make_dbl2(0.5, 0.25)));
    printf("mixed=%.2f mixed2=%.2f\n", mixed_sum(make_mixed(7, 0.5)), mixed2_sum(make_mixed2(1.5, 8)));
    printf("f3=%.1f size=%d\n", f3_sum(make_f3()), (int)sizeof(struct f3));
    struct pair8 pr = {5, 6};
    printf("many=%ld\n", many_structs(p, p, pr, b, p, p, p));

    struct point out;
    modify_param(p, &out);
    printf("after modify p.x=%d out.x=%d\n", p.x, out.x);

    struct point copy = p;
    copy.y = 99;
    printf("copy=(%d,%d) orig=(%d,%d)\n", copy.x, copy.y, p.x, p.y);
    struct point arr[3] = {{1, 2}, {3, 4}, {5, 6}};
    struct point *ap = arr;
    printf("arr[1].y=%d ap[2].x=%d\n", arr[1].y, ap[2].x);
    arr[0] = arr[2];
    printf("arr[0]=(%d,%d)\n", arr[0].x, arr[0].y);
    p = (struct point){7, 8};
    printf("compound literal=(%d,%d)\n", p.x, p.y);
    printf("is equal: %d\n", memcmp(&p, &(struct point){7, 8}, sizeof p) == 0);

    struct rect r2 = {.max = {.y = 9}, .min = {.x = 4}};
    printf("designated: %d %d %d %d\n", r2.min.x, r2.min.y, r2.max.x, r2.max.y);
    int ia[10] = {[2] = 5, [7] = 9, 11};
    printf("ia: %d %d %d %d %d\n", ia[0], ia[2], ia[7], ia[8], ia[9]);

    union val v;
    v.i = 0x3f800000;
    printf("union float=%.1f bytes=%02x %02x %02x %02x size=%d\n", v.f, v.bytes[0], v.bytes[1], v.bytes[2], v.bytes[3], (int)sizeof(v));

    struct flags fl;
    memset(&fl, 0, sizeof fl);
    fl.a = 5;
    fl.b = 17;
    fl.c = -3;
    fl.d = 1;
    fl.e = 0x123456789aULL;
    printf("flags: a=%u b=%u c=%d d=%u e=%llx size=%d\n", fl.a, fl.b, fl.c, fl.d, (unsigned long long)fl.e, (int)sizeof(fl));
    fl.a += 4;
    fl.c--;
    fl.b = 100;
    printf("flags: a=%u b=%u c=%d d=%u\n", fl.a, fl.b, fl.c, fl.d);
    fl.d = !fl.d;
    int cc = fl.c = 20;
    printf("flags: d=%u cc=%d c=%d\n", fl.d, cc, fl.c);

    struct anon an;
    an.kind = 1;
    an.num = 77;
    an.lo = 3;
    an.hi = 4;
    printf("anon: %d %d %d %d size=%d\n", an.kind, an.num, an.lo, an.hi, (int)sizeof(an));
    printf("offsets: %d %d %d\n", (int)offsetof(struct anon, num), (int)offsetof(struct anon, lo), (int)offsetof(struct rect, max));

    struct tail *tl = (struct tail *)&(char[32]){0};
    tl->n = 3;
    strcpy(tl->data, "xy");
    printf("tail: %d %s size=%d\n", tl->n, tl->data, (int)sizeof(struct tail));
    printf("sizes: %d %d %d %d %d\n", (int)sizeof(struct mixed), (int)sizeof(struct mixed2), (int)sizeof(struct dbl2), (int)sizeof(struct three), (int)sizeof(struct big));
    return 0;
}
