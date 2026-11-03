/* compiled by mcc ON the guest: files, formatted input and output, buffers */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/mcc-stdio.txt";
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("cannot create: %s\n", strerror(errno));
        return 1;
    }
    for (int i = 1; i <= 5; i++) fprintf(f, "line %d: %s %5.2f\n", i, i % 2 ? "odd" : "even", i * 1.5);
    fputs("last line without newline", f);
    fclose(f);

    f = fopen(path, "r");
    char buf[128];
    int lines = 0, chars = 0;
    while (fgets(buf, sizeof buf, f)) {
        lines++;
        chars += (int)strlen(buf);
        if (lines == 2) printf("second: %s", buf);
    }
    printf("lines=%d chars=%d eof=%d\n", lines, chars, feof(f) != 0);
    rewind(f);
    int n;
    char word[16];
    double d;
    if (fscanf(f, "line %d: %15s %lf", &n, word, &d) == 3) printf("scanned: %d %s %.2f\n", n, word, d);
    fseek(f, -4, SEEK_END);
    printf("tail: %s (at %ld)\n", fgets(buf, sizeof buf, f), ftell(f));
    fclose(f);

    struct stat st;
    if (stat(path, &st) == 0) printf("size=%ld regular=%d\n", (long)st.st_size, (st.st_mode & 0170000) == 0100000);
    char out[64];
    int len = snprintf(out, sizeof out, "[%08.3f|%-6s|%6s|%+d|%#x|%c]", 3.14159, "ab", "cd", 7, 255, 'Z');
    printf("%s (%d)\n", out, len);
    int a, b;
    char op;
    if (sscanf("12 + 30", "%d %c %d", &a, &op, &b) == 3) printf("sscanf: %d %c %d = %d\n", a, op, b, a + b);
    printf("strtol: %ld %ld %lu\n", strtol("  -42abc", NULL, 10), strtol("0x1f", NULL, 16), strtoul("777", NULL, 8));
    printf("atoi/atof: %d %.2f\n", atoi("123xyz"), atof("2.75"));
    remove(path);
    printf("removed: %d\n", access(path, F_OK) != 0);
    fprintf(stderr, "this goes to stderr\n");
    char *line = NULL;
    size_t cap = 0;
    FILE *pipe_in = fopen("/proc/version", "r");
    if (pipe_in) {
        ssize_t got = getline(&line, &cap, pipe_in);
        printf("getline read something: %d\n", got > 0);
        free(line);
        fclose(pipe_in);
    }
    return 0;
}
