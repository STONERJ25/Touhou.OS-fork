/* The command line as Windows builds it (GetCommandLineA and the UTF-16 copy), the module handle, an environment variable, and a return from the entry
 * point (status 3) instead of ExitProcess. */
#include "win.h"

extern char __ImageBase;        /* lld-link defines it at the image's own base */

int start(void) {
    char *a = GetCommandLineA();
    WCHAR *w = GetCommandLineW();
    out("cmdline: [");
    out(a);
    out("]\n");
    int same = lstrlenA(a) > 0;
    for (int i = 0; a[i] || w[i]; i++) if ((unsigned char)a[i] != w[i]) same = 0;
    out(same ? "wide: same\n" : "wide: DIFFERENT\n");
    out(GetModuleHandleA(0) == (HANDLE)&__ImageBase ? "module: ok\n" : "module: WRONG\n");
    char buf[64];
    DWORD n = GetEnvironmentVariableA("WINTEST", buf, sizeof buf);
    if (n && n < sizeof buf) { out("WINTEST="); out(buf); out("\n"); }
    else out("WINTEST unset\n");
    SetLastError(0);
    n = GetEnvironmentVariableA("WINTEST_SURELY_UNSET", buf, sizeof buf);
    out("unset: "); outnum(n); out(" error "); outnum(GetLastError()); out("\n");
    return 3;
}
