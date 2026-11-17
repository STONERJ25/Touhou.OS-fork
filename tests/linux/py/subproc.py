"""The subprocess module: run, check_output, shell=True, pipes between processes, the environment and the working directory, return codes and signals, time-outs, and the
errors of a missing program."""
import os
import signal
import subprocess
import sys


def show(label, value=""):
    print(f"{label}: {value}")


r = subprocess.run(["/bin/echo", "hello", "world"], capture_output=True, text=True)
show("run echo", (r.returncode, r.stdout, r.stderr))
r = subprocess.run(["sh", "-c", "echo out; echo err >&2; exit 3"], capture_output=True)
show("run sh", (r.returncode, r.stdout, r.stderr))
r = subprocess.run("echo $((6 * 7)); echo $0", shell=True, capture_output=True, text=True)
show("shell=True", (r.returncode, r.stdout.split()))
r = subprocess.run("echo a | tr a-z A-Z | cat", shell=True, capture_output=True, text=True)
show("a shell pipeline", r.stdout)
show("check_output", subprocess.check_output(["/bin/echo", "checked"]))
try:
    subprocess.check_output(["sh", "-c", "echo before; exit 4"])
except subprocess.CalledProcessError as e:
    show("CalledProcessError", (e.returncode, e.output, e.cmd[-1]))
try:
    subprocess.run(["false"], check=True)
except subprocess.CalledProcessError as e:
    show("check=True", e.returncode)
show("getoutput", subprocess.getoutput("echo hi; echo there"))
show("getstatusoutput", subprocess.getstatusoutput("echo x; exit 6"))
show("call", (subprocess.call(["true"]), subprocess.call(["false"]), subprocess.call("exit 17", shell=True)))

