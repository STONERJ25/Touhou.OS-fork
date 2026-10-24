"""Job control, priorities, hard links and /proc/<pid>/{fd,cwd,exe}.

* `jobctl` boots a text shell and runs the `jctest` program (user/bin/jctest.c): stop and continue, the waitpid words,
  SIGCHLD, Ctrl-Z through a pty, background reads and writes, orphaned groups, nice values, hard links, /proc/<pid>/fd.
* `jobctl_tools` types commands at the shell and checks what the tools print (kill -STOP, ps, nice, renice, ln, ls -l /proc)."""
import re
import time

import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT

SECTIONS = [
    "SIGSTOP, SIGCONT, waitpid(WUNTRACED | WCONTINUED), SIGCHLD, State: T",
    "stop signals: ignored, caught, blocked, pending discarded by SIGCONT, handler for SIGCONT",
    "kill -STOP / -CONT / -TSTP through the kappabox applet and the shell builtin; ps shows T",
    "terminals: Ctrl-Z stops the foreground job, the shell gets the terminal back, fg continues it",
    "terminals: background read (SIGTTIN), TOSTOP writes and tcsetattr (SIGTTOU), EIO for ignored or orphaned",
    "terminals: a job that reads before the shell has handed it the terminal stops, and goes on when it gets it",
    "orphaned process groups: SIGHUP then SIGCONT for stopped members, stop signals dropped",
    "priorities: setpriority, getpriority, nice, renice, inheritance, and a polite process gets less CPU",
    "hard links: link(), st_nlink, one inode, unlink one name, the ln applet, errors",
    "/proc/<pid>/fd, cwd and exe",
]


def run_jctest(name):
    """Boots a text shell and runs jctest. Returns (output, exit status of jctest)."""
    with Qemu(cmdline="text", name=name) as vm:
        vm.expect(PROMPT, timeout=60)
        vm.send("jctest; echo __DONE''__$?\n")
        m = vm.expect(r"__DONE__(\d+)\n", timeout=300)
        return vm.text(), int(m.group(1))


@test
def jobctl():
    """Job control from user space: stop/continue, waitpid statuses, SIGCHLD, ttys, orphaned groups, nice, links, /proc fd."""
    out, rc = run_jctest("t-jobctl")
    text = out.split("jctest: the shrine's job control test begins", 1)[-1]
    for section in SECTIONS:
        assert f"jctest: {section}" in text, f"section did not run: {section}\n{text[-2500:]}"
    assert "JCTEST PASS" in text and rc == 0, text[-3000:]
    m = re.search(r"cpu ticks: nice 0 got (\d+), nice 19 got (\d+)", text)
    assert m and int(m.group(1)) > 3 * int(m.group(2)), f"the polite process should get far less CPU: {m and m.group(0)}"


# (command, substring that must appear in the output, expected exit status or None); $! is the last background job
TOOLS = [
    ("sleep 300 & echo bg=$!", "bg=", 0),
    ("cat /proc/$!/status | grep State", "State:", 0),
    ("kill -STOP $!; sleep 1; ps | grep sleep", " T ", 0),
    ("grep -i '^state' /proc/$!/status", "T", 0),
    ("ps -l | grep sleep", "T reimu", 0),
    ("ps -T | grep sleep", " T ", 0),
    ("kill -CONT $!; sleep 1; ps | grep sleep", " S ", 0),
    ("kill -TSTP $!; sleep 1; ps | grep sleep", " T ", 0),
    ("kill -CONT $!; sleep 1; ps | grep -c ' T '", "0", None),
    ("nice -n 5 nice", "5", 0),
    ("nice -3 nice", "3", 0),
    ("nice --3 nice 2>&1", "cannot set niceness", 0),
    ("nice nosuchprogram 2>&1", "cannot run", 127),
    ("renice 10 -p $!", "old priority 0, new priority 10", 0),
    ("grep Nice /proc/$!/status", "Nice:\t10", 0),
    ("ps -l | grep sleep", "10", 0),
    ("renice 4 -p $! 2>&1", "failed to set priority", 1),
    ("renice 5 -p 1 2>&1", "failed to set priority", 1),
    ("ls -l /proc/$!/fd | grep -c '^l'", "3", 0),
    ("ls -l /proc/$!/fd | grep -c /dev/console", "2", 0),
    ("ls -l /proc/$!/fd/1", "-> /dev/console", 0),
    ("readlink /proc/$!/exe", "/bin/sleep", 0),
    ("readlink /proc/self/cwd", "/HumanVillage/reimu", 0),
    ("kill $!; sleep 1; ls /proc/$!/fd 2>&1 | head -1", "No such file", None),
    # hard links
    ("echo one > /tmp/hl1; ln /tmp/hl1 /tmp/hl2; echo two >> /tmp/hl2; cat /tmp/hl1", "one\ntwo", 0),
    ("ls -l /tmp/hl1 | cut -c1-30", " 2 reimu", 0),
    ("rm /tmp/hl1; cat /tmp/hl2; ls -l /tmp/hl2 | cut -c1-30", " 1 reimu", 0),
    ("ln /tmp/hl2 /tmp/hl2 2>&1", "failed to create hard link", 1),
    ("mkdir -p /tmp/hld; ln /tmp/hld /tmp/hld2 2>&1", "Operation not permitted", 1),
    ("ln /tmp/hl2 /HumanVillage/reimu/hlx 2>&1", "Cross-device link", 1),
    ("rm /tmp/hl2; rmdir /tmp/hld; true", "", 0),
]


