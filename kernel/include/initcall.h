/*
 * Initcalls: a subsystem registers its setup function with INITCALL(fn) and kmain() runs every registered function once
 * the VFS, devfs and the buses exist, so adding a subsystem never edits main.c.
 *
 *   static void fbdev_init(void) { devfs_register("fb0", ...); }
 *   INITCALL(fbdev_init);
 *
 * Order between initcalls is link order (unspecified): an initcall must not depend on another one.
 */
#ifndef K_INITCALL_H
#define K_INITCALL_H

typedef void (*initcall_t)(void);

#define INITCALL(fn) static const initcall_t __initcall_##fn __attribute__((used, section(".initcall"))) = fn

void run_initcalls(void);

#endif
