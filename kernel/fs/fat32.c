/* FAT32 (Keine's records): placeholder until the block layer lands. */
#include "vfs.h"
#include "kernel.h"

static int fat32_mount(const char *source, u32 flags, Mount *m) {
    (void)source; (void)flags; (void)m;
    return -ENODEV;
}

const FsType g_fat32_type = { "fat32", fat32_mount, NULL };
