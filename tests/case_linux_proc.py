"""docs/COMPAT.md, "A Linux-shaped /proc": a program of the Linux personality sees /proc as proc(5) describes it, a native program sees the native /proc it always saw.

Two kinds of test. The facts about the formats come from tests/linux/src/proc_probe.c (static, and as a dynamic PIE from the Linux root): it reads /proc/self/stat, status, statm, cmdline,
environ, maps, smaps, limits, mountinfo, the links, fd, task, a child process, /proc/stat, uptime, loadavg, meminfo, cpuinfo, /proc/sys ... and prints one line per fact, with no number
that differs between machines, so its output on TouhouOS must equal the stored output of a real Linux (tools/linux-oracle.py). Then the real procps and util-linux programs of the Linux
root (ps, free, uptime, pidof, pgrep, pkill, top, vmstat, nproc, lscpu, kill) are run in the guest and their output is checked by shape: the headers, the columns, plausible numbers, the
processes that must be in the lists. Nothing of the Linux side is committed; without WSL (or a Linux with gcc) these tests print SKIP and pass.

    python tools/qemu-slot.py -- python tests/run_all.py linux_proc
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import test
from case_linux import compare_with_linux
from case_linux_dyn import skip_reason
from case_linux_exec import Guest, golden_run


@test
def linux_proc_probe_matches_linux():
    """proc_probe reads the Linux view of /proc (the stat, status, statm, cmdline, environ, comm, maps and smaps of itself, a thread and a child that sleeps and then is a zombie, limits,
    mountinfo, the links self, thread-self, exe, cwd, root, fd and fdinfo, /proc/stat, uptime, loadavg, meminfo, cpuinfo, version, filesystems, partitions and /proc/sys/...) and says for
    each fact whether it holds: the same lines as the stored output of a real Linux, as a static program without a Linux root, with the root mounted, and as a dynamic one started by name"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with Guest("t-linux-proc-probe", ["proc_probe", "dyn_proc_probe"], late_root=True) as w:
        golden_run(w, "proc_probe", "FOO=bar linux /tmp/proc_probe alpha beta", timeout=240)          # real paths: no Linux root is mounted yet
        w.mount_root()
        golden_run(w, "proc_probe", "FOO=bar linux /tmp/proc_probe alpha beta", timeout=240)          # the same program inside the Linux root
        golden_run(w, "dyn_proc_probe", "FOO=bar /tmp/dyn_proc_probe alpha beta", timeout=240)        # dynamically linked, and started from the native shell by its name


