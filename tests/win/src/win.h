/* The test programs' own minimal Win32 declarations (no SDK header, no C runtime): written from the Win32 API reference on Microsoft Learn. Built by
 * tools/make-winprogs.py for --target=x86_64-pc-windows-msvc; every function here is imported from KERNEL32.dll through the import libraries made from
 * the .def files in tests/win. Each program is one translation unit, so the helpers below are plain static functions, and memset/memcpy (which the compiler may call
 * on its own) are defined here once per program. */
#ifndef TEST_WIN_H
#define TEST_WIN_H

typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef unsigned long long SIZE_T;
typedef unsigned short WCHAR;
#define IMPORT __declspec(dllimport)

IMPORT void ExitProcess(DWORD code);
IMPORT HANDLE GetStdHandle(DWORD which);
IMPORT BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped);
IMPORT DWORD GetLastError(void);
IMPORT void SetLastError(DWORD e);
IMPORT char *GetCommandLineA(void);
IMPORT WCHAR *GetCommandLineW(void);
IMPORT HANDLE GetModuleHandleA(const char *name);
IMPORT HANDLE GetProcessHeap(void);
IMPORT void *HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
IMPORT BOOL HeapFree(HANDLE heap, DWORD flags, void *p);
IMPORT void *HeapReAlloc(HANDLE heap, DWORD flags, void *p, SIZE_T n);
IMPORT SIZE_T HeapSize(HANDLE heap, DWORD flags, const void *p);
IMPORT void *VirtualAlloc(void *addr, SIZE_T n, DWORD type, DWORD protect);
IMPORT BOOL VirtualFree(void *addr, SIZE_T n, DWORD type);
IMPORT DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size);
IMPORT int lstrlenA(const char *s);
IMPORT unsigned long long GetTickCount64(void);
IMPORT BOOL QueryPerformanceCounter(long long *c);
IMPORT BOOL QueryPerformanceFrequency(long long *f);
IMPORT DWORD GetCurrentProcessId(void);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

void *memset(void *d, int c, SIZE_T n) {
    volatile unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, SIZE_T n) {
    volatile unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static DWORD slen(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    return n;
}
static __attribute__((unused)) void out(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}
static __attribute__((unused)) void outnum(unsigned long long v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}

#endif
