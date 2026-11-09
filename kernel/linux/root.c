/*
 * The Linux root (docs/COMPAT.md, "The Linux root"). The model is chroot(2) as its manual page describes it, plus an overlay of five directories of the real system (the
 * design is ours; nothing of an implementation of Linux was read). A Linux distribution's files (the dynamic linker, the C library, /bin/ls, /etc/passwd ...) are third-party data that the
 * user provides: `linuxroot` unpacks an archive of them into a tmpfs mounted on /linux, and a Linux program started from a native one runs with that tree as its "/".
 *
 * The rules, all of them in this file or in resolve() of kernel/fs/vfs.c:
 *   - Process.root is the top of the tree (NULL: the real root). A lookup that a process makes with its credentials starts there; the kernel's own lookups do not.
 *   - Inside the tree ".." stops at the top, and /proc, /dev, /tmp, /sys and /run are the real ones (the kernel's views of the machine: no distribution file can stand in for
 *     them; /tmp is shared with the native programs, which is what lets the two worlds hand each other files).
 *   - fork copies the root, a Linux program that execs another keeps it, a native program has none. A program that is started with setabi(ABI_LINUX | ABI_NOROOT) or while
 *     nothing is mounted on /linux runs in the real root, as the Linux programs of the first levels (static, kept at native paths) always did.
 *   - Entering the root moves the working directory to its top unless the directory is visible there under the same path (/tmp/x is; /HumanVillage/reimu is not).
 */
#include "linux.h"
#include "kernel.h"

/* The top of the tree mounted on /linux: a reference, or NULL when /linux is missing or is only an ordinary directory (nothing mounted on it). */
VNode *linux_root_get(void) {
    VNode *v;
    if (vfs_lookup_root(vfs_root(), "/linux", NULL, NULL, LOOKUP_DIR, &v)) return NULL;
    if (!v->mount || v->mount->root != v || !v->mount->covered) { vnode_put(v); return NULL; }
    return v;
}

/* The root the program that p execs will have, as a reference the exec takes over (or drops again when it fails). A Linux process keeps what it has; a native one that
 * becomes a Linux process enters the Linux root if there is one and the caller did not ask for the real root. */
VNode *linux_exec_root(Process *p) {
    if (p->abi == ABI_LINUX) {
        if (p->root) vnode_get(p->root);
        return p->root;
    }
    return p->exec_noroot ? NULL : linux_root_get();
}

/* Is the working directory of p the directory its cwd_path names when that path is read in root? */
static bool cwd_visible_in(Process *p, VNode *root) {
    VNode *v;
    if (vfs_lookup_root(root, p->cwd_path, NULL, &p->cred, LOOKUP_DIR, &v)) return false;
    bool same = v == p->cwd;
    vnode_put(v);
    return same;
}

/* execve has passed its point of no return: p now runs in root. */
void linux_root_adopt(Process *p, VNode *root) {
    VNode *old = p->root;
    p->root = root;
    if (old) vnode_put(old);                            /* the same tree: the exec took a second reference; the process keeps one */
    if (root == old) return;

    VNode *top = root ? root : vfs_root();
    if (root && cwd_visible_in(p, root)) return;        /* /tmp/x means the same directory on both sides */
    if (!root && !old) return;
    vnode_get(top);                                     /* otherwise the working directory is "/" of the new world */
    VNode *oldcwd = p->cwd;
    p->cwd = top;
    strlcpy(p->cwd_path, "/", sizeof p->cwd_path);
    if (oldcwd) vnode_put(oldcwd);
}
