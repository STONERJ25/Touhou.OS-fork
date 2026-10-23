#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("Hello from Gensokyo, userland! pid=%d uid=%d\n", getpid(), getuid());
    for (int i = 0; i < argc; i++) printf("argv[%d] = %s\n", i, argv[i]);
    return 0;
}
