"""The signal module: handlers, a process that signals itself, alarm and the interval timers interrupting a sleep, blocking and waiting for signals, SIGCHLD, SIGPIPE, KeyboardInterrupt,
signals between processes, the wake-up descriptor and the names of signals."""
import os
import signal
import sys
import threading
import time


def show(label, value=""):
    print(f"{label}: {value}")


print("--- handlers")
got = []
old = signal.signal(signal.SIGUSR1, lambda signum, frame: got.append(("USR1", signum, frame is not None)))
show("previous handler", old == signal.SIG_DFL)
os.kill(os.getpid(), signal.SIGUSR1)
show("handler ran", got)
signal.signal(signal.SIGUSR2, lambda s, f: got.append(("USR2", s)))
signal.raise_signal(signal.SIGUSR2)
show("raise_signal", got[-1])
show("getsignal", (signal.getsignal(signal.SIGUSR1) is not signal.SIG_DFL, signal.getsignal(signal.SIGTERM) == signal.SIG_DFL, signal.getsignal(signal.SIGINT) is signal.default_int_handler))
signal.signal(signal.SIGUSR1, signal.SIG_IGN)
os.kill(os.getpid(), signal.SIGUSR1)
show("ignored", len(got))
for bad in (signal.SIGKILL, signal.SIGSTOP):
    try:
        signal.signal(bad, lambda s, f: None)
    except OSError as e:
        show(f"cannot catch {signal.Signals(bad).name}", e.errno)
try:
    signal.signal(999, lambda s, f: None)
except ValueError as e:
    show("bad number", e)
show("names", (signal.Signals(2).name, signal.Signals.SIGTERM.value, signal.strsignal(signal.SIGTERM), signal.strsignal(signal.SIGSEGV), signal.SIGRTMIN, signal.SIGRTMAX))
show("valid_signals", (len(signal.valid_signals()), 9 in signal.valid_signals(), 0 in signal.valid_signals()))
show("numbers", [int(s) for s in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGILL, signal.SIGABRT, signal.SIGBUS, signal.SIGFPE, signal.SIGSEGV, signal.SIGPIPE,
                                    signal.SIGALRM, signal.SIGTERM, signal.SIGCHLD, signal.SIGCONT, signal.SIGSTOP, signal.SIGTSTP, signal.SIGTTIN, signal.SIGTTOU, signal.SIGURG,
                                    signal.SIGXCPU, signal.SIGXFSZ, signal.SIGVTALRM, signal.SIGPROF, signal.SIGWINCH, signal.SIGIO, signal.SIGPWR, signal.SIGSYS)])

print("--- KeyboardInterrupt and exceptions from handlers")
try:
    os.kill(os.getpid(), signal.SIGINT)
    time.sleep(1)
    show("SIGINT", "no exception")
except KeyboardInterrupt:
    show("SIGINT", "KeyboardInterrupt")


def raiser(signum, frame):
    raise RuntimeError(f"raised by the handler of {signal.Signals(signum).name}")


signal.signal(signal.SIGUSR1, raiser)
try:
    os.kill(os.getpid(), signal.SIGUSR1)
    time.sleep(1)
except RuntimeError as e:
    show("handler exception", e)
signal.signal(signal.SIGUSR1, signal.SIG_DFL)

print("--- alarm and timers")
ticks = []
signal.signal(signal.SIGALRM, lambda s, f: ticks.append("alarm"))
signal.alarm(1)
show("alarm left", signal.alarm(0) in (0, 1))
signal.alarm(1)
t0 = time.monotonic()
signal.pause()
show("pause woke up", (ticks, 0.5 < time.monotonic() - t0 < 20))
signal.alarm(1)
t0 = time.monotonic()
time.sleep(2)                                                       # PEP 475: the sleep goes on after the handler ran
show("sleep after the handler", (ticks, time.monotonic() - t0 >= 1.9))


def stop_sleeping(s, f):
    raise TimeoutError("the alarm ended the sleep")


signal.signal(signal.SIGALRM, stop_sleeping)
signal.alarm(1)
try:
    time.sleep(30)
except TimeoutError as e:
    show("handler ends the sleep", e)
signal.alarm(0)
signal.signal(signal.SIGALRM, lambda s, f: ticks.append("tick"))
ticks.clear()
signal.setitimer(signal.ITIMER_REAL, 0.05, 0.05)
t0 = time.monotonic()
while len(ticks) < 5 and time.monotonic() - t0 < 30:
    signal.pause()
show("setitimer interval", (ticks[:5], signal.getitimer(signal.ITIMER_REAL)[1] == 0.05))
show("setitimer returns the old", signal.setitimer(signal.ITIMER_REAL, 0)[1] == 0.05)
show("timer stopped", signal.getitimer(signal.ITIMER_REAL))
vt = []
signal.signal(signal.SIGVTALRM, lambda s, f: vt.append(s))
signal.setitimer(signal.ITIMER_VIRTUAL, 0.02)
t0 = time.monotonic()
while not vt and time.monotonic() - t0 < 60:
    sum(range(10000))
show("ITIMER_VIRTUAL fired", vt)
signal.setitimer(signal.ITIMER_VIRTUAL, 0)

