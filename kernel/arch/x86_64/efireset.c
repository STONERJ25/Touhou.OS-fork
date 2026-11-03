/*
 * Restarting the machine the way a UEFI firmware describes it: the runtime service ResetSystem.
 *
 * The chipset's own ways (the FADT reset register, the PCI reset port 0xCF9, the keyboard controller, a triple fault) are what Linux tries first, and they are what
 * this kernel tried first: the Dell OptiPlex ignored all four and sat there, the network dead and the screen frozen, after "restart" (the first remote update to the
 * real machine, 2026-10-25). The firmware knows how its own platform is reset; on a UEFI machine that is the way it is meant to be done.
 *
 * The loader passes the service's address (BootInfo.efi_reset_system). SetVirtualAddressMap is never called, so the service runs in physical mode, which the
 * specification allows: it needs the firmware's runtime memory mapped where it is. That is what happens here, just for this one call: a copy of the kernel's top-level
 * page table (the upper half, kernel and direct map, shared) with the lower half an identity map made of 2 MiB pages, write-back where the memory map says there is
 * memory and uncached everywhere else below 4 GiB (the chipset registers the firmware may write). Interrupts are off, the kernel's GDT and stack stay.
 */
#include "arch.h"
#include "cpu.h"
#include "kernel.h"
#include "mm.h"

u64 efi_call4(u64 fn, u64 a, u64 b, u64 c, u64 d);           /* efi_call.asm: a call with the Microsoft x64 convention */

#define EFI_RESET_COLD 0u
#define TWO_MIB (2UL << 20)

static u64 *table_at(u64 *tbl, unsigned idx) {
    if (!(tbl[idx] & PTE_P)) {
        paddr_t p = pmm_alloc_page_zeroed();
        if (!p) return NULL;
        tbl[idx] = p | PTE_P | PTE_W;
    }
    return phys_to_virt(tbl[idx] & PTE_ADDR);
}

/* [start, end) identity-mapped with 2 MiB pages (the ones it touches), with the given cache bits. */
static bool map_2m(u64 *pml4, u64 start, u64 end, u64 cache) {
    for (u64 a = start & ~(TWO_MIB - 1); a < end; a += TWO_MIB) {
        u64 *pdpt = table_at(pml4, (unsigned)((a >> 39) & 511));
        u64 *pd = pdpt ? table_at(pdpt, (unsigned)((a >> 30) & 511)) : NULL;
        if (!pd) return false;
        pd[(a >> 21) & 511] = a | PTE_P | PTE_W | PTE_PS | cache;
    }
    return true;
}

bool efi_reset_known(void) { return g_boot.version >= 4 && g_boot.efi_reset_system != 0; }

/* Returns only if the service is missing or did not restart the machine (interrupts are off when it is called). */
bool efi_reset_system(void) {
    if (!efi_reset_known()) return false;
    paddr_t root = pmm_alloc_page_zeroed();
    if (!root) return false;
    u64 *pml4 = phys_to_virt(root);
    const u64 *kernel = phys_to_virt(vmm_kernel_pml4());
    for (int i = 256; i < 512; i++) pml4[i] = kernel[i];
    bool ok = map_2m(pml4, 0, 4UL << 30, PTE_PCD);
    u32 n = 0;
    const BootMemRegion *r = boot_mmap(&n);
    for (u32 i = 0; ok && i < n; i++)
        if (r[i].type != BOOTMEM_MMIO && r[i].length) ok = map_2m(pml4, r[i].base, r[i].base + r[i].length, 0);
    if (!ok) return false;
    u64 old = read_cr3();
    write_cr3(root);
    efi_call4(g_boot.efi_reset_system, EFI_RESET_COLD, 0, 0, 0);
    write_cr3(old);
    return false;
}
