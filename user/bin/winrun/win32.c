/* The Win32 layer of winrun: a few kernel32 functions written from their documented behaviour on Microsoft Learn (the "Win32 API" reference pages of each
 * function: parameters, return values on success and failure, the last-error codes they name) and from the x64 calling convention, over the native libc.
 * Every function is ms_abi: the program calls it through its import address table with the Windows convention (rcx, rdx, r8, r9, shadow space), and the
 * compiler converts to the System V convention of the libc calls inside. No other implementation was looked at.
 *
 * Handles are small numbers (4, 8, 12, ...) into one table: the three standard handles occupy the first slots (GetStdHandle hands them out), files that
 * CreateFileA opens take the rest. The last error is one variable: winrun runs one thread. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "winrun.h"

typedef int BOOL;
typedef uint32_t DWORD;
typedef void *HANDLE;
typedef uint16_t WCHAR;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

/* System error codes (Microsoft Learn, "System Error Codes (0-499)") */
#define ERROR_SUCCESS            0
#define ERROR_FILE_NOT_FOUND     2
#define ERROR_PATH_NOT_FOUND     3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED      5
#define ERROR_INVALID_HANDLE     6
#define ERROR_NOT_ENOUGH_MEMORY  8
#define ERROR_INVALID_ADDRESS    487
#define ERROR_FILE_EXISTS        80
#define ERROR_INVALID_PARAMETER  87
#define ERROR_MOD_NOT_FOUND      126
#define ERROR_ALREADY_EXISTS     183
#define ERROR_ENVVAR_NOT_FOUND   203

#define HEAP_ZERO_MEMORY             0x00000008
#define HEAP_REALLOC_IN_PLACE_ONLY   0x00000010
#define MEM_COMMIT   0x00001000
#define MEM_RESERVE  0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE  0x00008000
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3
#define INVALID_FILE_SIZE 0xFFFFFFFFu

/* The TEB and the PEB (W2). A program linked with a C runtime reads them through gs: the thread's TEB is gs:[0], and the fields below are the ones that
 * Microsoft documents (winternl.h's TEB and PEB on Microsoft Learn: the NT_TIB at the start of the TEB, ProcessEnvironmentBlock after twelve pointers,
 * TlsSlots; the PEB's BeingDebugged, ProcessParameters with RTL_USER_PROCESS_PARAMETERS' ImagePathName and CommandLine; the "Thread Local Storage" page for
 * TlsAlloc and the 64 slots) and the x64 offsets that follow from them, confirmed by our own teb test program. Everything else stays zero.
 * One thread: one TEB, in winrun's own data. The last error lives in the TEB so that a GetLastError the compiler inlines (a read of gs:[0x68]) and ours agree. */
enum {
    TEB_STACK_BASE = 0x08, TEB_STACK_LIMIT = 0x10, TEB_SELF = 0x30, TEB_PROCESS_ID = 0x40, TEB_THREAD_ID = 0x48, TEB_TLS_POINTER = 0x58, TEB_PEB = 0x60,
    TEB_LAST_ERROR = 0x68, TEB_TLS_SLOTS = 0x1480, TEB_SIZE = 0x2000,
    PEB_BEING_DEBUGGED = 0x02, PEB_IMAGE_BASE = 0x10, PEB_PARAMETERS = 0x20, PEB_SIZE = 0x1000,
    PARAMS_IMAGE_PATH = 0x60, PARAMS_COMMAND_LINE = 0x70, PARAMS_SIZE = 0x400,
};
static uint8_t g_teb[TEB_SIZE] __attribute__((aligned(4096)));
static uint8_t g_peb[PEB_SIZE] __attribute__((aligned(4096)));
static uint8_t g_params[PARAMS_SIZE] __attribute__((aligned(16)));
static void *g_tls_array[1];                    /* ThreadLocalStoragePointer: one module, and images with a .tls directory are still refused */
static uint64_t g_tls_used;                     /* TlsAlloc's bitmap of the 64 slots */
#define g_last_error (*(volatile DWORD *)(g_teb + TEB_LAST_ERROR))
#define PUT64(p, off, v) (*(uint64_t *)((p) + (off)) = (uint64_t)(v))

