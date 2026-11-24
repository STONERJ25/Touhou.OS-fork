/* Files through kernel32: CreateFileA with its creation dispositions and their documented last errors, WriteFile, GetFileSize, ReadFile back, CloseHandle
 * (twice: the second is ERROR_INVALID_HANDLE), a missing file (ERROR_FILE_NOT_FOUND), and GetFileType of a file. Takes the file name as its one argument
 * (the last word of the command line). Prints one line per step. */
#include "win.h"

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
IMPORT HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD attrs, HANDLE tmpl);
IMPORT BOOL CloseHandle(HANDLE h);
IMPORT DWORD GetFileSize(HANDLE h, DWORD *high);
IMPORT BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped);
IMPORT DWORD GetFileType(HANDLE h);

/* step(what, value, error): the arguments are evaluated in no fixed order, so a call and GetLastError never share one step() */
static void step(const char *what, unsigned long long v, DWORD err) {
    out(what); out(" "); outnum(v); out(" error "); outnum(err); out("\n");
}

void start(void) {
    char *cl = GetCommandLineA(), *name = cl;
    for (char *p = cl; *p; p++) if (*p == ' ') name = p + 1;
    const char text[] = "written by a Windows program\n";
    HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    step("create", h != (HANDLE)-1, GetLastError());          /* h is set before step() is called: fine */
    DWORD w = 0;
    BOOL ok = WriteFile(h, text, sizeof text - 1, &w, 0);
    step("write", ok && w == sizeof text - 1, 0);
    step("type", GetFileType(h), 0);
    CloseHandle(h);
    h = CreateFileA(name, GENERIC_READ, 0, 0, 1 /* CREATE_NEW */, 0x80, 0);
    step("create_new on an existing file", h != (HANDLE)-1, GetLastError());
    h = CreateFileA(name, GENERIC_READ, 0, 0, 4 /* OPEN_ALWAYS */, 0x80, 0);
    step("open_always", h != (HANDLE)-1, GetLastError());
    DWORD high = 7;
    DWORD size = GetFileSize(h, &high);
    step("size", size, high);
    char buf[64];
    DWORD got = 0;
    ok = ReadFile(h, buf, sizeof buf, &got, 0);
    int same = ok && got == sizeof text - 1;
    for (DWORD i = 0; same && i < got; i++) if (buf[i] != text[i]) same = 0;
    step("read back", same, 0);
    ok = ReadFile(h, buf, sizeof buf, &got, 0);
    step("read at the end", ok && got == 0, 0);
    step("close", CloseHandle(h), 0);
    SetLastError(0);
    ok = CloseHandle(h);
    step("close again", ok, GetLastError());
    h = CreateFileA("/tmp/win/missing.txt", GENERIC_READ, 0, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    step("missing", h != (HANDLE)-1, GetLastError());
    ExitProcess(0);
}
