"""Job control: stop and continue, waitpid statuses, terminals (Ctrl-Z, SIGTTIN/SIGTTOU), orphaned groups, priorities,
hard links and /proc/<pid>/{fd,cwd,exe}. The checks run in the guest as the `jctest` program (user/bin/jctest.c); this file
boots a text shell, runs it and reports what it printed."""
import re

import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT


def run_jctest(name):
    """Boots a text shell and runs jctest. Returns the output; a verdict line is always present when the program finished."""
    with Qemu(cmdline="text", name=name) as vm:
        vm.expect(PROMPT, timeout=60)
        vm.send("jctest; echo __DONE''__$?\n")
        m = vm.expect(r"__DONE__(\d+)\n", timeout=300)
        out = vm.text()
        return out, int(m.group(1))


@test
def jobctl():
    """Job control from user space: stop/continue, waitpid statuses, SIGCHLD, ttys, orphaned groups, nice, links, /proc fd."""
    out, rc = run_jctest("t-jobctl")
    sections = out.split("jctest: the shrine's job control test begins", 1)[-1]
    assert "JCTEST PASS" in sections and rc == 0, sections[-3000:]