static uint64_t g_image_base;
static char *g_cmdline_a;
static WCHAR *g_cmdline_w;
static const char *g_image_path;

#define NHANDLES 64
static int g_fd[NHANDLES];             /* -1: a free slot */

static HANDLE handle_of(int slot) { return (HANDLE)(uintptr_t)(4 * (slot + 1)); }
static int fd_of(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v % 4 || v < 4 || v > 4 * NHANDLES) return -1;
    return g_fd[v / 4 - 1];
}
static DWORD from_errno(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EISDIR: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    default: return ERROR_INVALID_PARAMETER;
    }
}

void win32_init(uint64_t image_base, const char *image_path, const char *cmdline, uint64_t stack_lo, uint64_t stack_hi) {
    g_image_base = image_base;
    for (int i = 0; i < NHANDLES; i++) g_fd[i] = i < 3 ? i : -1;
    g_cmdline_a = strdup(cmdline);
    /* GetCommandLineW: the same string as UTF-16 (UTF-8 decoded; a byte that is not valid UTF-8 becomes U+FFFD) */
    size_t n = strlen(cmdline);
    g_cmdline_w = calloc(2 * n + 1, sizeof(WCHAR));
    const unsigned char *s = (const unsigned char *)cmdline;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = s[i], need = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
        if (need == 9 || i + need >= n + (need ? 0 : 1)) { g_cmdline_w[o++] = 0xFFFD; i++; continue; }
        c &= need ? 0x3F >> need : 0x7F;
        int bad = 0;
        for (uint32_t k = 1; k <= need; k++) { if ((s[i + k] & 0xC0) != 0x80) bad = 1; c = c << 6 | (s[i + k] & 0x3F); }
        if (bad) { g_cmdline_w[o++] = 0xFFFD; i++; continue; }
        i += need + 1;
        if (c >= 0x10000) { c -= 0x10000; g_cmdline_w[o++] = (WCHAR)(0xD800 | c >> 10); g_cmdline_w[o++] = (WCHAR)(0xDC00 | (c & 0x3FF)); }
        else g_cmdline_w[o++] = (WCHAR)c;
    }
    /* the TEB and the PEB */
    PUT64(g_teb, TEB_STACK_BASE, stack_hi);
    PUT64(g_teb, TEB_STACK_LIMIT, stack_lo);
    PUT64(g_teb, TEB_SELF, g_teb);
    PUT64(g_teb, TEB_PROCESS_ID, getpid());
    PUT64(g_teb, TEB_THREAD_ID, gettid());
    PUT64(g_teb, TEB_TLS_POINTER, g_tls_array);
    PUT64(g_teb, TEB_PEB, g_peb);
    g_peb[PEB_BEING_DEBUGGED] = 0;
    PUT64(g_peb, PEB_IMAGE_BASE, image_base);
    PUT64(g_peb, PEB_PARAMETERS, g_params);
    /* UNICODE_STRING: Length and MaximumLength in bytes (16 bits each), the buffer pointer at +8 */
    size_t wlen = 0;
    while (g_cmdline_w[wlen]) wlen++;
    if (wlen > 32766) wlen = 32766;
    *(uint16_t *)(g_params + PARAMS_COMMAND_LINE) = (uint16_t)(2 * wlen);
    *(uint16_t *)(g_params + PARAMS_COMMAND_LINE + 2) = (uint16_t)(2 * wlen + 2);
    PUT64(g_params, PARAMS_COMMAND_LINE + 8, g_cmdline_w);
    size_t plen = 0;                                /* ImagePathName: the program's own path, the first argument as given */
    const char *path = image_path;
    WCHAR *wpath = calloc(strlen(path) + 1, sizeof(WCHAR));
    for (; path[plen] && plen < 32766; plen++) wpath[plen] = (unsigned char)path[plen];
    *(uint16_t *)(g_params + PARAMS_IMAGE_PATH) = (uint16_t)(2 * plen);
    *(uint16_t *)(g_params + PARAMS_IMAGE_PATH + 2) = (uint16_t)(2 * plen + 2);
    PUT64(g_params, PARAMS_IMAGE_PATH + 8, wpath);
    g_image_path = image_path;
}

