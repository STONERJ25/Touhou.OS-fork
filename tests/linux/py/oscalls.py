"""The less common calls of the os, fcntl, resource, termios and pty modules, one at a time: each prints what it returned (when that is a fact the manual promises) or the errno it
failed with. Nothing here depends on being root. Not in the list, because this system lacks them (docs/COMPAT.md, "State: L3", what does not work): named pipes (os.mkfifo),
pidfd_open and pidfd_send_signal, the owner of a descriptor for SIGIO (F_SETOWN), and the extended attributes (whether the file system has them is the file system's
business: the answer of the machine the outputs were made on says nothing about this one). Spliced pipes and files are here."""
import array
import errno
import fcntl
import os
import pty
import resource
import select
import signal
import stat
import struct
import sys
import termios
import time


def show(label, value=""):
    print(f"{label}: {value}")


def tryit(label, fn, *args, **kw):
    try:
        r = fn(*args, **kw)
        show(label, f"ok {r!r}" if r is not None else "ok")
        return r
    except OSError as e:
        show(label, f"{type(e).__name__} {errno.errorcode.get(e.errno, e.errno)}")
    except Exception as e:  # noqa: BLE001
        show(label, f"{type(e).__name__}: {e}")


os.mkdir("w")
os.chdir("w")
with open("f", "w") as f:
    f.write("0123456789" * 100)
fd = os.open("f", os.O_RDWR)

print("--- files")
tryit("mknod regular", os.mknod, "node", 0o600 | stat.S_IFREG)
tryit("mknod regular is a file", lambda: stat.S_ISREG(os.stat("node").st_mode))
tryit("chown to self", os.chown, "f", -1, -1)
tryit("lchown", os.lchown, "f", -1, -1)
tryit("fchown", os.fchown, fd, -1, -1)
tryit("fchmod", os.fchmod, fd, 0o640)
tryit("chmod follow_symlinks=False", lambda: os.chmod("f", 0o644, follow_symlinks=False))
os.symlink("f", "lnk2")
tryit("chmod of the link itself", lambda: os.chmod("lnk2", 0o600, follow_symlinks=False))
tryit("chmod through the link", lambda: (os.chmod("lnk2", 0o640), oct(os.stat("f").st_mode & 0o777))[1])


def closerange_tour():
    fds = [os.open("f", os.O_RDONLY) for _ in range(5)]
    os.closerange(fds[1], fds[3])                                    # the descriptors from the second up to (not including) the fourth
    states = []
    for fd in fds:
        try:
            os.fstat(fd)
            states.append("open")
        except OSError as e:
            states.append(errno.errorcode[e.errno])
    for fd in (fds[0], fds[3], fds[4]):
        os.close(fd)
    return states


