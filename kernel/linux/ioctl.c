/*
 * ioctl for the Linux personality (docs/COMPAT.md, L2). Almost every request a Linux program makes means the same here as natively: the request numbers of the terminal calls
 * are Linux's, and so are the window size and the process group arguments. The exception is the terminal's attributes: a Linux program (readline, a shell with line editing, vi, stty)
 * reads and writes `struct termios` of the kernel (36 bytes: four flag words, the line discipline, 19 control characters) with Linux's bit values and control character
 * positions, while the native structure (struct gtermios) has fewer flags in other places. TCGETS and TCSETS translate between the two; what Linux has and the native terminal does
 * not (flow control, the extra echo flags, the characters for ^R ^W ^V ^O) is made up on the way out, as a default Linux terminal has it, and ignored on the way in.
 * Written from the termios(3) and tty_ioctl(4) manual pages and the asm-generic/termbits.h values that every program is built against.
 */
#include "linux.h"
#include "kernel.h"
#include "calls.h"
#include "netabi.h"

struct lnx_termios {
    u32 c_iflag, c_oflag, c_cflag, c_lflag;
    u8  c_line;
    u8  c_cc[19];
};
_Static_assert(sizeof(struct lnx_termios) == 36, "the kernel's struct termios is 36 bytes");

/* Linux bit values (octal, as in termbits.h) and their native counterparts */
static const struct { u32 lnx, nat; } g_iflag[] = { { 0400, ICRNL }, { 0100, INLCR }, { 0200, IGNCR } };
static const struct { u32 lnx, nat; } g_oflag[] = { { 01, OPOST }, { 04, ONLCR } };
static const struct { u32 lnx, nat; } g_lflag[] = { { 01, ISIG }, { 02, ICANON }, { 010, ECHO }, { 020, ECHOE }, { 040, ECHOK }, { 0400, TOSTOP } };
static const struct { int lnx, nat; } g_cc[] = { { 0, VINTR }, { 1, VQUIT }, { 2, VERASE }, { 3, VKILL }, { 4, VEOF }, { 5, VTIME }, { 6, VMIN }, { 10, VSUSP } };

#define LNX_IXON     02000
#define LNX_IUTF8    040000
#define LNX_CFLAG    0x4bf          /* CS8 | CREAD | HUPCL | B38400: what a console starts with */
#define LNX_ECHOCTL  01000
#define LNX_ECHOKE   04000
#define LNX_IEXTEN   0100000

static void to_linux(const struct gtermios *n, struct lnx_termios *l) {
    memset(l, 0, sizeof *l);
    l->c_iflag = LNX_IXON | LNX_IUTF8;
    for (unsigned i = 0; i < ARRAY_SIZE(g_iflag); i++) if (n->c_iflag & g_iflag[i].nat) l->c_iflag |= g_iflag[i].lnx;
    for (unsigned i = 0; i < ARRAY_SIZE(g_oflag); i++) if (n->c_oflag & g_oflag[i].nat) l->c_oflag |= g_oflag[i].lnx;
    l->c_cflag = LNX_CFLAG;
    l->c_lflag = LNX_ECHOCTL | LNX_ECHOKE | LNX_IEXTEN;
    for (unsigned i = 0; i < ARRAY_SIZE(g_lflag); i++) if (n->c_lflag & g_lflag[i].nat) l->c_lflag |= g_lflag[i].lnx;
    static const u8 made_up[19] = { [8] = 021, [9] = 023, [12] = 022, [13] = 017, [14] = 027, [15] = 026 };       /* ^Q ^S ^R ^O ^W ^V */
    memcpy(l->c_cc, made_up, sizeof made_up);
    for (unsigned i = 0; i < ARRAY_SIZE(g_cc); i++) l->c_cc[g_cc[i].lnx] = n->c_cc[g_cc[i].nat];
}

/* n holds the current native attributes: what Linux cannot express stays as it is. */
static void to_native(const struct lnx_termios *l, struct gtermios *n) {
    n->c_iflag = n->c_oflag = n->c_lflag = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(g_iflag); i++) if (l->c_iflag & g_iflag[i].lnx) n->c_iflag |= g_iflag[i].nat;
    for (unsigned i = 0; i < ARRAY_SIZE(g_oflag); i++) if (l->c_oflag & g_oflag[i].lnx) n->c_oflag |= g_oflag[i].nat;
    for (unsigned i = 0; i < ARRAY_SIZE(g_lflag); i++) if (l->c_lflag & g_lflag[i].lnx) n->c_lflag |= g_lflag[i].nat;
    for (unsigned i = 0; i < ARRAY_SIZE(g_cc); i++) n->c_cc[g_cc[i].nat] = l->c_cc[g_cc[i].lnx];
}

long lnx_ioctl(TrapFrame *tf) {
    u64 req = A1(tf);
    if (req == LNX_FIONBIO) {                               /* non-blocking mode and close-on-exec by ioctl: what the native ioctl has no request for */
        int on;
        if (copy_from_user(&on, (const void *)A2(tf), sizeof on)) return -EFAULT;
        File *f = fd_get(current->proc, (int)A0(tf));
        if (!f) return -EBADF;
        if (on) f->flags |= O_NONBLOCK; else f->flags &= ~O_NONBLOCK;
        return 0;
    }
    if (req == LNX_FIOCLEX || req == LNX_FIONCLEX) {
        int fd = (int)A0(tf);
        if (!fd_get(current->proc, fd)) return -EBADF;
        if (req == LNX_FIOCLEX) current->proc->fd_flags[fd] |= FD_CLOEXEC; else current->proc->fd_flags[fd] &= (u8)~FD_CLOEXEC;
        return 0;
    }
    if (req != TCGETS && req != TCSETS && req != TCSETSW && req != TCSETSF) {
        SyscallFn native = syscall_native(SYS_ioctl);
        return native ? native(tf) : -ENOSYS;
    }
    File *f = fd_get_ref(current->proc, (int)A0(tf));
    if (!f) return -EBADF;
    u8 buf[256];                                            /* the native request handlers get a buffer of this size */
    memset(buf, 0, sizeof buf);
    struct gtermios *nat = (struct gtermios *)buf;
    struct lnx_termios lnx;
    long rc = vfs_ioctl(f, TCGETS, buf);                    /* the current attributes (also the check that this is a terminal) */
    if (rc >= 0 && req == TCGETS) {
        to_linux(nat, &lnx);
        rc = copy_to_user((void *)A2(tf), &lnx, sizeof lnx) ? -EFAULT : 0;
    } else if (rc >= 0) {
        if (copy_from_user(&lnx, (void *)A2(tf), sizeof lnx)) rc = -EFAULT;
        else {
            to_native(&lnx, nat);
            rc = vfs_ioctl(f, req, buf);
        }
    }
    file_put(f);
    return rc < 0 ? rc : 0;
}
