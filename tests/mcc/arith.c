/* integer arithmetic, conversions, shifts, comparisons, increments */
#include <stdio.h>

static int g_calls;
static int next_val(void) { return ++g_calls; }

static void line(const char *name, long long v) { printf("%s = %lld\n", name, v); }
static void uline(const char *name, unsigned long long v) { printf("%s = %llu\n", name, v); }

int main(void) {
    int a = 17, b = 5;
    line("a+b", a + b);
    line("a-b", a - b);
    line("a*b", a * b);
    line("a/b", a / b);
    line("a%b", a % b);
    line("-a/b", -a / b);
    line("-a%b", -a % b);
    line("a/-b", a / -b);
    line("a%-b", a % -b);
    line("a&b", a & b);
    line("a|b", a | b);
    line("a^b", a ^ b);
    line("~a", ~a);
    line("a<<3", a << 3);
    line("a>>2", a >> 2);
    line("-a>>2", -a >> 2);
    line("!a", !a);
    line("!0", !0);
    line("a==b", a == b);
    line("a!=b", a != b);
    line("a<b", a < b);
    line("a<=17", a <= 17);
    line("a>b", a > b);
    line("a>=18", a >= 18);

    unsigned int u = 4000000000u;
    uline("u", u);
    uline("u+u", u + u);
    uline("u*2", u * 2);
    uline("u/7", u / 7);
    uline("u%7", u % 7);
    uline("u>>3", u >> 3);
    line("u>5", u > 5);
    line("(int)u", (int)u);
    line("(long)u", (long)u);
    line("-1<1u", -1 < 1u);
    line("(char)300", (char)300);
    line("(unsigned char)-1", (unsigned char)-1);
    line("(short)70000", (short)70000);
    line("(unsigned short)-2", (unsigned short)-2);

    long long big = 0x123456789abcdefLL;
    uline("big", (unsigned long long)big);
    uline("big>>8", (unsigned long long)(big >> 8));
    line("big*3", big * 3);
    line("big/1000", big / 1000);
    line("big%1000", big % 1000);
    line("(int)big", (int)big);
    unsigned long long ub = 0xfedcba9876543210ULL;
    uline("ub", ub);
    uline("ub>>4", ub >> 4);
    uline("ub/10", ub / 10);
    uline("ub%10", ub % 10);
    line("ub>1", ub > 1);
    line("(long long)ub", (long long)ub);

    char c = 100;
    c += 100;
    line("c+=100", c);
    unsigned char uc = 250;
    uc += 10;
    line("uc+=10", uc);
    short s = 32767;
    s++;
    line("s++", s);
    int i = 10;
    line("i++", i++);
    line("i", i);
    line("++i", ++i);
    line("i--", i--);
    line("--i", --i);
    i += 5;
    i -= 2;
    i *= 3;
    i /= 2;
    i %= 7;
    i <<= 2;
    i >>= 1;
    i |= 8;
    i &= 12;
    i ^= 5;
    line("i ops", i);

    int x = 5, y = 7, z;
    z = (x > y) ? x : y;
    line("max", z);
    z = x < y ? (y < 10 ? 1 : 2) : 3;
    line("nested ?:", z);
    z = (x++, y++, x + y);
    line("comma", z);
    line("x", x);
    line("y", y);

    int p = 0, q = 3;
    line("0&&sideeffect", p && next_val());
    line("calls", g_calls);
    line("3||sideeffect", q || next_val());
    line("calls", g_calls);
    line("1&&sideeffect", q && next_val());
    line("calls", g_calls);
    line("0||sideeffect", p || next_val());
    line("calls", g_calls);

    line("sizeof(char)", sizeof(char));
    line("sizeof(short)", sizeof(short));
    line("sizeof(int)", sizeof(int));
    line("sizeof(long)", sizeof(long));
    line("sizeof(long long)", sizeof(long long));
    line("sizeof(void*)", sizeof(void *));
    line("sizeof(float)", sizeof(float));
    line("sizeof(double)", sizeof(double));
    line("sizeof(a+b)", sizeof(a + b));
    line("sizeof(c+c)", sizeof(c + c));
    line("sizeof(1L)", sizeof(1L));
    line("sizeof('a')", sizeof('a'));

    line("'a'", 'a');
    line("'\\n'", '\n');
    line("'\\x41'", '\x41');
    line("'\\101'", '\101');
    line("0x7fffffff+1u", 0x7fffffff + 1u);
    line("07 + 010", 07 + 010);
    line("0b101", 0b101);
    line("1000000*1000000LL", 1000000 * 1000000LL);

    int arr[5];
    for (int k = 0; k < 5; k++) arr[k] = k * k;
    int sum = 0;
    for (int k = 0; k < 5; k++) sum += arr[k];
    line("sum squares", sum);

    unsigned h = 5381;
    const char *str = "hash me please";
    while (*str) h = h * 33 + (unsigned char)*str++;
    uline("hash", h);

    long fact = 1;
    for (int k = 2; k <= 20; k++) fact *= k;
    line("20!", fact);

    int neg = -7;
    line("neg/2", neg / 2);
    line("neg%2", neg % 2);
    line("neg>>1", neg >> 1);
    line("(unsigned)neg>>28", (unsigned)neg >> 28);
    line("neg<<1", neg << 1);
    line("abs-like", neg < 0 ? -neg : neg);
    return 0;
}