@test
def linux_proc_native_view_is_untouched():
    """The same paths show a different view to a Linux program and to a native one: stat, uptime, status, the listing of /proc and /proc/self differ, while a file a driver registers and
    root writes (/proc/net/tune) is the same bytes for both and can be written from a Linux shell"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with Guest("t-linux-proc-native", []) as w:
        sh = "linux /bin/sh -c"
        out, rc = w.run("cat /proc/self/stat")
        assert rc == 0 and re.fullmatch(r"\d+ \d+ \d+ \d+ \d+ [RSTZ] \d+ \d+ \d+ \d+ \S+\n", out), f"the native stat line: {out!r}"
        out, rc = w.run("linux /bin/cat /proc/self/stat")
        assert rc == 0 and re.match(r"\d+ \(cat\) R \d+ \d+ \d+ \d+ \d+ \d+ ", out) and len(out.split()) == 52, f"the Linux stat line (52 fields): {out!r}"
        out, rc = w.run("cat /proc/uptime")
        assert re.fullmatch(r"\d+\.\d\d\n", out), f"native uptime has one number: {out!r}"
        out, rc = w.run("linux /bin/cat /proc/uptime")
        assert re.fullmatch(r"\d+\.\d\d \d+\.\d\d\n", out), f"Linux uptime has two: {out!r}"
        out, rc = w.run("cat /proc/self/status")
        assert "\nPGid:\t" in out and "\nVmSize:\t" in out and "TracerPid" not in out, f"the native status: {out!r}"
        out, rc = w.run("linux /bin/cat /proc/self/status")
        assert "\nTracerPid:\t0\n" in out and "\nPGid:" not in out and "\nSigCgt:\t" in out, f"the Linux status: {out!r}"
        out, rc = w.run("ls /proc")
        names = out.split()
        assert "tasks" in names and "pci" in names and "thread-self" not in names, f"the native /proc listing: {names}"
        out, rc = w.run("linux /bin/ls /proc")
        names = out.split()
        assert {"self", "thread-self", "stat", "meminfo", "cpuinfo", "sys", "net", "1"} <= set(names) and "tasks" not in names, f"the Linux /proc listing: {names}"
        out, rc = w.run("ls -ld /proc/self")
        assert out.startswith("d"), f"/proc/self is a directory for a native program: {out!r}"
        out, rc = w.run("linux /bin/ls -ld /proc/self")
        assert out.startswith("l") and "/proc/self -> " in out, f"/proc/self is a symbolic link for a Linux program: {out!r}"
        out, rc = w.run("linux /bin/readlink /proc/self")
        assert rc == 0 and re.fullmatch(r"\d+\n", out), out
        # the mappings of a dynamic program are named after the files, with the links resolved as Linux does (/lib is /usr/lib, /bin is /usr/bin)
        out, rc = w.run("linux /bin/cat /proc/self/maps")
        assert rc == 0 and re.search(r"^[0-9a-f]{12}-[0-9a-f]{12} r-xp [0-9a-f]{8} \d\d:\d\d \d+ +/usr/lib/x86_64-linux-gnu/libc\.so\.6$", out, re.M), f"libc in maps: {out!r}"
        assert re.search(r" /usr/lib/x86_64-linux-gnu/ld-linux-x86-64\.so\.2$", out, re.M) and re.search(r" /usr/bin/cat$", out, re.M) and "[heap]" in out and "[stack]" in out, f"maps: {out!r}"
        out, rc = w.run("linux /bin/readlink /proc/self/exe")
        assert (out, rc) == ("/usr/bin/readlink\n", 0), f"/proc/self/exe is the canonical path: {out!r}"
        # a file that root writes and anyone reads is the native file for both
        native, rc = w.run("cat /proc/net/tune")
        assert rc == 0 and "lo_drop" in native, native
        out, rc = w.run("linux /bin/cat /proc/net/tune")
        assert (out, rc) == (native, 0), f"/proc/net/tune is the same bytes for a Linux reader: {out!r} against {native!r}"
        out, rc = w.run(f"{sh} 'echo lo_dup 0 > /proc/net/tune'; cat /proc/net/tune | grep lo_dup")
        assert rc == 0 and "lo_dup 0" in out, f"a Linux shell writes a native /proc file: {rc} {out!r}"
        out, rc = w.run("linux /bin/cat /proc/hwinfo | head -2")
        assert rc == 0 and out.strip(), f"a native-only file can still be opened by name: {out!r}"


@test
def linux_proc_procps_programs():
    """The procps and util-linux programs of the Linux root work on this /proc: ps (-e with chosen columns, aux), free, uptime, pidof, pgrep, pkill, top in batch mode, vmstat, nproc,
    lscpu and kill; their output is checked by shape (headers, columns, plausible numbers, the processes that have to be listed)"""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    with Guest("t-linux-proc-procps", []) as w:
        sh = "linux /bin/sh -c"
        problems = []

        def check(what, cond, shown):
            if not cond:
                problems.append(f"{what}: {shown!r}")

        out, rc = w.run("linux /bin/ps -eo pid,ppid,stat,comm", timeout=120)
        rows = [l.split(None, 3) for l in out.splitlines()]
        check("ps -eo header", rc == 0 and rows and rows[0] == ["PID", "PPID", "STAT", "COMMAND"], out)
        by_pid = {r[0]: r for r in rows[1:] if len(r) == 4}
        check("ps lists init (pid 1, parent 0)", "1" in by_pid and by_pid["1"][1] == "0", out)
        check("ps lists the native shell and itself", any(r[3] == "gapsh" for r in by_pid.values()) and any(r[3] == "ps" and r[2].startswith("R") for r in by_pid.values()), out)
        out, rc = w.run("linux /bin/ps aux", timeout=120)
        lines = out.splitlines()
        check("ps aux header", rc == 0 and lines and re.match(r"USER\s+PID\s+%CPU\s+%MEM\s+VSZ\s+RSS\s+TTY\s+STAT\s+START\s+TIME\s+COMMAND", lines[0]), out)
        check("ps aux rows: root's init and ps itself with numeric columns", any(re.match(r"root\s+1\s+\d+\.\d\s+\d+\.\d\s+\d+\s+\d+\s", l) for l in lines[1:]) and any(re.search(r"\sR\S*\s.*ps aux$", l) for l in lines[1:]), out)
        out, rc = w.run("linux /bin/free -m", timeout=60)
        m = re.search(r"^Mem:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", out, re.M)
        check("free -m: header, Mem and Swap rows with plausible numbers", rc == 0 and re.search(r"total\s+used\s+free\s+shared\s+buff/cache\s+available", out) and m and int(m.group(1)) >= 100 and
              int(m.group(2)) + int(m.group(3)) <= int(m.group(1)) + 1 and re.search(r"^Swap:\s+0\s+0\s+0", out, re.M), out)
        out, rc = w.run("linux /bin/uptime", timeout=60)
        check("uptime: the time, how long, users and three load averages", rc == 0 and re.search(r"\d\d:\d\d:\d\d up .*\d+ users?, +load average: \d+\.\d\d, \d+\.\d\d, \d+\.\d\d", out), out)
        out, rc = w.run(f"{sh} 'echo $$; pidof sh'", timeout=60)
        parts = out.split()
        check("pidof: the pid of the shell that runs it is among the pids of sh", rc == 0 and len(parts) >= 2 and parts[0] in parts[1:], out)
        out, rc = w.run("linux /bin/pgrep -x init", timeout=60)
        check("pgrep -x init prints 1", (out, rc) == ("1\n", 0), (out, rc))
        out, rc = w.run("linux /bin/pgrep -l -x gapsh", timeout=60)
        check("pgrep -l lists the native shell", rc == 0 and re.search(r"^\d+ gapsh$", out, re.M), (out, rc))
        out, rc = w.run(f"{sh} 'sleep 100 & p=$!; pkill -x sleep; wait $p; echo status=$?'", timeout=60)
        check("pkill -x sleep ends the sleep with SIGTERM (status 143)", "status=143" in out, out)
        out, rc = w.run(f"{sh} 'kill -0 $$ && echo alive; kill -l 15; kill -s HUP $$ ; echo not-reached'", timeout=60)
        check("kill: signal 0 finds the shell, -l names signal 15, a HUP ends it", "alive" in out and "TERM" in out and "not-reached" not in out, out)
        out, rc = w.run("linux /bin/nproc", timeout=60)
        check("nproc prints 1", (out, rc) == ("1\n", 0), (out, rc))
        out, rc = w.run("TERM=dumb linux /bin/top -b -n1", timeout=120)
        check("top -b -n1: the summary lines and a row for top itself", rc == 0 and re.search(r"^top - \d\d:\d\d:\d\d up ", out, re.M) and re.search(r"^Tasks:\s+\d+ total,\s+\d+ running", out, re.M) and
              re.search(r"^%Cpu\(s\):", out, re.M) and re.search(r"^MiB Mem :", out, re.M) and re.search(r"^\s*PID USER\s+PR\s+NI\s+VIRT\s+RES\s+SHR S\s+%CPU\s+%MEM\s+TIME\+ COMMAND", out, re.M) and
              re.search(r"^\s*\d+ root\s+.*\s[RS]\s+.*\stop$", out, re.M), out)
        out, rc = w.run("linux /bin/vmstat 1 2", timeout=120)
        vm = [l.split() for l in out.splitlines()]
        check("vmstat 1 2: two header lines and two rows of 17 or 18 numbers (4.0.x adds a guest column)", rc == 0 and len(vm) == 4 and "procs" in out and all(len(r) in (17, 18) and all(x.isdigit() for x in r) for r in vm[2:]), out)
        out, rc = w.run("linux /bin/lscpu", timeout=120)
        check("lscpu: architecture, CPUs and the vendor", rc == 0 and re.search(r"^Architecture:\s+x86_64", out, re.M) and re.search(r"^CPU\(s\):\s+1$", out, re.M) and re.search(r"^Vendor ID:\s+\S+", out, re.M), out)
    assert not problems, "\n".join(problems)