void *win32_teb(void) { return g_teb; }

/* ---------------------------------------------------------------- W0: process, standard handles, errors */

static WINAPI void ExitProcess(DWORD code) {
    fflush(NULL);
    _exit((int)code);
}

static WINAPI HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE) return handle_of(0);
    if (which == STD_OUTPUT_HANDLE) return handle_of(1);
    if (which == STD_ERROR_HANDLE) return handle_of(2);
    g_last_error = ERROR_INVALID_HANDLE;
    return INVALID_HANDLE_VALUE;
}

static WINAPI BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    int fd = fd_of(h);
    if (written) *written = 0;
    if (fd < 0) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    if (overlapped) { g_last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    DWORD done = 0;
    while (done < n) {
        long r = write(fd, (const char *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; g_last_error = from_errno(errno); if (written) *written = done; return FALSE; }
        done += (DWORD)r;
    }
    if (written) *written = done;
    return TRUE;
}

static WINAPI BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *reserved) {
    int fd = fd_of(h);
    if (fd < 0 || !isatty(fd)) { if (written) *written = 0; g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return WriteFile(h, buf, n, written, NULL);
}

static WINAPI BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *overlapped) {
    int fd = fd_of(h);
    if (got) *got = 0;
    if (fd < 0) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    if (overlapped) { g_last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    long r;
    do r = read(fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) { g_last_error = from_errno(errno); return FALSE; }
    if (got) *got = (DWORD)r;                  /* 0 at the end of the file, and TRUE: that is how a synchronous ReadFile reports end of file */
    return TRUE;
}

static WINAPI DWORD GetLastError(void) { return g_last_error; }
static WINAPI void SetLastError(DWORD e) { g_last_error = e; }

/* ---------------------------------------------------------------- W1: command line, module, heap, memory, environment, time */

static WINAPI char *GetCommandLineA(void) { return g_cmdline_a; }
static WINAPI WCHAR *GetCommandLineW(void) { return g_cmdline_w; }

static WINAPI HANDLE GetModuleHandleA(const char *name) {
    if (!name) return (HANDLE)(uintptr_t)g_image_base;
    g_last_error = ERROR_MOD_NOT_FOUND;
    return NULL;
}
static WINAPI HANDLE GetModuleHandleW(const WCHAR *name) {
    if (!name) return (HANDLE)(uintptr_t)g_image_base;
    g_last_error = ERROR_MOD_NOT_FOUND;
    return NULL;
}

/* The process heap: libc's malloc with a 16-byte header in front of every block that keeps its size (HeapSize answers it); blocks stay 16-byte aligned
 * as the x64 Windows heap's are. One heap: GetProcessHeap's handle is a constant. */
#define HEAP_HANDLE ((HANDLE)(uintptr_t)0x4EA90000)
#define HDR 16

static WINAPI HANDLE GetProcessHeap(void) { return HEAP_HANDLE; }

static WINAPI void *HeapAlloc(HANDLE heap, DWORD flags, size_t n) {
    if (heap != HEAP_HANDLE || n > ((size_t)1 << 40)) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    uint8_t *p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, n + HDR) : malloc(n + HDR);
    if (!p) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    *(size_t *)p = n;
    return p + HDR;
}

static WINAPI BOOL HeapFree(HANDLE heap, DWORD flags, void *mem) {
    if (heap != HEAP_HANDLE) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    if (mem) free((uint8_t *)mem - HDR);
    return TRUE;
}

static WINAPI size_t HeapSize(HANDLE heap, DWORD flags, const void *mem) {
    if (heap != HEAP_HANDLE || !mem) { g_last_error = ERROR_INVALID_PARAMETER; return (size_t)-1; }
    return *(const size_t *)((const uint8_t *)mem - HDR);
}

static WINAPI void *HeapReAlloc(HANDLE heap, DWORD flags, void *mem, size_t n) {
    if (heap != HEAP_HANDLE || !mem || n > ((size_t)1 << 40)) { g_last_error = ERROR_INVALID_PARAMETER; return NULL; }
    uint8_t *old = (uint8_t *)mem - HDR;
    size_t was = *(size_t *)old;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n > was) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }      /* the block cannot grow where it is: the documented failure */
        *(size_t *)old = n;
        return mem;
    }
    uint8_t *p = realloc(old, n + HDR);
    if (!p) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    if ((flags & HEAP_ZERO_MEMORY) && n > was) memset(p + HDR + was, 0, n - was);
    *(size_t *)p = n;
    return p + HDR;
}

