/* Reads the file named on the command line with CreateFileA/ReadFile in 1000-byte pieces and prints its size and FNV-1a hash (64 bits). */
#include "crt.h"

typedef unsigned int DWORD;
typedef void *HANDLE;
__declspec(dllimport) HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD attrs, HANDLE tmpl);
__declspec(dllimport) int ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped);
__declspec(dllimport) int CloseHandle(HANDLE h);
__declspec(dllimport) DWORD GetLastError(void);

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: checksum FILE\n"); return 2; }
    HANDLE h = CreateFileA(argv[1], 0x80000000u, 1, NULL, 3, 0x80, NULL);
    if (h == (HANDLE)-1) { fprintf(stderr, "checksum: %s: error %u\n", argv[1], GetLastError()); return 1; }
    unsigned long long hash = 0xcbf29ce484222325ull, size = 0;
    unsigned char buf[1000];
    DWORD got;
    while (ReadFile(h, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) { hash ^= buf[i]; hash *= 0x100000001b3ull; }
        size += got;
    }
    CloseHandle(h);
    printf("%s: %llu bytes, fnv1a64 %016llx\n", argv[1], size, hash);
    return 0;
}
