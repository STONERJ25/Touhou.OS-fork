/* compiled by mcc ON the guest: hello, arguments, environment and the exit status */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("Hello from Gensokyo, compiled by Marisa!\n");
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("argv[%d]=%s\n", i, i ? argv[i] : "(program)");
    const char *home = getenv("HOME");
    printf("HOME is set: %d\n", home != NULL);
    printf("pid is positive: %d\n", getpid() > 0);
    return 3;
}
