/*
 * The initial stack of a Linux program, as the System V x86-64 psABI describes it ("Process Initialization", the auxiliary vector as listed in getauxval(3)).
 * From the top of the stack down: the strings (random bytes, the platform name, the file name AT_EXECFN points at, the environment, the arguments), padding to
 * 16 bytes, then the words the stack pointer points into: argc, argv[0..argc-1], a null, envp[...], a null, the auxiliary vector (pairs of words) ending in
 * AT_NULL. The stack pointer is 16-byte aligned. A static C library finds its program headers (and so its thread-local storage template) through AT_PHDR,
 * its random canary through AT_RANDOM, and the page size, the user ids and the CPU feature word from the other entries.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "random.h"

/* auxiliary vector types (elf.h; getauxval(3)) */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31
#define AT_MINSIGSTKSZ 51                       /* Linux 5.14: the least a signal stack can be (the dynamic linker asks for it) */

#define LINUX_CLK_TCK 100
#define AUX_MAX       24                        /* entries (pairs) we may write */

static int put(AddrSpace *as, u64 *sp, const void *src, size_t n) {
    *sp -= n;
    return uvm_write(as, *sp, src, n);
}

int linux_build_stack(AddrSpace *as, const LinuxImage *img, char *const argv[], char *const envp[], const char *execfn, u64 *sp_out) {
    int rc = uvm_map(as, USER_STACK_TOP - USER_STACK_MAX, USER_STACK_MAX, PROT_READ | PROT_WRITE, VMA_STACK);
    if (!rc) rc = uvm_populate(as, USER_STACK_TOP - USER_STACK_INIT, USER_STACK_INIT);
    if (rc) return rc;

    static const char platform[] = "x86_64";
    int argc = 0, envc = 0;
    size_t bytes = strlen(execfn) + 1 + sizeof platform + 16;
    while (argv && argv[argc]) bytes += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) bytes += strlen(envp[envc++]) + 1;
    if (bytes > USER_STACK_INIT / 2) return -E2BIG;

    u64 *ptrs = kmalloc((size_t)(argc + envc + 2) * sizeof(u64));
    if (!ptrs) return -ENOMEM;

    u64 sp = USER_STACK_TOP;
    u8 rnd[16];
    random_fill(rnd, sizeof rnd);
    rc |= put(as, &sp, rnd, sizeof rnd);
    u64 random_u = sp;
    rc |= put(as, &sp, platform, sizeof platform);
    u64 platform_u = sp;
    rc |= put(as, &sp, execfn, strlen(execfn) + 1);
    u64 execfn_u = sp;
    for (int i = envc - 1; i >= 0; i--) {
        rc |= put(as, &sp, envp[i], strlen(envp[i]) + 1);
        ptrs[argc + 1 + i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        rc |= put(as, &sp, argv[i], strlen(argv[i]) + 1);
        ptrs[i] = sp;
    }
    sp &= ~0xFUL;

    u32 ca, cb, cc, cd;
    cpuid(1, 0, &ca, &cb, &cc, &cd);                    /* AT_HWCAP on x86 is the feature word CPUID leaf 1 returns in edx */
    u64 aux[2 * AUX_MAX];
    size_t na = 0;
#define AUX(type, value) do { aux[na++] = (type); aux[na++] = (u64)(value); } while (0)
    AUX(AT_PHDR, img->phdr);
    AUX(AT_PHENT, img->phent);
    AUX(AT_PHNUM, img->phnum);
    AUX(AT_PAGESZ, PAGE_SIZE);
    AUX(AT_BASE, img->interp_base);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, img->entry);
    AUX(AT_UID, current->proc->ruid);
    AUX(AT_EUID, img->euid);
    AUX(AT_GID, current->proc->rgid);
    AUX(AT_EGID, img->egid);
    AUX(AT_PLATFORM, platform_u);
    AUX(AT_HWCAP, cd);
    AUX(AT_CLKTCK, LINUX_CLK_TCK);
    AUX(AT_SECURE, img->secure ? 1 : 0);
    AUX(AT_RANDOM, random_u);
    AUX(AT_HWCAP2, 0);
    AUX(AT_EXECFN, execfn_u);
    AUX(AT_MINSIGSTKSZ, 2048);                          /* a handler's frame (rt_sigframe, the FXSAVE area, the red zone) takes about 1100 bytes here */
    AUX(AT_NULL, 0);
#undef AUX

    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + na;
    sp = (sp - words * 8) & ~0xFUL;
    u64 *image = kzalloc(words * 8);
    if (!image) { kfree(ptrs); return -ENOMEM; }
    size_t w = 0;
    image[w++] = (u64)argc;
    for (int i = 0; i < argc; i++) image[w++] = ptrs[i];
    image[w++] = 0;
    for (int i = 0; i < envc; i++) image[w++] = ptrs[argc + 1 + i];
    image[w++] = 0;
    for (size_t i = 0; i < na; i++) image[w++] = aux[i];
    rc |= uvm_write(as, sp, image, words * 8);
    kfree(image);
    kfree(ptrs);
    *sp_out = sp;
    return rc ? -EFAULT : 0;
}