@test
def jobctl_tools():
    """kill -STOP/-CONT/-TSTP, ps (T, -l, -T), nice, renice, ln (hard links) and ls -l /proc/<pid>/fd at the shell."""
    with Qemu(cmdline="text", name="t-jobctl-tools") as vm:
        vm.expect(PROMPT, timeout=60)
        failures = []
        for cmd, want, rc in TOOLS:
            out, status = test_shell.run(vm, cmd)
            if want not in out or (rc is not None and status != rc):
                failures.append(f"$ {cmd}\n  want {want!r} rc={rc}, got rc={status}:\n{out.strip()}")
        assert not failures, f"{len(failures)} check(s) failed:\n" + "\n".join(failures[:6])


# ---------------------------------------------------------------- gapsh's job control, typed at the shell
#
# The same session runs twice: on the serial console (the text console of the machine) and inside `jctest pty`, which
# starts an interactive gapsh on a pseudo-terminal and relays the console to it (the way the desktop's terminal window
# talks to its shell). Ctrl-Z, Ctrl-C and Ctrl-D are the bytes 0x1a, 0x03 and 0x04 typed into the terminal.

CTRL_Z, CTRL_C, CTRL_D = "\x1a", "\x03", "\x04"
PTY_PROMPT = r"ptysh> "


def stopped(cmd_rx, n=1, why=""):
    return rf"\[{n}\]\+  Stopped{why} +{cmd_rx}\n"


