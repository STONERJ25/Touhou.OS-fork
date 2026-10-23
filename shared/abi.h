/*
 * Kernel <-> userland ABI: errno values, flags, structures, ioctls. Shared by the kernel and libgap.
 * Numbers follow Linux where that costs nothing, so existing C code ports without surprises.
 */
#ifndef SHARED_ABI_H
#define SHARED_ABI_H

#include <stdint.h>

/* ---- errno ---- */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define EDEADLK 35
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define EOVERFLOW 75
#define ECANCELED 125
#define ENOTSOCK 88
#define EPROTONOSUPPORT 93
#define ENOTSUP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111

/* ---- open flags ---- */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1

#define AT_FDCWD (-100)

/* ---- file modes ---- */
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000
#define S_IRWXU  0700
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRWXG  070
#define S_IRGRP  040
#define S_IWGRP  020
#define S_IXGRP  010
#define S_IRWXO  07
#define S_IROTH  04
#define S_IWOTH  02
#define S_IXOTH  01
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

struct gstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    int64_t  st_atime;
    int64_t  st_mtime;
    int64_t  st_ctime;
};

/* ---- mounts and filesystem statistics (SYS_mount / SYS_statfs) ---- */
#define MNT_RDONLY  0x00000001          /* mount flags: read-only */
#define MNT_OPTS    0x80000000          /* mount(): r8 points to a comma-separated option string ("uid=1000,fmask=0133") */

/* statfs(path, &st): space and identity of the filesystem that holds `path`. Block counts are in units of f_bsize. */
struct gstatfs {
    uint64_t f_type;                    /* Linux-compatible magic (tmpfs 0x01021994, FAT 0x4d44, proc 0x9fa0), 0 = unknown */
    uint64_t f_bsize;                   /* block size (the cluster size on FAT) */
    uint64_t f_blocks;                  /* total blocks */
    uint64_t f_bfree;                   /* free blocks */
    uint64_t f_bavail;                  /* free blocks available to ordinary users */
    uint64_t f_files;                   /* total file nodes, 0 = not tracked */
    uint64_t f_ffree;                   /* free file nodes */
    uint64_t f_namelen;                 /* longest file name */
    uint64_t f_flags;                   /* MNT_* flags of the mount */
    uint64_t f_fsid;                    /* mount id (st_dev of its files) */
    char f_fstypename[16];              /* "tmpfs", "fat32", "fat16", "procfs", ... */
    char f_mntfromname[64];             /* what was mounted ("/dev/vda2", "gensokyo") */
    char f_mntonname[64];               /* where it is mounted */
};

#define GNAME_MAX 255
struct gdirent {
    uint64_t d_ino;
    uint32_t d_type;            /* DT_* */
    uint32_t d_namlen;
    char     d_name[GNAME_MAX + 1];
};
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10

/* ---- signals ---- */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGWINCH 28
#define SIGCANCEL 31            /* reserved for pthread_cancel: sticky, never runs a handler; see docs/THREADS.md */
#define NSIG     32

#define WNOHANG 1
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s)    ((s) & 0x7f)

/* ---- mmap ---- */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

/* ---- time ---- */
struct gtimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* ---- system information ---- */
struct guname {
    char sysname[32];
    char nodename[32];
    char release[32];
    char version[64];
    char machine[32];
    char codename[48];
};

struct gsysinfo {
    uint64_t uptime_ms;
    uint64_t total_ram;
    uint64_t free_ram;
    uint64_t heap_used;
    uint32_t nprocs;
    uint32_t ntasks;
    uint64_t ticks;
    uint64_t irqs;
    uint32_t cpu_mhz;
    uint32_t fb_width, fb_height;
    char cpu_brand[48];
};

/* ---- terminal ---- */
#define NCCS 16
struct gtermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_cc[NCCS];
};
#define ICRNL  0x01
#define INLCR  0x02
#define IGNCR  0x04
#define OPOST  0x01
#define ONLCR  0x02
#define ISIG   0x01
#define ICANON 0x02
#define ECHO   0x04
#define ECHOE  0x08
#define ECHOK  0x10
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VMIN   5
#define VTIME  6
#define VSUSP  7
#define VWERASE 8

struct gwinsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* Sockets: AF_UNIX streams here; AF_INET constants, socket options and the netctl structures are in netabi.h */
#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_NONBLOCK   0x800
#define SOCK_CLOEXEC    0x80000
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2
struct gpeercred { int32_t pid; uint32_t uid, gid; };
#define SOCK_PEERCRED   0x8901          /* ioctl: struct gpeercred of the other end of a connected socket */

#define TIOCGNAME  0x5480               /* ioctl: NUL-terminated terminal name ("console", "pts/3") into a 64 byte buffer */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B

/* ---- reboot ---- */
#define REBOOT_RESTART  1
#define REBOOT_POWEROFF 2
#define REBOOT_CHECK    3               /* does nothing: 0 if the caller may restart or power off the machine, else -EPERM */

/* ---- framebuffer / graphics devices ---- */
struct gfbinfo {
    uint32_t width, height, pitch;      /* pitch in bytes */
    uint32_t bpp;                       /* always 32 */
    uint32_t flags;                     /* FBINFO_* */
};
#define FBINFO_SWAP_RB 1                /* hardware order is R in the low byte (RGBX); otherwise B in the low byte (BGRX) */
#define FBIOGET_INFO 0x4600
#define FBIO_FLUSH   0x4601             /* arg: struct grect* (NULL = everything); the display is single buffered, so a no-op today */
#define FBIO_GRAPHICS 0x4602            /* arg: int. 1 = this open file draws the screen (kernel text console pauses), 0 = text mode.
                                           Leaving graphics mode, or closing the file, restores the text console. */

struct grect {
    int32_t x, y, w, h;
};

/* ---- input events (/dev/input/events) ---- */
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_X     0
#define REL_Y     1
#define REL_WHEEL 8
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct ginput_event {
    uint8_t  type;
    uint8_t  pad;
    uint16_t code;
    int32_t  value;                     /* key: 1 down, 0 up, 2 repeat */
    uint64_t time_ms;
};
#define INPUT_GRAB 0x4501               /* arg: 1 = deliver keyboard to this reader instead of the console tty */

#include "netabi.h"

#endif /* SHARED_ABI_H */
