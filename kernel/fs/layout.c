/*
 * The Gensokyo filesystem layout (see docs/THEMING.md): thematic directories are real, conventional POSIX names
 * are symlinks. Mounts devfs, procfs and a tmpfs for /BambooForest.
 */
#include "vfs.h"
#include "kernel.h"

void devfs_register_standard(void);

static void mk(const char *path, u32 mode, u32 uid, u32 gid) {
    int rc = vfs_mkdir(path, mode, NULL, NULL);
    if (rc && rc != -EEXIST) klog("Keine", "mkdir %s failed (%d)", path, rc);
    vfs_chown(path, uid, gid, NULL, NULL);
    vfs_chmod(path, mode, NULL, NULL);
}

void layout_create(void) {
    int rc = vfs_mount("tmpfs", "gensokyo", "/", 0);
    if (rc) panic("cannot mount the root filesystem (%d)", rc);

    mk("/Hakurei", 0755, 0, 0);                         /* the shrine: system configuration */
    mk("/Hakurei/etc", 0755, 0, 0);
    mk("/HumanVillage", 0755, 0, 0);                    /* homes */
    mk("/HumanVillage/reimu", 0755, 1000, 1000);
    mk("/HumanVillage/marisa", 0755, 1001, 1001);
    mk("/HumanVillage/sakuya", 0755, 1002, 1002);
    mk("/ScarletDevilMansion", 0755, 0, 0);             /* applications and Patchouli's library */
    mk("/ScarletDevilMansion/lib", 0755, 0, 0);
    mk("/ScarletDevilMansion/share", 0755, 0, 0);
    mk("/ScarletDevilMansion/Voile", 0755, 0, 0);       /* documentation */
    mk("/YoukaiMountain", 0755, 0, 0);                  /* devices (Kappa) and news (Bunbunmaru) */
    mk("/YoukaiMountain/Kappa", 0755, 0, 0);
    mk("/YoukaiMountain/Bunbunmaru", 0755, 0, 0);
    mk("/BambooForest", 01777, 0, 0);                   /* temporary files */
    mk("/Netherworld", 01777, 0, 0);                    /* trash */
    mk("/.boundary", 0700, 0, 0);
    mk("/dev", 0755, 0, 0);
    mk("/proc", 0555, 0, 0);
    mk("/sys", 0555, 0, 0);
    mk("/mnt", 0755, 0, 0);
    mk("/root", 0700, 0, 0);
    mk("/bin", 0755, 0, 0);
    mk("/sbin", 0755, 0, 0);
    mk("/var", 0755, 0, 0);

    vfs_symlink("Hakurei/etc", "/etc", NULL, NULL);
    vfs_symlink("HumanVillage", "/home", NULL, NULL);
    vfs_symlink("BambooForest", "/tmp", NULL, NULL);
    vfs_symlink("ScarletDevilMansion", "/usr", NULL, NULL);

    if ((rc = vfs_mount("devfs", "devfs", "/dev", 0))) klog("Keine", "devfs mount failed (%d)", rc);
    if ((rc = vfs_mount("procfs", "proc", "/proc", 0))) klog("Keine", "procfs mount failed (%d)", rc);
    if ((rc = vfs_mount("tmpfs", "bamboo", "/BambooForest", 0))) klog("Keine", "BambooForest mount failed (%d)", rc);
    vfs_chmod("/BambooForest", 01777, NULL, NULL);
    devfs_register_standard();
    klog("Keine", "filesystem online: / (tmpfs), /dev, /proc, /BambooForest");
}