tryit("closerange", closerange_tour)
tryit("fsync", os.fsync, fd)
tryit("fdatasync", os.fdatasync, fd)
tryit("sync", os.sync)
tryit("posix_fallocate", os.posix_fallocate, fd, 0, 8192)
tryit("size after fallocate", lambda: os.fstat(fd).st_size)
tryit("posix_fadvise", os.posix_fadvise, fd, 0, 0, os.POSIX_FADV_SEQUENTIAL)
tryit("preadv", lambda: (lambda b: (os.preadv(fd, [b], 0), bytes(b)))(bytearray(8)))
tryit("pwritev", os.pwritev, fd, [b"AB", b"CD"], 0)
tryit("utime with a descriptor", os.utime, fd, (1_000_000_000, 1_000_000_001))
tryit("utime ns", lambda: (os.utime("f", ns=(1_500_000_000_123456789, 1_600_000_000_987654321)), os.stat("f").st_mtime_ns // 1_000_000_000))
tryit("utime follow_symlinks=False", lambda: os.utime("f", (5, 5), follow_symlinks=False))
tryit("statvfs of a descriptor", lambda: os.fstatvfs(fd).f_bsize > 0)
tryit("lseek SEEK_DATA", lambda: os.lseek(fd, 0, os.SEEK_DATA))
tryit("lseek SEEK_HOLE", lambda: os.lseek(fd, 0, os.SEEK_HOLE) > 0)
tryit("sendfile", lambda: (lambda w: os.sendfile(w, fd, 0, 100))(os.open("copy", os.O_WRONLY | os.O_CREAT)))
tryit("copy_file_range", lambda: (lambda w: os.copy_file_range(fd, w, 50, 0, 0))(os.open("copy2", os.O_WRONLY | os.O_CREAT)))
tryit("memfd_create", lambda: (lambda m: (os.write(m, b"memory"), os.lseek(m, 0, 0), os.read(m, 10)))(os.memfd_create("test")))
tryit("pipe2 flags", lambda: (lambda r, w: (os.get_blocking(r), os.get_inheritable(w)))(*os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)))
tryit("eventfd", lambda: (lambda e: (os.eventfd_write(e, 5), os.eventfd_read(e)))(os.eventfd(0)))


def splice_tour():
    r, w = os.pipe()
    os.write(w, b"spliced through a pipe")
    out = os.open("spliced", os.O_WRONLY | os.O_CREAT)
    first = os.splice(r, out, 100)                                   # pipe to file, the position of the file moves on
    os.write(w, b" and more")
    second = os.splice(r, out, 100)
    os.close(out)
    r2, w2 = os.pipe()
    src = os.open("spliced", os.O_RDONLY)
    third = os.splice(src, w2, 8, offset_src=3)                      # file to pipe at an offset: the position of the file stays
    return first, second, third, os.read(r2, 20), os.lseek(src, 0, os.SEEK_CUR), open("spliced").read()


tryit("splice", splice_tour)
tryit("splice of two files", lambda: os.splice(os.open("f", os.O_RDONLY), os.open("copy3", os.O_WRONLY | os.O_CREAT), 10))
tryit("fwalk", lambda: sorted(d for d, _, _, _ in os.fwalk(".")))
tryit("scandir of a descriptor", lambda: sorted(e.name for e in os.scandir(os.open(".", os.O_RDONLY | os.O_DIRECTORY)))[:3])
tryit("listdir of a descriptor", lambda: sorted(os.listdir(os.open(".", os.O_RDONLY | os.O_DIRECTORY)))[:3])
dfd = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
tryit("stat with dir_fd", lambda: os.stat("f", dir_fd=dfd).st_size)
tryit("mkdir with dir_fd", os.mkdir, "viafd", 0o755, dir_fd=dfd)
tryit("rename with dir_fds", lambda: os.rename("viafd", "viafd2", src_dir_fd=dfd, dst_dir_fd=dfd))
tryit("symlink with dir_fd", lambda: os.symlink("f", "lnk", dir_fd=dfd))
tryit("readlink with dir_fd", lambda: os.readlink("lnk", dir_fd=dfd))
tryit("unlink with dir_fd", lambda: os.unlink("lnk", dir_fd=dfd))
tryit("access with dir_fd", lambda: os.access("f", os.R_OK, dir_fd=dfd))
tryit("open with dir_fd", lambda: os.fstat(os.open("f", os.O_RDONLY, dir_fd=dfd)).st_size)
tryit("fchmodat", lambda: os.chmod("f", 0o600, dir_fd=dfd))
tryit("link with dir_fd", lambda: os.link("f", "hardlink", src_dir_fd=dfd, dst_dir_fd=dfd))
tryit("rmdir with dir_fd", lambda: os.rmdir("viafd2", dir_fd=dfd))
tryit("fchdir", lambda: (os.fchdir(dfd), os.getcwd().endswith("w"))[1])

print("--- locks and fcntl")
tryit("flock", lambda: (fcntl.flock(fd, fcntl.LOCK_EX), fcntl.flock(fd, fcntl.LOCK_UN)))
tryit("flock non-blocking", lambda: fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB))
tryit("lockf", lambda: (fcntl.lockf(fd, fcntl.LOCK_EX), fcntl.lockf(fd, fcntl.LOCK_UN)))
tryit("F_GETFL", lambda: fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDWR)
tryit("F_SETFL O_NONBLOCK", lambda: (fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK), os.get_blocking(fd)))
tryit("F_DUPFD", lambda: fcntl.fcntl(fd, fcntl.F_DUPFD, 40))
tryit("F_DUPFD_CLOEXEC", lambda: (lambda n: (n, os.get_inheritable(n)))(fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 50)))
tryit("F_GETFD/F_SETFD", lambda: (fcntl.fcntl(fd, fcntl.F_SETFD, fcntl.FD_CLOEXEC), fcntl.fcntl(fd, fcntl.F_GETFD)))
r, w = os.pipe()
tryit("F_GETPIPE_SZ", lambda: fcntl.fcntl(r, fcntl.F_GETPIPE_SZ) >= 4096)
tryit("F_SETLK", lambda: len(fcntl.fcntl(fd, fcntl.F_SETLK, struct.pack("hhqqi", fcntl.F_WRLCK, os.SEEK_SET, 0, 0, 0))))
def fionread():
    os.write(w, b"12345")
    counted = array.array("i", [0])
    fcntl.ioctl(r, termios.FIONREAD, counted)
    return counted[0]


tryit("FIONREAD", fionread)
tryit("F_SETSIG/O_ASYNC", lambda: fcntl.fcntl(r, fcntl.F_SETFL, os.O_ASYNC))

