/* preprocessor test: compared token by token with clang -E in tests/mcc_cmp.py (not run in the guest) */
#define EMPTY
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)
#define ADD(a, b) ((a) + (b))
#define VA(fmt, ...) printf(fmt, ## __VA_ARGS__)
#define VB(...) f(__VA_ARGS__)
#define VC(a, ...) a: __VA_ARGS__
#define NUM 42
#define OBJ NUM + 1
#define F(x) x + F(x)
#define LPAREN (
#define APPLY(m, x) m x
#define SELF SELF + 1
#define A B
#define B A
#define EMPTYARG(x) [x]
#define TWO(a, b) a b
#define NAMEDVA(args...) g(args)

STR(hello world) STR( a   +   b ) STR("quoted \n" 'c') XSTR(NUM) XSTR(OBJ)
CAT(foo, bar) XCAT(NUM, 1) CAT(1, 2) CAT(, x) CAT(x, ) XCAT(a, XCAT(b, c))
ADD(1, 2) ADD(ADD(1, 2), 3) ADD((1, 2), 3)
VA("x") VA("x", 1) VA("x", 1, 2) VB() VB(1) VB(1, 2, 3) VC(k) VC(k, v) VC(k, v, w)
OBJ F(1) SELF A B EMPTYARG() EMPTYARG(1) EMPTYARG( a b ) NAMEDVA() NAMEDVA(1,2)
APPLY(ADD, (1, 2)) EMPTY int EMPTY x;
TWO(1, 2) TWO(,) TWO( ,2) TWO(1,)

#if defined(NUM) && NUM == 42 && (1 << 3) == 8 && -1 < 0 && (0xFFFFFFFF > 0) && 'a' == 97
ok1
#else
bad1
#endif
#if 0
#error never
#elif 1 ? 0 : 1
bad2
#elif (2 + 3 * 4) == 14 && !defined(NOPE) && UNDEFINED_THING == 0
ok2
#else
bad3
#endif
#ifdef NUM
ok3
#endif
#ifndef NUM
bad4
#endif
#undef NUM
#ifdef NUM
bad5
#else
ok4
#endif
#if -1 > 0u
ok5
#endif
#if (-1 >> 1) == -1 && (7 / 2) == 3 && (-7 % 3) == -1 && ~0 == -1
ok6
#endif
__LINE__ __FILE__ __COUNTER__ __COUNTER__ __STDC__ __STDC_VERSION__
#line 100
__LINE__
#define LINE_USER __LINE__
LINE_USER
"abc" "def" "g\x41"
L"wide" L"x" "y"
'\n' '\\' '\'' '\0' '\x41' 'ab' L'x'
0x1F 017 0b101 1.5e3 .5f 0x1.8p3 100ULL 1u 'a'
a ## b # c ??=
