/* control flow: loops, switch, goto, recursion, statement expressions */
#include <stdio.h>

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int ackermann(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static const char *classify(int c) {
    switch (c) {
    case 0: return "zero";
    case 1:
    case 2:
    case 3: return "small";
    case 4 ... 9: return "digit";
    case 'a': return "letter a";
    case 100: return "hundred";
    default: return "other";
    }
}

static int fallthrough(int n) {
    int r = 0;
    switch (n) {
    case 3: r += 3;
    case 2: r += 2;
    case 1: r += 1; break;
    case 10: r = 10;
    default: r += 100;
    }
    return r;
}

static int gcd(int a, int b) {
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int collatz(long n) {
    int steps = 0;
    while (n != 1) {
        n = (n % 2) ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}

static int primes_below(int limit) {
    int count = 0;
    for (int n = 2; n < limit; n++) {
        int ok = 1;
        for (int d = 2; d * d <= n; d++) {
            if (n % d == 0) {
                ok = 0;
                break;
            }
        }
        if (ok) count++;
    }
    return count;
}

static int goto_demo(int n) {
    int i = 0, total = 0;
loop:
    if (i >= n) goto done;
    total += i;
    i++;
    goto loop;
done:
    return total;
}

static int nested(int n) {
    int found = -1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (j > i) continue;
            if (i * j == 12) {
                found = i * 100 + j;
                goto out;
            }
        }
    }
out:
    return found;
}

static int dowhile(int n) {
    int c = 0;
    do {
        c++;
        n /= 2;
    } while (n > 0);
    return c;
}

static int hanoi_moves;
static void hanoi(int n, int from, int to, int via) {
    if (n == 0) return;
    hanoi(n - 1, from, via, to);
    hanoi_moves++;
    hanoi(n - 1, via, to, from);
}

int main(void) {
    printf("fib(20)=%d ack(2,3)=%d gcd(84,36)=%d collatz(27)=%d primes<100=%d\n", fib(20), ackermann(2, 3), gcd(84, 36), collatz(27), primes_below(100));
    for (int c = 0; c < 12; c++) printf("%d:%s ", c, classify(c));
    printf("\n%s %s %s\n", classify('a'), classify(100), classify(-5));
    printf("fallthrough: %d %d %d %d %d\n", fallthrough(1), fallthrough(2), fallthrough(3), fallthrough(10), fallthrough(7));
    printf("goto=%d nested=%d dowhile=%d\n", goto_demo(10), nested(10), dowhile(100));
    hanoi(10, 1, 3, 2);
    printf("hanoi moves=%d\n", hanoi_moves);

    int x = 10;
    int y = ({
        int t = x * 2;
        t + 1;
    });
    printf("stmt expr=%d\n", y);

    int count = 0;
    for (int i = 0, j = 10; i < j; i++, j--) count += j - i;
    printf("two vars: %d\n", count);

    int w = 0;
    while (1) {
        if (++w > 5) break;
        if (w % 2) continue;
        printf("w=%d ", w);
    }
    printf("\n");

    int k;
    for (k = 0; k < 3; k++)
        ;
    printf("k=%d\n", k);

    char text[] = "count the vowels in this sentence";
    int vowels = 0;
    for (char *p = text; *p; p++) {
        switch (*p) {
        case 'a':
        case 'e':
        case 'i':
        case 'o':
        case 'u':
            vowels++;
            break;
        }
    }
    printf("vowels=%d\n", vowels);

    unsigned char uc = 200;
    switch (uc) {
    case 200: printf("uchar 200\n"); break;
    default: printf("no\n");
    }
    long lv = 5000000000L;
    switch (lv) {
    case 5000000000L: printf("long case\n"); break;
    default: printf("no\n");
    }
    int bits = 0;
    for (unsigned v = 0xF0F0u; v; v >>= 1) bits += v & 1;
    printf("bits=%d\n", bits);
    return 0;
}