print("--- process and scheduling")
tryit("nice", os.nice, 0)
tryit("getpriority", os.getpriority, os.PRIO_PROCESS, 0)
tryit("setpriority", os.setpriority, os.PRIO_PROCESS, 0, 0)
tryit("sched_getscheduler", os.sched_getscheduler, 0)
tryit("sched_getparam", lambda: os.sched_getparam(0).sched_priority)
tryit("sched_get_priority_max", os.sched_get_priority_max, os.SCHED_FIFO)
tryit("sched_yield", os.sched_yield)
tryit("sched_getaffinity", lambda: len(os.sched_getaffinity(0)) >= 1)
tryit("sched_setaffinity", lambda: os.sched_setaffinity(0, os.sched_getaffinity(0)))
tryit("sched_rr_get_interval", lambda: os.sched_rr_get_interval(0) >= 0)
tryit("getloadavg", lambda: len(os.getloadavg()))
tryit("sysconf", lambda: (os.sysconf("SC_PAGE_SIZE"), os.sysconf("SC_CLK_TCK"), os.sysconf("SC_NPROCESSORS_ONLN") >= 1, os.sysconf("SC_OPEN_MAX") > 0))
tryit("confstr", lambda: os.confstr("CS_PATH"))
tryit("pathconf", lambda: (os.pathconf(".", "PC_NAME_MAX"), os.pathconf(".", "PC_PATH_MAX")))
tryit("getgroups", lambda: isinstance(os.getgroups(), list))
tryit("getresuid", lambda: len(os.getresuid()))
tryit("setuid to its own", lambda: os.setuid(os.getuid()))
tryit("setegid to its own", lambda: os.setegid(os.getegid()))
tryit("getpgid", lambda: os.getpgid(0) > 0)
tryit("setpgid", lambda: os.setpgid(0, 0))
tryit("setpgrp", os.setpgrp)
tryit("killpg", lambda: os.killpg(os.getpgrp(), 0))
tryit("wait3", lambda: (lambda pid: (os.wait3(0)[0] == pid))(os.fork() or os._exit(0)))
tryit("ctermid", os.ctermid)
tryit("getrlimit NOFILE", lambda: resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 0)
tryit("setrlimit NOFILE", lambda: resource.setrlimit(resource.RLIMIT_NOFILE, resource.getrlimit(resource.RLIMIT_NOFILE)))
tryit("getrlimit STACK", lambda: resource.getrlimit(resource.RLIMIT_STACK)[0] > 0)
tryit("prlimit", lambda: resource.prlimit(0, resource.RLIMIT_CORE)[0] >= 0)
tryit("getrusage", lambda: (resource.getrusage(resource.RUSAGE_SELF).ru_utime >= 0, resource.getrusage(resource.RUSAGE_CHILDREN).ru_stime >= 0, resource.getrusage(resource.RUSAGE_THREAD).ru_utime >= 0))
tryit("getpagesize", resource.getpagesize)
tryit("max RSS is reported", lambda: resource.getrusage(resource.RUSAGE_SELF).ru_maxrss >= 0)
tryit("clock_nanosleep", lambda: time.sleep(0.001))
tryit("alarm remaining", lambda: (signal.alarm(10), signal.alarm(0))[1] > 0)
tryit("sigaltstack via faulthandler", lambda: __import__("faulthandler").enable())
tryit("faulthandler dump", lambda: __import__("faulthandler").dump_traceback(file=open(os.devnull, "w")))

print("--- terminals")
tryit("openpty", lambda: (lambda m, s: (os.isatty(m), os.isatty(s), os.ttyname(s).startswith("/dev/pts/")))(*os.openpty()))
m, s = os.openpty()
tryit("tcgetattr", lambda: len(termios.tcgetattr(s)))
tryit("tcsetattr", lambda: termios.tcsetattr(s, termios.TCSANOW, termios.tcgetattr(s)))
tryit("echo through a pty", lambda: (os.write(m, b"typed\n"), select.select([m], [], [], 10), os.read(m, 100))[2])
tryit("TIOCGWINSZ", lambda: len(fcntl.ioctl(s, termios.TIOCGWINSZ, b"\x00" * 8)))
tryit("TIOCSWINSZ", lambda: fcntl.ioctl(s, termios.TIOCSWINSZ, b"\x18\x00\x50\x00\x00\x00\x00\x00"))
tryit("window size read back", lambda: os.get_terminal_size(s))
tryit("tcflush", lambda: termios.tcflush(s, termios.TCIOFLUSH))
tryit("tcdrain", lambda: termios.tcdrain(s))
tryit("setraw", lambda: (__import__("tty").setraw(s), len(termios.tcgetattr(s)))[1])
tryit("not a terminal", termios.tcgetattr, fd)
os.close(m)
os.close(s)
pid, mfd = pty.fork()
if pid == 0:
    os.write(1, b"hello from the pty child\n")
    os._exit(3)
out = b""
while True:
    try:
        chunk = os.read(mfd, 100)
    except OSError:
        break
    if not chunk:
        break
    out += chunk
_, status = os.waitpid(pid, 0)
show("pty.fork", (out, os.waitstatus_to_exitcode(status)))
os.close(mfd)
print("--- done")