print("--- blocking and waiting")
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR1})
show("mask", signal.SIGUSR1 in signal.pthread_sigmask(signal.SIG_BLOCK, set()))
signal.signal(signal.SIGUSR1, lambda s, f: got.append("late USR1"))
os.kill(os.getpid(), signal.SIGUSR1)
show("blocked: pending", (signal.SIGUSR1 in signal.sigpending(), "late USR1" in got))
show("sigwait", signal.sigwait({signal.SIGUSR1}) == signal.SIGUSR1)
show("after sigwait", (signal.SIGUSR1 in signal.sigpending(), "late USR1" in got))
os.kill(os.getpid(), signal.SIGUSR1)
signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGUSR1})
show("unblocked: delivered", "late USR1" in got)
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGUSR2})
os.kill(os.getpid(), signal.SIGUSR2)
info = signal.sigtimedwait({signal.SIGUSR2}, 5)
show("sigtimedwait", (info.si_signo, info.si_code == -6, info.si_pid == os.getpid()))
show("sigtimedwait time-out", signal.sigtimedwait({signal.SIGUSR2}, 0.05))
signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGUSR2})
signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGRTMIN})
for _ in range(3):
    os.kill(os.getpid(), signal.SIGRTMIN)
n = 0
while signal.sigtimedwait({signal.SIGRTMIN}, 0.2):
    n += 1
show("real-time signals queue", n)
signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGRTMIN})

print("--- threads")
seen_in = []
signal.signal(signal.SIGUSR1, lambda s, f: seen_in.append(threading.current_thread().name))
ready = threading.Event()
stop = threading.Event()


def thread_body():
    ready.set()
    stop.wait(30)


t = threading.Thread(target=thread_body, name="other")
t.start()
ready.wait()
signal.pthread_kill(t.ident, signal.SIGUSR1)
time.sleep(0.2)
show("pthread_kill runs the Python handler in the main thread", seen_in)
stop.set()
t.join()
res = []


def in_thread():
    try:
        signal.signal(signal.SIGUSR1, signal.SIG_IGN)
    except ValueError as e:
        res.append(str(e))


t3 = threading.Thread(target=in_thread)
t3.start()
t3.join()
show("signal() in a thread", res)

print("--- between processes")
r, w = os.pipe()
pid = os.fork()
if pid == 0:
    os.close(r)
    signal.signal(signal.SIGUSR1, lambda s, f: (os.write(w, b"child got USR1\n"), os._exit(0)))
    os.write(w, b"child ready\n")
    for _ in range(300):
        time.sleep(0.1)
    os._exit(1)
os.close(w)
buf = b""
while b"ready" not in buf:
    buf += os.read(r, 100)
os.kill(pid, signal.SIGUSR1)
while b"got" not in buf:
    buf += os.read(r, 100)
show("signal to a child", buf)
_, status = os.waitpid(pid, 0)
show("child status", os.waitstatus_to_exitcode(status))
os.close(r)
chld = []
signal.signal(signal.SIGCHLD, lambda s, f: chld.append(s))
pid = os.fork()
if pid == 0:
    os._exit(0)
t0 = time.monotonic()
while not chld and time.monotonic() - t0 < 30:
    time.sleep(0.01)
show("SIGCHLD", chld)
os.waitpid(pid, 0)
signal.signal(signal.SIGCHLD, signal.SIG_DFL)
pid = os.fork()
if pid == 0:
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    time.sleep(30)
    os._exit(0)
time.sleep(0.2)
os.kill(pid, signal.SIGTERM)
_, status = os.waitpid(pid, 0)
show("SIGTERM kills", os.waitstatus_to_exitcode(status))
pid = os.fork()
if pid == 0:
    os.abort()
_, status = os.waitpid(pid, 0)
show("abort", os.waitstatus_to_exitcode(status))
pid = os.fork()
if pid == 0:
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    rr, ww = os.pipe()
    os.close(rr)
    os.write(ww, b"x")
    os._exit(0)
_, status = os.waitpid(pid, 0)
show("SIGPIPE with the default action", os.waitstatus_to_exitcode(status))
pid = os.fork()
if pid == 0:
    os.kill(os.getpid(), signal.SIGSTOP)
    os._exit(8)
_, status = os.waitpid(pid, os.WUNTRACED)
show("stopped", (os.WIFSTOPPED(status), os.WSTOPSIG(status)))
os.kill(pid, signal.SIGCONT)
_, status = os.waitpid(pid, 0)
show("continued and exited", os.waitstatus_to_exitcode(status))
rr, ww = os.pipe()
os.close(rr)
try:
    os.write(ww, b"x")
except BrokenPipeError as e:
    show("SIGPIPE is ignored by Python: EPIPE", e.errno)
os.close(ww)
try:
    os.kill(2 ** 22 + 12345, 0)
except ProcessLookupError as e:
    show("kill of a missing process", e.errno)
try:
    os.kill(os.getpid(), 999)
except OSError as e:
    show("kill with a bad signal", e.errno)

print("--- the wake-up descriptor")
import socket  # noqa: E402

a, b = socket.socketpair()
a.setblocking(False)
signal.set_wakeup_fd(a.fileno())
signal.signal(signal.SIGUSR1, lambda s, f: None)
os.kill(os.getpid(), signal.SIGUSR1)
show("wake-up byte", b.recv(10))
signal.set_wakeup_fd(-1)
a.close()
b.close()
show("siginterrupt", signal.siginterrupt(signal.SIGUSR1, False))
print("--- done")
