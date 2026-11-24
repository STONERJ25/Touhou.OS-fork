/* What a C runtime reads through gs: (docs/WINDOWS.md, "The TEB and the PEB"): Self, the PEB, the stack bounds, the ids, the last error read and
 * written directly at gs:[0x68] against GetLastError/SetLastError, the PEB's image base, BeingDebugged and the command line UNICODE_STRING, and the TLS
 * slots against TlsAlloc/TlsGetValue/TlsSetValue; then GetCurrentThreadId, the pseudo-handles and a critical section. */
#include "crt.h"

typedef unsigned int DWORD;
typedef void *HANDLE;
#define IMPORT __declspec(dllimport)
IMPORT DWORD GetLastError(void);
IMPORT void SetLastError(DWORD e);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT DWORD GetCurrentThreadId(void);
IMPORT HANDLE GetCurrentProcess(void);
IMPORT HANDLE GetCurrentThread(void);
IMPORT unsigned short *GetCommandLineW(void);
IMPORT DWORD TlsAlloc(void);
IMPORT int TlsFree(DWORD i);
IMPORT void *TlsGetValue(DWORD i);
IMPORT int TlsSetValue(DWORD i, void *v);
IMPORT int IsDebuggerPresent(void);
IMPORT void InitializeCriticalSection(void *cs);
IMPORT void EnterCriticalSection(void *cs);
IMPORT void LeaveCriticalSection(void *cs);
IMPORT void DeleteCriticalSection(void *cs);
IMPORT DWORD GetModuleFileNameA(HANDLE m, char *buf, DWORD size);

extern char __ImageBase;

static unsigned long long gs64(unsigned off) {
    unsigned long long v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((unsigned long long)off));
    return v;
}
static unsigned gs32(unsigned off) {
    unsigned v;
    __asm__ volatile("movl %%gs:(%1), %0" : "=r"(v) : "r"((unsigned long long)off));
    return v;
}
static void gs32_write(unsigned off, unsigned v) { __asm__ volatile("movl %0, %%gs:(%1)" : : "r"(v), "r"((unsigned long long)off) : "memory"); }

static int fails;
static void check(int ok, const char *what) { printf("%s: %s\n", what, ok ? "ok" : "FAILED"); fails += !ok; }

int main(void) {
    unsigned char *teb = (unsigned char *)gs64(0x30);
    unsigned char *peb = (unsigned char *)gs64(0x60);
    check(teb && *(unsigned long long *)(teb + 0x30) == (unsigned long long)teb, "gs:[0x30] is the TEB's own address");
    check(peb && *(unsigned long long *)(teb + 0x60) == (unsigned long long)peb, "gs:[0x60] is the PEB");
    unsigned long long local = (unsigned long long)&teb;
    check(gs64(0x10) <= local && local < gs64(0x08), "the stack lies between StackLimit and StackBase");
    check(gs64(0x40) == GetCurrentProcessId() && gs64(0x48) == GetCurrentThreadId() && GetCurrentThreadId(), "the ids at 0x40 and 0x48");
    SetLastError(1234);
    check(gs32(0x68) == 1234, "SetLastError is visible at gs:[0x68]");
    gs32_write(0x68, 77);
    check(GetLastError() == 77, "a write to gs:[0x68] is GetLastError");
    check(*(void **)(peb + 0x10) == &__ImageBase, "PEB ImageBaseAddress");
    check(peb[2] == 0 && !IsDebuggerPresent(), "PEB BeingDebugged is 0");
    unsigned char *params = *(unsigned char **)(peb + 0x20);
    unsigned short len = *(unsigned short *)(params + 0x70);
    unsigned short *cl = *(unsigned short **)(params + 0x78), *w = GetCommandLineW();
    int same = cl == w;
    for (unsigned i = 0; same && w[i]; i++) if (2 * (i + 1) > len) same = 0;
    check(same && w[len / 2] == 0, "ProcessParameters CommandLine is GetCommandLineW");
    DWORD a = TlsAlloc(), b = TlsAlloc();
    TlsSetValue(b, (void *)0x5eed);
    check(a != b && a < 64 && b < 64 && *(void **)(teb + 0x1480 + 8 * b) == (void *)0x5eed, "TlsSetValue writes TEB TlsSlots[i]");
    *(void **)(teb + 0x1480 + 8 * a) = (void *)0xabc;
    SetLastError(5);
    check(TlsGetValue(a) == (void *)0xabc && GetLastError() == 0, "TlsGetValue reads the slot and clears the last error");
    check(TlsFree(a) && !TlsFree(a) && GetLastError() == 87, "TlsFree twice is ERROR_INVALID_PARAMETER");
    check(GetCurrentProcess() == (HANDLE)-1 && GetCurrentThread() == (HANDLE)-2, "the pseudo-handles");
    unsigned char cs[40];
    InitializeCriticalSection(cs);
    EnterCriticalSection(cs);
    EnterCriticalSection(cs);
    int rec = *(int *)(cs + 12);
    LeaveCriticalSection(cs);
    LeaveCriticalSection(cs);
    check(rec == 2 && *(int *)(cs + 12) == 0 && *(int *)(cs + 8) == -1, "a critical section counts its recursion");
    DeleteCriticalSection(cs);
    char name[256];
    DWORD n = GetModuleFileNameA(NULL, name, sizeof name);
    check(n == strlen(name) && strchr(name, 't') != NULL, "GetModuleFileNameA");
    printf("module: %s\n", name);
    return fails;
}
