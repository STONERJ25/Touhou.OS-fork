/*
 * Default (empty) implementations of the driver entry points that kmain() calls. A driver provides the real function
 * in its own file and the linker picks it over these weak ones, so drivers can be added without touching kmain().
 */
#include "kernel.h"

__attribute__((weak)) void virtio_blk_init(void) { }
__attribute__((weak)) void ahci_init(void) { }
__attribute__((weak)) void nvme_init(void) { }
__attribute__((weak)) void usb_init(void) { }
/* Mounts persistent volumes (the labelled HumanVillage partition) once the block devices exist. */
__attribute__((weak)) void storage_mount_boot_volumes(void) { }
