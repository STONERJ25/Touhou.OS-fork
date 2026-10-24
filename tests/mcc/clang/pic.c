/* built by CLANG with -fPIC: every access to a global goes through the GOT (R_X86_64_GOTPCREL / GOTPCRELX / REX_GOTPCRELX),
 * which mld must serve in a static executable */
#include <stdio.h>
#include <string.h>

extern char **environ;
int global_value = 42;
int global_array[4] = {1, 2, 3, 4};
static int counter = 5;
size_t (*fp)(const char *) = strlen;
const char *const names[] = {"alpha", "beta"};

int bump(int by) {
    counter += by;
    return counter;
}

int main(int argc, char **argv) {
    printf("global=%d array=%d counter=%d\n", global_value, global_array[2], bump(3));
    printf("strlen via pointer=%d via call=%d\n", (int)fp("gensokyo"), (int)strlen("marisa"));
    printf("names: %s %s\n", names[0], names[1]);
    printf("environ set: %d, argc=%d\n", environ != 0, argc);
    puts(argv[0] ? "has argv[0]" : "no argv[0]");
    return 0;
}