/* VirtualAlloc over the native mmap: a reservation and its commit are the same mapping here (the pages are zero and arrive on first touch either way).
 * VirtualFree(MEM_RELEASE) needs the size of the whole region, so the regions are remembered. */
#define NREGIONS 256
static struct { uint64_t base, size; } g_region[NREGIONS];

static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE;
    case 0x02: return PROT_READ;
    case 0x04: case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC;
    case 0x20: return PROT_READ | PROT_EXEC;
    case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return -1;
    }
}

static WINAPI void *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect) {
    int prot = prot_of(protect);
    if (!size || prot < 0 || !(type & (MEM_COMMIT | MEM_RESERVE)) || (type & ~(MEM_COMMIT | MEM_RESERVE))) { g_last_error = ERROR_INVALID_PARAMETER; return NULL; }
    if (addr) {
        /* inside a region we handed out: committing what is already there succeeds (the pages exist); a new placement is not offered */
        for (int i = 0; i < NREGIONS; i++)
            if (g_region[i].size && (uint64_t)addr >= g_region[i].base && (uint64_t)addr + size <= g_region[i].base + g_region[i].size && (type & MEM_COMMIT)) {
                uint64_t lo = (uint64_t)addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
                mprotect((void *)lo, hi - lo, prot);
                return (void *)lo;
            }
        g_last_error = ERROR_INVALID_ADDRESS;
        return NULL;
    }
    int slot = -1;
    for (int i = 0; i < NREGIONS && slot < 0; i++) if (!g_region[i].size) slot = i;
    if (slot < 0) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    g_region[slot].base = (uint64_t)p;
    g_region[slot].size = size;
    return p;
}

static WINAPI BOOL VirtualFree(void *addr, size_t size, DWORD type) {
    for (int i = 0; i < NREGIONS; i++) {
        if (!g_region[i].size || (uint64_t)addr < g_region[i].base || (uint64_t)addr >= g_region[i].base + g_region[i].size) continue;
        if (type == MEM_RELEASE) {
            if (size || (uint64_t)addr != g_region[i].base) break;          /* MEM_RELEASE takes the base the allocation returned and a size of 0 */
            munmap(addr, g_region[i].size);
            g_region[i].size = 0;
            return TRUE;
        }
        if (type == MEM_DECOMMIT) {
            uint64_t lo = (uint64_t)addr & ~0xFFFull, hi = size ? ((uint64_t)addr + size + 0xFFF) & ~0xFFFull : g_region[i].base + g_region[i].size;
            if (hi > g_region[i].base + g_region[i].size) break;
            memset((void *)lo, 0, hi - lo);         /* decommitted pages come back zero when they are committed again (no madvise here) */
            return TRUE;
        }
        break;
    }
    g_last_error = ERROR_INVALID_PARAMETER;
    return FALSE;
}

static WINAPI DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD size) {
    const char *v = name ? getenv(name) : NULL;
    if (!v) { g_last_error = ERROR_ENVVAR_NOT_FOUND; return 0; }
    size_t n = strlen(v);
    if (!buf || n + 1 > size) return (DWORD)(n + 1);       /* too small: the size needed, counting the terminating NUL */
    memcpy(buf, v, n + 1);
    return (DWORD)n;                                        /* fits: the length without the NUL */
}