def job_control_session(vm, prompt):
    """Drives an interactive gapsh through Ctrl-Z, jobs, bg, fg, wait, disown, kill and job specs."""
    def run(cmd, timeout=60):
        return test_shell.run(vm, cmd, timeout=timeout, prompt=prompt)

    def check(cmd, *want, rc=0, absent=()):
        out, status = run(cmd)
        for w in want:
            assert re.search(w, out), f"$ {cmd}\n  wanted /{w}/ in:\n{out}"
        for a in absent:
            assert not re.search(a, out), f"$ {cmd}\n  did not want /{a}/ in:\n{out}"
        assert rc is None or status == rc, f"$ {cmd}\n  status {status}, wanted {rc}:\n{out}"
        return out

    def since(mark):
        return vm.text()[mark:]

    # Ctrl-Z stops the foreground job and the prompt comes back
    vm.send("tr a-z A-Z\n")
    vm.send("zun\n")
    vm.expect(r"ZUN\n", timeout=60)                         # tr is running and has the terminal
    vm.send(CTRL_Z)
    vm.expect(stopped("tr a-z A-Z"), timeout=30)
    vm.expect(prompt, timeout=30)
    check("jobs", r"^\[1\]\+  Stopped +tr a-z A-Z\n$")
    check("grep State /proc/$(jobs -p)/status", r"State:\s+T")
    check("echo still-here", r"still-here")                 # the shell reads its own line, the stopped job does not

    # fg %1 continues it in front: it reads the terminal again
    vm.send("fg %1\n")
    vm.expect(r"\ntr a-z A-Z\n", timeout=30)
    vm.send("more\n")
    vm.expect(r"MORE\n", timeout=30)
    vm.send(CTRL_D)
    vm.expect(prompt, timeout=30)
    check("jobs; echo count=$(jobs | wc -l)", r"count=0")

    # bg continues a stopped job in the background; wait %1 returns its exit status
    vm.send("gapsh -c 'echo __RE\"\"ADY__; sleep 2; echo resumed > /tmp/jc.bg; exit 5'\n")
    vm.expect(r"\n__READY__\n", timeout=60)
    vm.send(CTRL_Z)
    vm.expect(stopped("gapsh -c echo .*exit 5"), timeout=30)
    vm.expect(prompt, timeout=30)
    check("bg", r"\[1\]\+ gapsh -c echo .*&\n")
    check("jobs", r"\[1\]\+  Running +gapsh -c")
    check("wait %1; echo st=$?", r"st=5")
    check("cat /tmp/jc.bg; jobs", r"^resumed\n$")

    # job specs, and a stopped job that is killed is reported
    check("sleep 301 &", r"\[1\] \d+")
    check("sleep 302 &", r"\[2\] \d+")
    check("jobs", r"\[1\]-  Running +sleep 301\n", r"\[2\]\+  Running +sleep 302\n")
    check("kill -STOP %-; jobs", r"\[1\]\+  Stopped \(signal\) +sleep 301\n", r"\[2\]-  Running +sleep 302\n")
    check("jobs %sleep", r"ambiguous job spec", rc=1)
    check("jobs %?302 %%", r"\[2\]-  Running +sleep 302\n\[1\]\+  Stopped")
    check("fg %9", r"%9: no such job", rc=1)
    mark = len(vm.text())
    check("kill %?301")
    check("sleep 1; jobs")
    assert re.search(r"\[1\][ +-]  Terminated +sleep 301\n", since(mark)), "the killed stopped job was not reported:\n" + since(mark)
    mark = len(vm.text())
    check("kill -STOP %sleep; kill -9 %%")
    check("sleep 1; jobs")
    assert re.search(r"\[2\][ +-]  Killed +sleep 302\n", since(mark)), "the job killed with -9 was not reported:\n" + since(mark)
    check("jobs; echo count=$(jobs | wc -l)", r"count=0")

    # disown forgets a job (a stopped one is continued first, nobody could do it later)
    check("sleep 303 &")
    check("kill -STOP %+; disown; jobs; grep State /proc/$!/status", r"State:", absent=[r"State:\s+T", r"sleep 303"])
    check("kill -0 $! && echo alive", r"alive")
    check("disown %1", r"no such job", rc=1)
    check("kill $!")

    # a pipeline is one job
    vm.send("cat | tr a-z A-Z\n")
    vm.send("pipe\n")
    vm.expect(r"PIPE\n", timeout=60)
    vm.send(CTRL_Z)
    vm.expect(stopped(r"cat \| tr a-z A-Z"), timeout=30)
    vm.expect(prompt, timeout=30)
    check("jobs -l", r"^\[1\]\+  \d+ \d+ Stopped +cat \| tr a-z A-Z\n$")
    vm.send("fg\n")
    vm.expect(r"\ncat \| tr a-z A-Z\n", timeout=30)
    vm.send("again\n")
    vm.expect(r"AGAIN\n", timeout=30)
    vm.send(CTRL_D)
    vm.expect(prompt, timeout=30)

    def jobs_until(pattern):
        for _ in range(20):
            out, _ = run("jobs")
            if re.search(pattern, out):
                return
            run("sleep 1")
        raise AssertionError(f"jobs never showed /{pattern}/:\n{out}")

    # a background job that reads the terminal stops (SIGTTIN); fg gives it the terminal
    check("cat > /tmp/jc.in &")
    jobs_until(r"\[1\]\+  Stopped \(tty input\) +cat > /tmp/jc.in")
    vm.send("fg\n")
    vm.expect(r"\ncat > /tmp/jc.in\n", timeout=30)
    vm.send("typed\n")
    vm.send(CTRL_D)
    vm.expect(prompt, timeout=30)
    check("cat /tmp/jc.in", r"^typed\n$")

    # terminal settings: a stopped job's are saved, the shell's come back, fg restores the job's
    vm.send("jctest rawstop\n")
    vm.expect(r"RAWSTOP ready\n", timeout=60)
    vm.send(CTRL_Z)
    vm.expect(stopped("jctest rawstop"), timeout=30)
    vm.expect(prompt, timeout=30)
    check("jctest tmodes", r"TMODES icanon=1 echo=1 isig=1")
    vm.send("fg\n")
    vm.expect(r"\njctest rawstop\n", timeout=30)
    vm.send("q")
    vm.expect(r"RAWSTOP after icanon=0 echo=0 isig=1\n", timeout=30)
    vm.expect(prompt, timeout=30)
    check("jctest tmodes", r"TMODES icanon=1 echo=1 isig=1")

    # an interactive shell started in the background waits for fg instead of taking the terminal
    check("gapsh &")
    jobs_until(r"\[1\]\+  Stopped \(tty input\) +gapsh")
    vm.send("fg\n")
    vm.expect(r"\ngapsh\n", timeout=30)
    vm.expect(prompt, timeout=60)
    check("echo nested-$$", r"nested-\d+")
    vm.send("exit\n")
    vm.expect(prompt, timeout=60)
    check("jobs; echo count=$(jobs | wc -l)", r"count=0")

    # ^C and ^Z drop the rest of the command line, so a loop cannot run away with the terminal
    mark = len(vm.text())
    vm.send("tr a-z A-Z; echo not-re''ached\n")
    vm.send("cc\n")
    vm.expect(r"CC\n", timeout=60)
    vm.send(CTRL_C)
    vm.expect(prompt, timeout=30)
    vm.send("while true; do tr a-z A-Z; done; echo not-re''ached\n")
    vm.send("zz\n")
    vm.expect(r"ZZ\n", timeout=60)
    vm.send(CTRL_Z)
    vm.expect(stopped("tr a-z A-Z"), timeout=30)
    vm.expect(prompt, timeout=30)
    check("jobs", r"^\[1\]\+  Stopped +tr a-z A-Z\n$")
    assert "not-reached" not in since(mark), since(mark)

    # leaving with a stopped job warns first
    check("exit", r"There are stopped jobs", rc=1)
    check("echo still-alive", r"still-alive")


