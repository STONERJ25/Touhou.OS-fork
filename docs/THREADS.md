# Threads (Alice strings several puppets from one shrine)

Status: **work in progress**. This page is updated with every commit and says plainly what exists. Anything not listed
under "Done" does not exist yet.

## Done

* nothing committed yet (design finished, kernel work starting)

## Not done yet

* everything: kernel thread syscalls, futex, per-thread signals, libc pthreads, tests

## Design in one paragraph

A `Process` stays the thread group (address space, descriptors, cwd, umask, credentials, signal dispositions). A user
`Task` is one thread: it has its own kernel stack, FS base (TLS), signal mask, thread-directed pending set and a user
thread id (`gettid`; the first thread's id equals the pid). The futex is keyed by physical address, so it works for
private memory and for shared mappings. Joining uses the Linux scheme: the kernel zeroes a user word at thread exit and
wakes it, and `pthread_join` waits on that word. `exit_group` (and every fatal signal) terminates all threads; the last
thread out tears the process down. `exec` kills the other threads and waits until they have left the kernel.