static WINAPI int lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static WINAPI uint64_t GetTickCount64(void) { return mono_ns() / 1000000; }
static WINAPI DWORD GetTickCount(void) { return (DWORD)(mono_ns() / 1000000); }
static WINAPI BOOL QueryPerformanceFrequency(int64_t *f) { *f = 10000000; return TRUE; }        /* 10 MHz, 100 ns ticks */
static WINAPI BOOL QueryPerformanceCounter(int64_t *c) { *c = (int64_t)(mono_ns() / 100); return TRUE; }
static WINAPI DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static WINAPI void Sleep(DWORD ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static WINAPI DWORD GetFileType(HANDLE h) {
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { g_last_error = ERROR_INVALID_HANDLE; return FILE_TYPE_UNKNOWN; }
    g_last_error = ERROR_SUCCESS;                           /* FILE_TYPE_UNKNOWN with NO_ERROR means "known handle, unknown type" */
    if (isatty(fd)) return FILE_TYPE_CHAR;
    if (S_ISREG(st.st_mode)) return FILE_TYPE_DISK;
    if (S_ISFIFO(st.st_mode)) return FILE_TYPE_PIPE;
    return FILE_TYPE_UNKNOWN;
}

static WINAPI BOOL GetConsoleMode(HANDLE h, DWORD *mode) {
    int fd = fd_of(h);
    if (fd < 0 || !isatty(fd)) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    if (mode) *mode = fd == 0 ? 0x0007 : 0x0003;            /* input: processed, line, echo; output: processed, wrap at end of line */
    return TRUE;
}
static WINAPI BOOL SetConsoleMode(HANDLE h, DWORD mode) {
    int fd = fd_of(h);
    if (fd < 0 || !isatty(fd)) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return TRUE;                                            /* accepted and not acted on: the terminal keeps its own mode */
}

/* ---------------------------------------------------------------- files over the native open/read/write */

static int access_ok(const char *name) {
    struct stat st;
    return stat(name, &st) == 0;
}

static WINAPI HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD attrs, HANDLE tmpl) {
    if (!name || disp < CREATE_NEW || disp > TRUNCATE_EXISTING) { g_last_error = ERROR_INVALID_PARAMETER; return INVALID_HANDLE_VALUE; }
    int fl = (access & GENERIC_READ) && (access & GENERIC_WRITE) ? O_RDWR : (access & GENERIC_WRITE) ? O_WRONLY : O_RDONLY;
    int existed = access_ok(name);
    switch (disp) {
    case CREATE_NEW: fl |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: fl |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: fl |= O_CREAT; break;
    case TRUNCATE_EXISTING: fl |= O_TRUNC; break;
    }
    int slot = -1;
    for (int i = 3; i < NHANDLES && slot < 0; i++) if (g_fd[i] < 0) slot = i;
    if (slot < 0) { g_last_error = ERROR_TOO_MANY_OPEN_FILES; return INVALID_HANDLE_VALUE; }
    int fd = open(name, fl, 0644);
    if (fd < 0) { g_last_error = from_errno(errno); return INVALID_HANDLE_VALUE; }
    g_fd[slot] = fd;
    /* CREATE_ALWAYS and OPEN_ALWAYS succeed on an existing file and say so through the last error */
    g_last_error = (existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS;
    return handle_of(slot);
}

static WINAPI BOOL CloseHandle(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (fd_of(h) < 0) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    int slot = (int)(v / 4 - 1);
    if (slot >= 3) close(g_fd[slot]);                       /* the standard handles stay open: the native process still owns those descriptors */
    g_fd[slot] = -1;
    return TRUE;
}

static WINAPI DWORD GetFileSize(HANDLE h, DWORD *high) {
    int fd = fd_of(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { g_last_error = ERROR_INVALID_HANDLE; return INVALID_FILE_SIZE; }
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    g_last_error = ERROR_SUCCESS;
    return (DWORD)st.st_size;
}

/* ---------------------------------------------------------------- W2: threads' identity, TLS slots, critical sections, small process queries */

#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define ERROR_NO_MORE_ITEMS 259

static WINAPI DWORD TlsAlloc(void) {
    for (DWORD i = 0; i < 64; i++)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            PUT64(g_teb, TEB_TLS_SLOTS + 8 * i, 0);        /* a new slot reads 0 */
            return i;
        }
    g_last_error = ERROR_NO_MORE_ITEMS;
    return TLS_OUT_OF_INDEXES;
}
static WINAPI BOOL TlsFree(DWORD i) {
    if (i >= 64 || !(g_tls_used & (1ull << i))) { g_last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    g_tls_used &= ~(1ull << i);
    return TRUE;
}
static WINAPI void *TlsGetValue(DWORD i) {
    if (i >= 64) { g_last_error = ERROR_INVALID_PARAMETER; return NULL; }
    g_last_error = ERROR_SUCCESS;                         /* documented: TlsGetValue clears the last error on success, so a stored 0 can be told apart */
    return *(void **)(g_teb + TEB_TLS_SLOTS + 8 * i);
}
static WINAPI BOOL TlsSetValue(DWORD i, void *v) {
    if (i >= 64) { g_last_error = ERROR_INVALID_PARAMETER; return FALSE; }
    PUT64(g_teb, TEB_TLS_SLOTS + 8 * i, v);
    return TRUE;
}

static WINAPI DWORD GetCurrentThreadId(void) { return (DWORD)*(uint64_t *)(g_teb + TEB_THREAD_ID); }
static WINAPI HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }         /* the documented pseudo-handles */
static WINAPI HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static WINAPI BOOL IsDebuggerPresent(void) { return g_peb[PEB_BEING_DEBUGGED] != 0; }

/* CRITICAL_SECTION (40 bytes on x64): winrun runs one thread, so entering is counting. The fields used are LockCount (+8), RecursionCount (+12) and
 * OwningThread (+16), with the meanings the structure's names give them. */
static WINAPI void InitializeCriticalSection(void *cs) { memset(cs, 0, 40); *(int32_t *)((uint8_t *)cs + 8) = -1; }
static WINAPI BOOL InitializeCriticalSectionAndSpinCount(void *cs, DWORD spin) { InitializeCriticalSection(cs); return TRUE; }
static WINAPI void EnterCriticalSection(void *cs) {
    uint8_t *c = cs;
    (*(int32_t *)(c + 8))++;
    (*(int32_t *)(c + 12))++;
    *(uint64_t *)(c + 16) = GetCurrentThreadId();
}
static WINAPI BOOL TryEnterCriticalSection(void *cs) { EnterCriticalSection(cs); return TRUE; }
static WINAPI void LeaveCriticalSection(void *cs) {
    uint8_t *c = cs;
    (*(int32_t *)(c + 8))--;
    if (--*(int32_t *)(c + 12) <= 0) { *(int32_t *)(c + 12) = 0; *(uint64_t *)(c + 16) = 0; }
}
static WINAPI void DeleteCriticalSection(void *cs) { memset(cs, 0, 40); }

static WINAPI DWORD GetModuleFileNameA(HANDLE module, char *buf, DWORD size) {
    if (module && module != (HANDLE)(uintptr_t)g_image_base) { g_last_error = ERROR_MOD_NOT_FOUND; return 0; }
    size_t n = strlen(g_image_path);
    if (!size) { g_last_error = 122; return 0; }
    if (n >= size) { memcpy(buf, g_image_path, size - 1); buf[size - 1] = 0; g_last_error = 122; return size; }       /* truncated: ERROR_INSUFFICIENT_BUFFER */
    memcpy(buf, g_image_path, n + 1);
    g_last_error = ERROR_SUCCESS;
    return (DWORD)n;
}

/* FILETIME: 100 ns intervals since 1601-01-01 UTC; 11644473600 seconds lie between that and the Unix epoch. */
static WINAPI void GetSystemTimeAsFileTime(uint64_t *ft) {
    struct timespec ts;
    clock_gettime(0 /* CLOCK_REALTIME */, &ts);
    *ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}

static WINAPI BOOL SetConsoleCtrlHandler(void *handler, BOOL add) { return TRUE; }       /* accepted; Ctrl+C still ends the process the native way */
static WINAPI DWORD GetConsoleOutputCP(void) { return 65001; }                              /* UTF-8: what the terminal shows */
static WINAPI DWORD GetConsoleCP(void) { return 65001; }
static WINAPI BOOL SetConsoleOutputCP(DWORD cp) { if (cp != 65001) { g_last_error = ERROR_INVALID_PARAMETER; return FALSE; } return TRUE; }
static WINAPI BOOL FlushFileBuffers(HANDLE h) { if (fd_of(h) < 0) { g_last_error = ERROR_INVALID_HANDLE; return FALSE; } return TRUE; }

/* ---------------------------------------------------------------- files and directories by name, file positions, UTF-8 <-> UTF-16 */

#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu

static WINAPI DWORD SetFilePointer(HANDLE h, int32_t lo, int32_t *hi, DWORD method) {
    int fd = fd_of(h);
    if (fd < 0 || method > 2) { g_last_error = fd < 0 ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER; return INVALID_SET_FILE_POINTER; }
    int64_t dist = hi ? (int64_t)((uint64_t)(uint32_t)*hi << 32 | (uint32_t)lo) : lo;   /* without hi, lo is a signed 32-bit distance */
    long r = lseek(fd, dist, (int)method);                  /* FILE_BEGIN, FILE_CURRENT, FILE_END are SEEK_SET, SEEK_CUR, SEEK_END */
    if (r < 0) { g_last_error = errno == EINVAL ? 131 /* ERROR_NEGATIVE_SEEK */ : from_errno(errno); return INVALID_SET_FILE_POINTER; }
    if (hi) *hi = (int32_t)((uint64_t)r >> 32);
    g_last_error = ERROR_SUCCESS;
    return (DWORD)r;
}
static WINAPI BOOL SetFilePointerEx(HANDLE h, int64_t dist, int64_t *newpos, DWORD method) {
    int fd = fd_of(h);
    if (fd < 0 || method > 2) { g_last_error = fd < 0 ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER; return FALSE; }
    long r = lseek(fd, dist, (int)method);
    if (r < 0) { g_last_error = errno == EINVAL ? 131 : from_errno(errno); return FALSE; }
    if (newpos) *newpos = r;
    return TRUE;
}

static WINAPI DWORD GetFileAttributesA(const char *name) {
    struct stat st;
    if (!name || stat(name, &st) < 0) { g_last_error = name ? from_errno(errno) : ERROR_INVALID_PARAMETER; return INVALID_FILE_ATTRIBUTES; }
    /* FILE_ATTRIBUTE_DIRECTORY, or FILE_ATTRIBUTE_ARCHIVE (what Windows reports for an ordinary new file), plus READONLY without a write bit */
    return (S_ISDIR(st.st_mode) ? 0x10 : 0x20) | (st.st_mode & 0200 ? 0 : 0x01);
}
static WINAPI BOOL DeleteFileA(const char *name) {
    if (unlink(name) < 0) { g_last_error = from_errno(errno); return FALSE; }
    return TRUE;
}
static WINAPI BOOL CreateDirectoryA(const char *name, void *sa) {
    if (mkdir(name, 0755) < 0) { g_last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : from_errno(errno); return FALSE; }
    return TRUE;
}
static WINAPI BOOL RemoveDirectoryA(const char *name) {
    if (rmdir(name) < 0) { g_last_error = from_errno(errno); return FALSE; }
    return TRUE;
}
static WINAPI DWORD GetCurrentDirectoryA(DWORD size, char *buf) {
    char tmp[4096];
    if (!getcwd(tmp, sizeof tmp)) { g_last_error = from_errno(errno); return 0; }
    size_t n = strlen(tmp);
    if (!buf || n + 1 > size) return (DWORD)(n + 1);       /* too small: the size needed with the NUL */
    memcpy(buf, tmp, n + 1);
    return (DWORD)n;
}

/* MultiByteToWideChar and WideCharToMultiByte for CP_UTF8 (65001) and, as UTF-8 too, CP_ACP (0): a length of -1 means "up to and with the NUL"; an output size
 * of 0 asks for the size needed; too small an output is 0 and ERROR_INSUFFICIENT_BUFFER. Invalid UTF-8 becomes U+FFFD. */
static WINAPI int MultiByteToWideChar(DWORD cp, DWORD flags, const char *src, int srclen, WCHAR *dst, int dstlen) {
    if (!src || (cp != 65001 && cp != 0) || dstlen < 0) { g_last_error = ERROR_INVALID_PARAMETER; return 0; }
    size_t n = srclen < 0 ? strlen(src) + 1 : (size_t)srclen;
    const unsigned char *s = (const unsigned char *)src;
    int o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = s[i], need = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
        int bad = need == 9 || i + need >= n + (need ? 0 : 1);
        if (!bad) {
            c &= need ? 0x3F >> need : 0x7F;
            for (uint32_t k = 1; k <= need; k++) { if ((s[i + k] & 0xC0) != 0x80) bad = 1; c = c << 6 | (s[i + k] & 0x3F); }
        }
        if (bad) { c = 0xFFFD; i++; } else i += need + 1;
        int units = c >= 0x10000 ? 2 : 1;
        if (dstlen) {
            if (o + units > dstlen) { g_last_error = 122; return 0; }
            if (units == 2) { dst[o] = (WCHAR)(0xD800 | (c - 0x10000) >> 10); dst[o + 1] = (WCHAR)(0xDC00 | ((c - 0x10000) & 0x3FF)); }
            else dst[o] = (WCHAR)c;
        }
        o += units;
    }
    return o;
}
static WINAPI int WideCharToMultiByte(DWORD cp, DWORD flags, const WCHAR *src, int srclen, char *dst, int dstlen, const char *defchar, BOOL *useddef) {
    if (!src || (cp != 65001 && cp != 0) || dstlen < 0) { g_last_error = ERROR_INVALID_PARAMETER; return 0; }
    size_t n = 0;
    if (srclen < 0) { while (src[n]) n++; n++; } else n = (size_t)srclen;
    if (useddef) *useddef = FALSE;
    int o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00); i++; }
        else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;                       /* a lone surrogate */
        unsigned char b[4];
        int k = c < 0x80 ? (b[0] = (unsigned char)c, 1) : c < 0x800 ? (b[0] = (unsigned char)(0xC0 | c >> 6), b[1] = (unsigned char)(0x80 | (c & 0x3F)), 2)
              : c < 0x10000 ? (b[0] = (unsigned char)(0xE0 | c >> 12), b[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), b[2] = (unsigned char)(0x80 | (c & 0x3F)), 3)
              : (b[0] = (unsigned char)(0xF0 | c >> 18), b[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F)), b[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), b[3] = (unsigned char)(0x80 | (c & 0x3F)), 4);
        if (dstlen) {
            if (o + k > dstlen) { g_last_error = 122; return 0; }
            memcpy(dst + o, b, (size_t)k);
        }
        o += k;
    }
    return o;
}

