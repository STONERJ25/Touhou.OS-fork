/*
 * Updates pushed over the network, loader side (docs/UPDATE.md, "The fallback in the loader").
 *
 * The `update` tool keeps the previous kernel and initrd next to the new ones (kernel.old, initrd.old, and cmdline.old when the update
 * changed the command line) and writes update.try: the new files are on trial. The loader counts the starts under that trial in a
 * non-volatile variable. The system proves itself by deleting update.try ("update confirm", run by whoever pushed the update once they can
 * reach the machine again, or at the console). Until then each start counts: the first TRIES of them use the new files, the next ones the
 * previous set, with "update.fellback" on the command line so that the system can say so. A kernel that panics, hangs or never gets as far
 * as its network is therefore replaced by the working one after at most TRIES restarts, with nobody at the machine.
 *
 * The same fallback is used when kernel.elf is missing or damaged and kernel.old is there: a power cut between the two renames of an
 * update leaves exactly that.
 */
#include "loader.h"
#include "../shared/elf.h"

#define TRIES 2

typedef EFI_STATUS (EFIAPI *GetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data);
typedef EFI_STATUS (EFIAPI *SetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 attr, UINTN size, void *data);

static EFI_GUID tryGuid     = { 0x7e4d6a10, 0x5b3c, 0x4c1f, { 0x9a, 0x21, 0x6f, 0x0d, 0x2b, 0x8c, 0x31, 0x55 } };
static EFI_GUID fileInfoGuid = EFI_FILE_INFO_ID;

/* Reads up to cap bytes from the start of a file; *size is the whole file's size. 0, or -1 when there is no such file. */
static int read_prefix(EFI_FILE_PROTOCOL *root, const char *ascii, void *buf, UINTN cap, UINT64 *size) {
    CHAR16 name[32];
    UINTN i = 0;
    for (; ascii[i] && i < 30; i++) name[i] = (CHAR16)(UINT8)ascii[i];
    name[i] = 0;
    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(root->Open(root, &f, name, EFI_FILE_MODE_READ, 0))) return -1;
    UINT64 info[96];
    UINTN info_size = sizeof info;
    if (EFI_ERROR(f->GetInfo(f, &fileInfoGuid, &info_size, info))) { f->Close(f); return -1; }
    *size = ((EFI_FILE_INFO *)info)->FileSize;
    UINTN want = *size < cap ? (UINTN)*size : cap;
    UINTN got = want;
    if (want && (EFI_ERROR(f->Read(f, &got, buf)) || got != want)) { f->Close(f); return -1; }
    f->Close(f);
    return 0;
}

static int exists(EFI_FILE_PROTOCOL *root, const char *name) {
    UINT8 b[1];
    UINT64 size;
    return read_prefix(root, name, b, 0, &size) == 0 && size > 0;
}

/* A kernel image that can be started: an x86-64 ELF executable whose program headers lie inside the file, whose loadable segments are all there (a file cut short
 * by a power cut or a bad copy is found out here, where the fallback is decided, and not after load_kernel() has begun) and whose entry point is in one of them. */
static int kernel_ok(EFI_FILE_PROTOCOL *root, const char *name) {
    UINT64 buf[256];                                          /* the first 2 KiB, 8-byte aligned: the ELF header and, right behind it, the program header table */
    UINT64 size;
    if (read_prefix(root, name, buf, sizeof buf, &size) != 0 || size < 4096) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)buf;
    if (!elf64_check(eh) || eh->e_type != ET_EXEC) return 0;
    UINT64 have = size < sizeof buf ? size : sizeof buf;
    if (eh->e_phnum == 0 || eh->e_phnum > 32 || eh->e_phoff < sizeof *eh || eh->e_phoff + (UINT64)eh->e_phnum * sizeof(Elf64_Phdr) > have) return 0;
    int loads = 0, entry_in = 0;
    for (UINT32 i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr p;
        memcpy(&p, (const UINT8 *)buf + eh->e_phoff + (UINT64)i * sizeof p, sizeof p);
        if (p.p_type != PT_LOAD) continue;
        if (p.p_filesz > p.p_memsz || p.p_offset > size || p.p_filesz > size - p.p_offset) return 0;
        loads++;
        if (eh->e_entry >= p.p_vaddr && eh->e_entry - p.p_vaddr < p.p_memsz) entry_in = 1;
    }
    return loads > 0 && entry_in;
}

static UINT32 fnv1a(const UINT8 *p, UINTN n) {
    UINT32 h = 2166136261u;
    for (UINTN i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* How many times the loader has started under the trial that update.try (hash h) describes, this start included; 0 if the variable store
 * refuses. One variable for all updates, holding the hash and the count: a new update starts again at 1. */
static UINT32 count_start(UINT32 h) {
    CHAR16 name[] = { 'T', 'o', 'u', 'h', 'o', 'u', 'U', 'p', 'd', 'a', 't', 'e', 'T', 'r', 'y', 0 };
    struct { UINT32 hash, count; } v = { 0, 0 };
    UINT32 attr = 0;
    UINTN size = sizeof v;
    EFI_RUNTIME_SERVICES *rt = g_st->RuntimeServices;
    if (EFI_ERROR(((GetVariableFn)rt->GetVariable)(name, &tryGuid, &attr, &size, &v)) || size != sizeof v || v.hash != h) { v.hash = h; v.count = 0; }
    v.count++;
    if (EFI_ERROR(((SetVariableFn)rt->SetVariable)(name, &tryGuid, 7 /* non-volatile, boot and runtime */, sizeof v, &v))) return 0;
    return v.count;
}

void update_choose(EFI_FILE_PROTOCOL *root, UpdateChoice *u) {
    u->trial = 0;
    u->fallback = 0;
    u->kernel = "kernel.elf";
    u->initrd = "initrd.img";
    u->cmdline = "cmdline.txt";
    int have_old = kernel_ok(root, "kernel.old") && exists(root, "initrd.old");
    const char *why = 0;

    UINT8 buf[256];
    UINT64 size;
    if (read_prefix(root, "update.try", buf, sizeof buf, &size) == 0 && size > 0 && size <= sizeof buf) {
        UINT32 count = count_start(fnv1a(buf, (UINTN)size));
        if (count == 0) {
            log_str("loader: update.try is there (the new files are on trial) but the starts cannot be counted\n");
            if (have_old) why = "the starts of a trial cannot be counted";
        } else if (count <= TRIES) {
            u->trial = (int)count;
            log_str("loader: the new files are on trial (update.try): start "); log_dec(count);
            log_str(" of "); log_dec(TRIES); log_str(" before the previous set returns\n");
        } else if (have_old) {
            why = "the new files did not confirm themselves in time";
        } else {
            log_str("loader: the trial is over, but there is no previous kernel to go back to: starting the new files again\n");
        }
    }
    if (!why && !kernel_ok(root, "kernel.elf") && have_old) why = "kernel.elf is missing or damaged";
    if (why) {
        u->fallback = 1;
        u->trial = 0;
        u->kernel = "kernel.old";
        u->initrd = "initrd.old";
        int old_cmdline = exists(root, "cmdline.old");
        if (old_cmdline) u->cmdline = "cmdline.old";
        log_str("loader: starting the previous kernel and initrd (kernel.old, initrd.old"); log_str(old_cmdline ? ", cmdline.old" : "");
        log_str("): "); log_str(why); log_str("\n");
    }
}