print("--- input and output")
r = subprocess.run(["cat"], input="some input\nsecond line\n", capture_output=True, text=True)
show("cat", r.stdout)
r = subprocess.run(["wc", "-c"], input=b"x" * 100000, capture_output=True)
show("wc -c of 100000 bytes", r.stdout.strip())
big = bytes(range(256)) * 4096                                   # 1 MB through a pipe and back: more than a pipe holds, so both directions must be served
r = subprocess.run(["cat"], input=big, capture_output=True)
show("1 MB through cat", (len(r.stdout), r.stdout == big))
r = subprocess.run(["sh", "-c", "cat; echo done >&2"], input=b"in", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
show("stderr into stdout", r.stdout)
with open("outfile.txt", "w") as f:
    subprocess.run(["echo", "to a file"], stdout=f)
show("stdout to a file", open("outfile.txt").read())
with open("outfile.txt") as f:
    show("stdin from a file", subprocess.run(["cat"], stdin=f, capture_output=True, text=True).stdout)
r = subprocess.run(["cat"], stdin=subprocess.DEVNULL, capture_output=True)
show("stdin from /dev/null", r.stdout)
r = subprocess.run(["sh", "-c", "echo $FOO $BAR"], env={"FOO": "one", "BAR": "two", "PATH": "/usr/bin:/bin"}, capture_output=True, text=True)
show("env", r.stdout)
os.mkdir("sub")
r = subprocess.run(["sh", "-c", "pwd | sed 's#.*/##'"], cwd="sub", capture_output=True, text=True)
show("cwd", r.stdout)
r = subprocess.run(["ls", "-1", "sub"], capture_output=True, text=True)
show("ls of an empty directory", repr(r.stdout))

print("--- Popen")
p = subprocess.Popen(["cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p.stdin.write(b"line one\n")
p.stdin.flush()
show("readline", p.stdout.readline())
p.stdin.write(b"line two\n")
p.stdin.close()
show("rest", p.stdout.read())
show("wait", p.wait())
p = subprocess.Popen("sleep 30", shell=True)
show("poll of a running process", p.poll())
p.terminate()
show("terminate", p.wait())
p = subprocess.Popen(["sleep", "30"])
p.kill()
show("kill", p.wait())
p = subprocess.Popen(["sh", "-c", "kill -9 $$"])
show("killed itself", p.wait())
p = subprocess.Popen(["sh", "-c", "trap 'exit 33' TERM; kill -TERM $$; sleep 5"])
show("trap", p.wait())
p = subprocess.Popen(["sleep", "30"])
p.send_signal(signal.SIGINT)
show("SIGINT", p.wait())
p1 = subprocess.Popen(["printf", "banana\\napple\\ncherry\\n"], stdout=subprocess.PIPE)
p2 = subprocess.Popen(["sort"], stdin=p1.stdout, stdout=subprocess.PIPE)
p1.stdout.close()
p3 = subprocess.Popen(["head", "-2"], stdin=p2.stdout, stdout=subprocess.PIPE)
p2.stdout.close()
show("a pipeline of three", (p3.communicate()[0], p1.wait(), p2.wait()))
p = subprocess.Popen(["sh", "-c", "echo to-stdout; echo to-stderr >&2"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
show("communicate", p.communicate())
p = subprocess.Popen(["sh", "-c", "exit 12"])
show("returncode before and after", (p.returncode, p.wait(), p.returncode))
with subprocess.Popen(["echo", "context manager"], stdout=subprocess.PIPE, text=True) as p:
    show("with", p.stdout.read())
show("pid", p.pid > 0)

print("--- time-outs")
try:
    subprocess.run(["sleep", "30"], timeout=0.5)
except subprocess.TimeoutExpired as e:
    show("TimeoutExpired", (e.timeout > 0, e.cmd))
p = subprocess.Popen(["sleep", "30"])
try:
    p.wait(timeout=0.3)
except subprocess.TimeoutExpired:
    show("wait with a time-out", "expired")
p.kill()
p.wait()
print("--- errors")
for argv in (["/nonexistent/program"], ["nonexistent-program-name"], ["/etc"], ["/etc/passwd"]):
    try:
        subprocess.run(argv)
    except OSError as e:
        show(f"run {argv[0]}", f"{type(e).__name__} errno={e.errno}")
try:
    subprocess.run(["true"], cwd="no/such/dir")
except OSError as e:
    show("missing cwd", type(e).__name__)
r = subprocess.run("/nonexistent/x", shell=True, capture_output=True, text=True)
show("shell: command not found", (r.returncode, "not found" in r.stderr or "No such file" in r.stderr))

print("--- descriptors")
r, w = os.pipe()
os.set_inheritable(w, True)
p = subprocess.run(["sh", "-c", f"echo through-fd >&{w}"], pass_fds=(w,))
os.close(w)
show("pass_fds", os.read(r, 100))
os.close(r)
r, w = os.pipe()
p = subprocess.run(["sh", "-c", f"(echo leaked >&{w}) 2>/dev/null || echo closed"], capture_output=True, text=True)
show("close_fds (the default)", p.stdout)
os.close(r)
os.close(w)
p = subprocess.run(["sh", "-c", "kill -0 $PPID && echo parent-alive"], capture_output=True, text=True)
show("the parent is there", p.stdout)
p = subprocess.run([sys.executable, "-c", "import os; print(os.getsid(0) == os.getpid())"], capture_output=True, text=True, start_new_session=True)
show("start_new_session", p.stdout.strip())
p = subprocess.run([sys.executable, "-c", "import os; print(os.getpgrp() == os.getpid())"], capture_output=True, text=True, process_group=0)
show("process_group", p.stdout.strip())
p = subprocess.run(["sh", "-c", "umask"], capture_output=True, text=True, umask=0o077)
show("umask", p.stdout.strip())
p = subprocess.run([sys.executable, "-c", "print('a child python')"], capture_output=True, text=True)
show("python in python", p.stdout.strip())
p = subprocess.run([sys.executable, "-c", "import sys; sys.exit(5)"])
show("python exit code", p.returncode)
p = subprocess.run([sys.executable, "-c", "raise SystemExit('message')"], capture_output=True, text=True)
show("python SystemExit", (p.returncode, p.stderr))
p = subprocess.run([sys.executable, "-c", "1/0"], capture_output=True, text=True)
show("python traceback", (p.returncode, p.stderr.strip().splitlines()[-1]))
p = subprocess.run([sys.executable, "-c", "import os; os.kill(os.getpid(), 11)"], capture_output=True)
show("python killed by SIGSEGV", p.returncode)
print("--- done")
