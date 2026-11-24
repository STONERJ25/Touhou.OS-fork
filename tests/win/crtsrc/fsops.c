/* Files and directories by name, file positions and the UTF-8 conversions: CreateDirectoryA (twice: ERROR_ALREADY_EXISTS), GetFileAttributesA,
 * SetFilePointer and SetFilePointerEx (from the start, the current position and the end; a negative position is ERROR_NEGATIVE_SEEK), DeleteFileA,
 * RemoveDirectoryA, GetCurrentDirectoryA, MultiByteToWideChar and WideCharToMultiByte with CP_UTF8. argv[1] is a directory name to create and remove. */
#include "crt.h"

typedef unsigned int DWORD;
typedef void *HANDLE;
typedef unsigned short WCHAR;
#define IMPORT __declspec(dllimport)
IMPORT HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD attrs, HANDLE tmpl);
IMPORT int ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped);
IMPORT int WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped);
IMPORT int CloseHandle(HANDLE h);
IMPORT DWORD GetLastError(void);
IMPORT DWORD SetFilePointer(HANDLE h, int lo, int *hi, DWORD method);
IMPORT int SetFilePointerEx(HANDLE h, long long dist, long long *newpos, DWORD method);
IMPORT DWORD GetFileAttributesA(const char *name);
IMPORT int DeleteFileA(const char *name);
IMPORT int CreateDirectoryA(const char *name, void *sa);
IMPORT int RemoveDirectoryA(const char *name);
IMPORT DWORD GetCurrentDirectoryA(DWORD size, char *buf);
IMPORT int MultiByteToWideChar(DWORD cp, DWORD flags, const char *src, int srclen, WCHAR *dst, int dstlen);
IMPORT int WideCharToMultiByte(DWORD cp, DWORD flags, const WCHAR *src, int srclen, char *dst, int dstlen, const char *def, int *used);

static char two[3];
static const char *read2(HANDLE h) { DWORD got = 0; ReadFile(h, two, 2, &got, NULL); two[got] = 0; return two; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *dir = argv[1];
    char file[256];
    snprintf(file, sizeof file, "%s/f.txt", dir);
    int ok = CreateDirectoryA(dir, NULL);
    printf("mkdir: %d\n", ok);
    ok = CreateDirectoryA(dir, NULL);
    printf("mkdir again: %d error %u\n", ok, GetLastError());
    printf("dir attributes: directory %d\n", (GetFileAttributesA(dir) & 0x10) != 0);
    HANDLE h = CreateFileA(file, 0xC0000000u, 0, NULL, 2, 0x80, NULL);
    DWORD w;
    WriteFile(h, "0123456789", 10, &w, NULL);
    DWORD p = SetFilePointer(h, 3, NULL, 0);
    printf("seek 3 from the start: %u, read [%s]\n", p, read2(h));
    long long pos = 0;
    ok = SetFilePointerEx(h, -2, &pos, 2);
    printf("seek -2 from the end: %d %lld, read [%s]\n", ok, pos, read2(h));
    p = SetFilePointer(h, -4, NULL, 1);
    printf("seek -4 from here: %u, read [%s]\n", p, read2(h));
    p = SetFilePointer(h, -100, NULL, 1);
    printf("seek before the start: %x error %u\n", p, GetLastError());
    CloseHandle(h);
    printf("file attributes: directory %d\n", (GetFileAttributesA(file) & 0x10) != 0);
    DWORD a = GetFileAttributesA("no-such-file.txt");
    printf("missing: %x error %u\n", a, GetLastError());
    ok = DeleteFileA(file);
    printf("delete: %d\n", ok);
    ok = DeleteFileA(file);
    printf("delete again: %d error %u\n", ok, GetLastError());
    printf("rmdir: %d\n", RemoveDirectoryA(dir));
    char cwd[512];
    DWORD need = GetCurrentDirectoryA(0, NULL);
    DWORD got = GetCurrentDirectoryA(sizeof cwd, cwd);
    printf("cwd: needs %d more than its length: %d\n", 1, need == got + 1 && got == strlen(cwd));
    const char *u8 = "Gensokyo \xe6\x9d\xb1\xe6\x96\xb9 \xf0\x9f\x8c\xb8!";      /* "Gensokyo", two CJK characters, a character beyond U+FFFF */
    int wn = MultiByteToWideChar(65001, 0, u8, -1, NULL, 0);
    WCHAR wbuf[64];
    int wn2 = MultiByteToWideChar(65001, 0, u8, -1, wbuf, 64);
    printf("to UTF-16: %d units (%d), [%x %x %x %x]\n", wn, wn2, wbuf[9], wbuf[10], wbuf[12], wbuf[13]);
    int small = MultiByteToWideChar(65001, 0, u8, -1, wbuf, 4);
    printf("too small: %d error %u\n", small, GetLastError());
    char back[64];
    int bn = WideCharToMultiByte(65001, 0, wbuf, -1, back, sizeof back, NULL, NULL);
    printf("back to UTF-8: %d bytes, same %d\n", bn, bn == (int)strlen(u8) + 1 && !strcmp(back, u8));
    return 0;
}
