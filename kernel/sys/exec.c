/* execve: ELF64 loading into a fresh address space, argument/environment stack, shebang scripts. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "elf.h"

#define AT_NULL   0
#define AT_PAGESZ 6

static int read_exact(File *f, u64 off, void *buf, size_t n) {
    f->off = off;
    size_t done = 0;
    while (done < n) {
        ssize_t r = vfs_read(f, (u8 *)buf + done, n - done);
        if (r < 0) return (int)r;
        if (r == 0) return -ENOEXEC;
        done += (size_t)r;
    }
    return 0;
}

static int load_elf(File *f, AddrSpace *as, u64 *entry_out) {
    Elf64_Ehdr eh;
    if (read_exact(f, 0, &eh, sizeof eh)) return -ENOEXEC;
    if (!elf64_check(&eh) || eh.e_type != ET_EXEC || eh.e_phnum == 0 || eh.e_phnum > 32) return -ENOEXEC;

    Elf64_Phdr *ph = kmalloc(eh.e_phnum * sizeof *ph);
    if (!ph) return -ENOMEM;
    int rc = read_exact(f, eh.e_phoff, ph, eh.e_phnum * sizeof *ph);
    u64 hi = 0;
    for (int i = 0; !rc && i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        u64 vaddr = ph[i].p_vaddr, memsz = ph[i].p_memsz, filesz = ph[i].p_filesz;
        if (vaddr < 0x1000 || vaddr >= USER_TOP || memsz > USER_TOP - vaddr || filesz > memsz) { rc = -ENOEXEC; break; }
        u64 start = ALIGN_DOWN(vaddr, PAGE_SIZE), end = ALIGN_UP(vaddr + memsz, PAGE_SIZE);
        u32 prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) | ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
                   ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
        if (!prot) prot = PROT_READ;
        rc = uvm_map(as, start, end - start, prot, 0);
        if (rc) { rc = rc == -EEXIST ? -ENOEXEC : rc; break; }
        rc = uvm_populate(as, start, end - start);
        if (rc) break;
        u64 va = vaddr, remaining = filesz, foff = ph[i].p_offset;
        while (remaining && !rc) {
            u64 chunk = MIN(remaining, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
            paddr_t pa = vmm_translate(as->pml4, va, NULL);
            rc = pa ? read_exact(f, foff, phys_to_virt(pa), (size_t)chunk) : -EFAULT;
            va += chunk;
            foff += chunk;
            remaining -= chunk;
        }
        if (end > hi) hi = end;
    }
    kfree(ph);
    if (rc) return rc;
    if (eh.e_entry < 0x1000 || eh.e_entry >= USER_TOP) return -ENOEXEC;
    as->brk_start = as->brk = hi;
    *entry_out = eh.e_entry;
    return 0;
}

static int build_stack(AddrSpace *as, char *const argv[], char *const envp[], u64 *sp_out, int *argc_out,
                       u64 *argv_out, u64 *envp_out) {
    int rc = uvm_map(as, USER_STACK_TOP - USER_STACK_MAX, USER_STACK_MAX, PROT_READ | PROT_WRITE, VMA_STACK);
    if (!rc) rc = uvm_populate(as, USER_STACK_TOP - USER_STACK_INIT, USER_STACK_INIT);
    if (rc) return rc;

    int argc = 0, envc = 0;
    size_t bytes = 0;
    while (argv && argv[argc]) bytes += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) bytes += strlen(envp[envc++]) + 1;
    if (bytes > USER_STACK_INIT / 2) return -E2BIG;

    u64 *ptrs = kmalloc((size_t)(argc + envc + 2) * sizeof(u64));
    if (!ptrs) return -ENOMEM;
    u64 sp = USER_STACK_TOP;
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        uvm_write(as, sp, envp[i], len);
        ptrs[argc + 1 + i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        uvm_write(as, sp, argv[i], len);
        ptrs[i] = sp;
    }
    sp &= ~0xFUL;

    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + 4;
    sp = (sp - words * 8) & ~0xFUL;
    u64 *img = kzalloc(words * 8);
    if (!img) { kfree(ptrs); return -ENOMEM; }
    size_t w = 0;
    img[w++] = (u64)argc;
    for (int i = 0; i < argc; i++) img[w++] = ptrs[i];
    img[w++] = 0;
    for (int i = 0; i < envc; i++) img[w++] = ptrs[argc + 1 + i];
    img[w++] = 0;
    img[w++] = AT_PAGESZ;
    img[w++] = PAGE_SIZE;
    img[w++] = AT_NULL;
    img[w++] = 0;
    rc = uvm_write(as, sp, img, words * 8);
    kfree(img);
    kfree(ptrs);
    *sp_out = sp;
    *argc_out = argc;
    *argv_out = sp + 8;
    *envp_out = sp + 8 + (u64)(argc + 1) * 8;
    return rc;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int exec_depth;

int process_exec(Process *p, const char *path, char *const argv[], char *const envp[], TrapFrame *tf) {
    File *f;
    int rc = vfs_open(path, O_RDONLY, 0, &p->cred, p->cwd, &f);
    if (rc) return rc;
    if (!S_ISREG(f->vn->mode)) { file_put(f); return -EACCES; }
    if (vfs_access(f->vn, &p->cred, X_OK)) { file_put(f); return -EACCES; }

    char head[160];
    memset(head, 0, sizeof head);
    ssize_t n = (f->off = 0, vfs_read(f, head, sizeof head - 1));
    if (n >= 4 && head[0] == '#' && head[1] == '!') {
        /* Script: re-run with the interpreter. */
        if (exec_depth >= 3) { file_put(f); return -ELOOP; }
        file_put(f);
        char *line = head + 2;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        while (*line == ' ') line++;
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = 0; while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
        int argc = 0;
        while (argv && argv[argc]) argc++;
        char **nargv = kmalloc((size_t)(argc + 4) * sizeof(char *));
        if (!nargv) return -ENOMEM;
        int k = 0;
        nargv[k++] = line;
        if (arg) nargv[k++] = arg;
        nargv[k++] = (char *)path;
        for (int i = 1; i < argc; i++) nargv[k++] = argv[i];
        nargv[k] = NULL;
        exec_depth++;
        rc = process_exec(p, line, nargv, envp, tf);
        exec_depth--;
        kfree(nargv);
        return rc;
    }

    AddrSpace nas;
    rc = uvm_new(&nas);
    if (rc) { file_put(f); return rc; }
    u64 entry, sp, argv_u, envp_u;
    int argc;
    u32 fmode = f->vn->mode, fuid = f->vn->uid, fgid = f->vn->gid;      /* for setuid/setgid programs */
    rc = load_elf(f, &nas, &entry);
    if (!rc) rc = build_stack(&nas, argv, envp, &sp, &argc, &argv_u, &envp_u);
    file_put(f);
    if (rc) { uvm_destroy(&nas); return rc; }

    /* Point of no return: swap address spaces. */
    if (fmode & S_ISUID) p->cred.uid = fuid;                          /* the effective id becomes the file's owner; the real one stays */
    if (fmode & S_ISGID) p->cred.gid = fgid;
    AddrSpace old = p->as;
    p->as = nas;
    current->pml4 = nas.pml4;
    vmm_switch(nas.pml4);
    uvm_destroy(&old);

    for (int i = 0; i < MAX_FD; i++)
        if (p->fds[i] && (p->fd_flags[i] & FD_CLOEXEC)) fd_close(p, i);
    for (int s = 0; s < NSIG; s++)
        if (p->sigact[s].handler != SIG_IGN) memset(&p->sigact[s], 0, sizeof p->sigact[s]);
    p->sig_pending = 0;
    strlcpy(p->name, base_name(argv && argv[0] ? argv[0] : path), sizeof p->name);
    p->cmdline[0] = 0;
    for (int i = 0; argv && argv[i]; i++) {
        if (i) strlcat(p->cmdline, " ", sizeof p->cmdline);
        strlcat(p->cmdline, argv[i], sizeof p->cmdline);
    }
    strlcpy(current->name, p->name, sizeof current->name);

    memset(tf, 0, sizeof *tf);
    tf->rip = entry;
    tf->rsp = sp;
    tf->cs = USER_CS;
    tf->ss = USER_DS;
    tf->rflags = RFLAGS_IF | 2;
    tf->rdi = (u64)argc;
    tf->rsi = argv_u;
    tf->rdx = envp_u;
    return 0;
}