@test
def jobctl_shell_console():
    """gapsh job control on the text console: Ctrl-Z, jobs, fg, bg, wait %N, disown, kill %N, job specs, pipelines."""
    with Qemu(cmdline="text", name="t-jobctl-console") as vm:
        vm.expect(PROMPT, timeout=60)
        job_control_session(vm, PROMPT)
        out, rc = test_shell.run(vm, "kill -9 %1; wait %1; echo st=$?")
        assert "st=137" in out, out


@test
def jobctl_shell_pty():
    """The same through a pseudo-terminal (jctest pty), and leaving it twice hangs up the stopped job."""
    with Qemu(cmdline="text", name="t-jobctl-pty") as vm:
        vm.expect(PROMPT, timeout=60)
        vm.send("PS1='ptysh> ' jctest pty\n")
        vm.expect(PTY_PROMPT, timeout=60)
        out, _ = test_shell.run(vm, "tty", prompt=PTY_PROMPT)
        assert "/dev/pts/" in out, out
        job_control_session(vm, PTY_PROMPT)
        out, _ = test_shell.run(vm, "echo pid=$(jobs -p)", prompt=PTY_PROMPT)
        pid = re.search(r"pid=(\d+)", out).group(1)
        out, _ = test_shell.run(vm, "exit", prompt=PTY_PROMPT)
        assert "There are stopped jobs" in out, out
        vm.send("exit\n")                                   # right after the warning: leaves, the stopped job gets SIGHUP
        vm.expect(r"pty: the shell left with status \d+\n", timeout=60)
        vm.expect(PROMPT, timeout=30)
        for _ in range(20):
            out, _ = test_shell.run(vm, f"test -e /proc/{pid} && echo alive || echo gone")
            if "gone" in out:
                break
            test_shell.run(vm, "sleep 1")
        assert "gone" in out, f"the stopped job {pid} outlived its shell"


@test
def jobctl_desktop_terminal():
    """Ctrl-Z in the desktop's terminal window (gapterm sends ^Z through its pty): the job stops, jobs lists it, fg resumes it."""
    with Qemu(cmdline="", name="t-jobctl-desktop", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=90)
        time.sleep(5)
        vm.key("t", ctrl=True, alt=True)
        time.sleep(3)

        def retry_until(action, pattern, tries=10, wait=5):
            for _ in range(tries):
                action()
                try:
                    return vm.expect(pattern, timeout=wait)
                except TimeoutError:
                    pass
            raise AssertionError(f"never saw /{pattern}/; serial tail:\n{vm.text()[-800:]}")

        retry_until(lambda: vm.type("logger -t jc term-$((6*7))\n"), r"term-42")
        # the job logs once it has the terminal, then logs every line it reads from it
        vm.type("gapsh -c 'logger -t jc job-$((1+1)); while read l; do logger -t jc got-$l; done; logger -t jc job-done'\n")
        vm.expect(r"job-2", timeout=60)
        vm.key("z", ctrl=True)
        vm.type("logger -t jc \"st=$? $(jobs)\"\n")
        vm.expect(r"st=148 \[1\]\+\s+Stopped\s+gapsh -c logger", timeout=60)
        vm.type("fg\n")
        vm.type("zun\n")                                  # read by the job once fg has handed it the terminal
        vm.expect(r"got-zun", timeout=60)
        vm.key("d", ctrl=True)
        vm.expect(r"job-done", timeout=60)
        retry_until(lambda: vm.type("logger -t jc \"jobs=$(jobs | wc -l)\"\n"), r"jobs=0", tries=4, wait=15)
