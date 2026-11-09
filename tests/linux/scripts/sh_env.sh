# The environment and the files a Linux program expects the system to have: env, printenv, uname, /etc/os-release, /etc/passwd, the descriptors of /proc/self/fd (the shape
# of the answer, not the numbers), /dev/null, /dev/zero, /dev/urandom, /dev/stdout. Run by a POSIX shell with an empty environment but PATH; compared with a real Linux.
echo "--- environment"
env -i A=1 B=two /usr/bin/env | sort
printenv PATH
FOO=bar sh -c 'echo "FOO is $FOO in a child"'
unset FOO; sh -c 'echo "FOO is ${FOO:-unset} in a child"'
echo "--- identity of the system"
uname -s
uname -m
cat /etc/os-release | head -4
grep -c '^root:' /etc/passwd
head -1 /etc/passwd | cut -d: -f1,3,4
echo "--- /proc/self/fd (a program's own descriptors)"
ls -l /proc/self/fd </dev/null 2>&1 | sed -n 's#^l[-rwx]* *[0-9]* *[^ ]* *[^ ]* *[0-9]* [A-Za-z]* *[0-9]* *[0-9:]* \([0-9]*\) -> \(.*\)$#\1 -> \2#p' | sed 's#/proc/[0-9]*/fd#/proc/PID/fd#; s#/proc/self/fd#/proc/PID/fd#; s#pipe:\[[0-9]*\]#pipe#'
readlink /proc/self/exe | sed 's#.*/##'
test -d /proc/self && echo "/proc/self is a directory"
test -r /proc/self/maps && echo "/proc/self/maps can be read"
grep -c '' /proc/self/maps | sed 's/[0-9][0-9]*/some/'
echo "--- devices"
ls -l /dev/null | cut -c1-10
ls -l /dev/zero | cut -c1-10
stat -c '%t:%T %F' /dev/null /dev/zero /dev/urandom
head -c 8 /dev/zero | od -An -tx1
head -c 8 /dev/urandom | wc -c
echo "visible" > /dev/stdout
echo "also visible" > /dev/fd/1
echo "gone" > /dev/null
test -c /dev/tty && echo "/dev/tty is a character device"
echo "--- paths"
cd /
pwd
cd /usr/bin && pwd
cd .. && pwd
cd /tmp/.. && pwd
cd / && ls -d bin lib lib64 usr etc dev proc tmp
readlink /bin
readlink /lib
readlink /lib64
test -x /bin/sh && echo "/bin/sh is executable"
test -e /lib64/ld-linux-x86-64.so.2 && echo "the dynamic linker is where programs name it"
ls /nonexistent 2>/dev/null; echo "status $?"