#define E(f) { #f, (void *)f }
static const Win32Export kernel32[] = {
    E(ExitProcess), E(GetStdHandle), E(WriteFile), E(WriteConsoleA), E(ReadFile), E(GetLastError), E(SetLastError),
    E(GetCommandLineA), E(GetCommandLineW), E(GetModuleHandleA), E(GetModuleHandleW),
    E(GetProcessHeap), E(HeapAlloc), E(HeapFree), E(HeapReAlloc), E(HeapSize), E(VirtualAlloc), E(VirtualFree),
    E(GetEnvironmentVariableA), E(lstrlenA), E(GetTickCount), E(GetTickCount64), E(QueryPerformanceCounter), E(QueryPerformanceFrequency),
    E(GetCurrentProcessId), E(Sleep), E(GetFileType), E(GetConsoleMode), E(SetConsoleMode),
    E(CreateFileA), E(CloseHandle), E(GetFileSize),
    E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue), E(GetCurrentThreadId), E(GetCurrentProcess), E(GetCurrentThread), E(IsDebuggerPresent),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount), E(EnterCriticalSection), E(TryEnterCriticalSection), E(LeaveCriticalSection),
    E(DeleteCriticalSection), E(GetModuleFileNameA), E(GetSystemTimeAsFileTime), E(SetConsoleCtrlHandler), E(GetConsoleOutputCP), E(GetConsoleCP),
    E(SetConsoleOutputCP), E(FlushFileBuffers),
    E(SetFilePointer), E(SetFilePointerEx), E(GetFileAttributesA), E(DeleteFileA), E(CreateDirectoryA), E(RemoveDirectoryA), E(GetCurrentDirectoryA),
    E(MultiByteToWideChar), E(WideCharToMultiByte),
    { NULL, NULL },
};

const Win32Dll win32_dlls[] = {
    { "kernel32.dll", kernel32 },
    { NULL, NULL },
};
