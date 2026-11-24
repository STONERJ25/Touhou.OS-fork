/* String handling through the mini C runtime: copy, append, search, compare, overlapping moves, snprintf truncation, the argv quoting rules, atexit. */
#include "crt.h"

static void bye(void) { puts("atexit: second registered, first run"); }
static void bye2(void) { puts("atexit: first registered, last run"); }

int main(int argc, char **argv) {
    atexit(bye2);
    atexit(bye);
    char buf[64];
    strcpy(buf, "Reimu");
    strcat(buf, " and ");
    strcat(buf, "Marisa");
    printf("%s (%zu)\n", buf, strlen(buf));
    char *m = strchr(buf, 'M');
    printf("strchr: %s, missing: %s\n", m, strchr(buf, 'Z') ? "found" : "NULL");
    printf("strcmp: %d %d %d, strncmp: %d\n", strcmp("abc", "abd") < 0, strcmp("b", "a") > 0, strcmp("same", "same"), strncmp("abcX", "abcY", 3));
    char mv[16] = "0123456789";
    memmove(mv + 2, mv, 6);
    printf("memmove up: %s\n", mv);
    memmove(mv, mv + 3, 5);
    printf("memmove down: %s\n", mv);
    printf("memcmp: %d\n", memcmp("abc", "abd", 3) < 0);
    char small[8];
    int n = snprintf(small, sizeof small, "%s-%d", "truncated", 12345);
    printf("snprintf: [%s] wanted %d\n", small, n);
    for (int i = 0; i < argc; i++) printf("argv[%d] = [%s]\n", i, argv[i]);
    return 7;
}
