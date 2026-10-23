"""Job control, priorities, hard links and /proc/<pid>/{fd,cwd,exe}.

* `jobctl` boots a text shell and runs the `jctest` program (user/bin/jctest.c): stop and continue, the waitpid words,
  SIGCHLD, Ctrl-Z through a pty, background reads and writes, orphaned groups, nice values, hard links, /proc/<pid>/fd.
* `jobctl_tools` types commands at the shell and checks what the tools print (kill -STOP, ps, nice, renice, ln, ls -l /proc)."""
import re

import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT

SECTIONS = [
    "SIGSTOP, SIGCONT, waitpid(WUNTRACED | WCONTINUED), SIGCHLD, State: T",
    "stop signals: ignored, caught, blocked, pending discarded by SIGCONT, handler for SIGCONT",
    "kill -STOP / -CONT / -TSTP through the kappabox applet and the shell builtin; ps shows T",
    "terminals: Ctrl-Z stops the foreground job, the shell gets the terminal back, fg continues it",
    "terminals: background read (SIGTTIN), TOSTOP writes and tcsetattr (SIGTTOU), EIO for ignored or orphaned",
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
