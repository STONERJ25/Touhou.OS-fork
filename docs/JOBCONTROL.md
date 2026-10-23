# Job control, priorities, links and /proc/<pid>/fd

How a process is stopped and continued, what a terminal does about background jobs, what `waitpid` reports, what a shell
has to do on top of it, and the smaller process-control pieces that came with it (nice values, hard links, `/proc/<pid>/fd`,
`cwd` and `exe`). The kernel half is done and tested; gapsh's `jobs`/`fg`/`bg` are built on it by someone else (see
"What the shell has to do").

## Stopped processes

A process is **stopped** or running; the state belongs to the whole process, so it stops every thread
(see [THREADS.md](THREADS.md)). `Process.stopped`, `stop_sig` and the two "unreported" flags live in `kernel/include/process.h`;
the logic is in `kernel/sys/signal.c` (`job_signal`, `process_stop`, `process_continue`, `job_park`).

| signal | default action | can be caught, ignored or blocked |
|--------|----------------|-----------------------------------|
| `SIGSTOP` (19) | stop | no |
| `SIGTSTP` (20), `SIGTTIN` (21), `SIGTTOU` (22) | stop | yes |
| `SIGCONT` (18) | continue (and nothing else) | yes, but it continues the process anyway |

* **Stopping.** A stop signal whose action is the default takes effect when it is *sent*: `Process.stopped` becomes true,
  the parent gets `SIGCHLD` and a `waitpid(WUNTRACED)` wake-up. The threads of the process then park (`job_park`) at the
  next edge of user mode (the return from a system call or an interrupt). A thread that is blocked inside the kernel (in
  `read`, `poll`, a futex wait, ...) is *not* woken: it stays asleep, and when its call finishes it parks instead of
  returning to user code. So a stop never turns into `EINTR`, nothing needs restarting, and a `sleep 100` that is stopped for a
  minute simply wakes up in the middle of the stop and stays frozen until the continue. CPU-bound threads are frozen by the
  next timer interrupt.
* **A stop signal that is caught** runs its handler instead of stopping; **ignored** (`SIG_IGN`, not allowed for `SIGSTOP`)
  does nothing; **blocked in every thread** stays pending and stops the process when it is unblocked and delivered.
* **Continuing.** `SIGCONT` always continues a stopped process, even if it is blocked or ignored, discards pending stop
  signals, wakes the parked threads and makes the parent hear about it (`SIGCHLD`, `waitpid(WCONTINUED)`). If a handler is
  installed it runs afterwards. Sending a stop signal discards a pending `SIGCONT`. `SIGCONT` to a running process
  changes nothing.
* **Killing a stopped process** works: `SIGKILL` (or an `exit_group` from anywhere) wakes the parked threads and they
  leave.
* **init (pid 1) never stops.** Stop signals sent to it are dropped, so nobody can freeze the machine's reaper.
* **Orphaned process groups.** A group is orphaned when none of its members has a parent in another group of the same
  session; init does not count as such a parent (it reaps, it does not do job control, and in a text login it shares the
  shell's session). Inside an orphaned group `SIGTSTP`, `SIGTTIN` and `SIGTTOU` are dropped (nobody could ever continue
  the process), `SIGSTOP` still stops. When a process exits, or leaves its group with `setpgid`/`setsid`, every group that
  became orphaned *and holds a stopped member* receives `SIGHUP` and then `SIGCONT` (POSIX), so the stopped process wakes
  up and sees its hangup instead of sleeping forever.

## waitpid

`waitpid(pid, &status, options)` takes `WNOHANG` (1), `WUNTRACED` (2) and `WCONTINUED` (8). The status word:

| meaning | value | test |
|---------|-------|------|
| exited with `code` | `code << 8` | `WIFEXITED`, `WEXITSTATUS` |
| killed by signal `n` | `n` | `WIFSIGNALED`, `WTERMSIG` |
| stopped by signal `n` (only with `WUNTRACED`) | `n << 8 \| 0x7f` | `WIFSTOPPED`, `WSTOPSIG` |
| continued (only with `WCONTINUED`) | `0xffff` | `WIFCONTINUED` |

Every stop and every continue is reported once; a process that exits while its stop was never collected is reported as
exited. `WIFSIGNALED` is false for stopped and continued words. A parent that sets `SA_NOCLDSTOP` in its `SIGCHLD`
action gets no `SIGCHLD` for stops and continues (`waitpid` still reports them). `SIGCHLD` itself is still default-ignored.

## Terminals

A terminal belongs to a **session** and has a **foreground process group**. The kernel records both in `Tty.session` and
`Tty.fg_pgid`:

* a pty slave: when a session leader opens it (it becomes the leader's controlling terminal), the leader's session and group;
* the console: when a process without a controlling terminal calls `tcsetpgrp` on it, that process's session; the group
  is whatever `tcsetpgrp` says. (`init` and gapsh already do this.)

The rules apply to processes of the terminal's session that are **not** in the foreground group, and only while the
terminal has a foreground group that still has members (otherwise nobody is a background process):

| call | rule |
|------|------|
| `read` | `SIGTTIN` is sent to the caller's group (which stops); `EIO` instead if `SIGTTIN` is ignored or blocked by the caller, or the group is orphaned. The call goes on once the process is continued **and** in front. Re-checked after every wake-up, so a stopped reader never eats the shell's input |
| `write` | only if `TOSTOP` is set in `c_lflag` (default off): `SIGTTOU`, as above; with `SIGTTOU` ignored or blocked the write just happens; an orphaned group gets `EIO` |
| `tcsetattr`, `tcsetpgrp` | like a `TOSTOP` write, always (a shell ignores `SIGTTOU` so that it can call `tcsetpgrp` while it is in the background) |

* The line discipline sends `SIGINT` (^C), `SIGQUIT` (^\) and `SIGTSTP` (^Z, `c_cc[VSUSP]`) to the foreground group, as
  before, and echoes `^Z`. A terminal hangup (the pty master is closed) sends `SIGHUP` to the foreground group.
* `TOSTOP` is `0x20` in `c_lflag` (`<termios.h>` pulls it in from `abi.h`).
* A process outside the terminal's session is never held up by its rules (daemons that inherited the console's descriptors
  keep writing).

## /proc and ps

`/proc/<pid>/status` shows `State: T` for a stopped process (`R` running, `S` sleeping, `Z` zombie), `/proc/<pid>/stat` has the
same letter, and `ps` prints it in its state column; `ps -T` and `/proc/<pid>/threads` show `T` for every thread of a
stopped process. `kill -STOP`, `kill -CONT` and `kill -TSTP` need nothing new: the kappabox `kill` applet and gapsh's
builtin already know the names (`kill -l`).

## What the shell has to do

gapsh (or any shell) owns the terminal's job control; the kernel only provides the rules above. For an interactive shell:

1. **At start-up** make the shell a process-group leader of its own group (`setpgid(0, 0)` if it is not already) and
   `tcsetpgrp(0, getpgrp())`. Ignore `SIGTSTP`, `SIGTTIN` and `SIGTTOU` in the shell itself (`signal(sig, SIG_IGN)`), so the
   shell never stops and may call `tcsetpgrp` and `tcsetattr` from the background. Keep `SIGINT`/`SIGQUIT` handled or
   ignored at the prompt. (gapsh already does all of this.)
2. **Every job gets its own process group.** After `fork`, both sides call `setpgid(pid, pgid)` (parent: `setpgid(pid, pgid)`,
   child: `setpgid(0, pgid)`; the first process of a pipeline is the group leader, `pgid = its pid`, the others join it) so
   that the group exists whichever side runs first. In the child, **before `exec`**, put the signals back to the default:
   `SIGINT`, `SIGQUIT`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`, `SIGCHLD` (an ignored signal stays ignored across `exec`).
3. **A foreground job** gets the terminal: `tcsetpgrp(0, pgid)` (also in the child before `exec` if you want to be safe
   against the race with the shell's reads). The shell then waits with `waitpid(-pgid, &st, WUNTRACED)` for each process of
   the job: a stopped word means the job is stopped (`WSTOPSIG` says why: 20 is Ctrl-Z, 19 `kill -STOP`, 21 wanted input, 22
   wanted to write). When the job has finished or stopped, take the terminal back with `tcsetpgrp(0, shell_pgid)`, restore
   the shell's own terminal settings (save the job's with `tcgetattr` when it stops, so `fg` can restore them) and print
   `[N]+  Stopped  command` (or `Done`/`Exit N`/`Terminated`) before the next prompt.
4. **`fg %N`**: print the command, `tcsetpgrp(0, job_pgid)`, restore the job's saved settings if it was stopped,
   `kill(-job_pgid, SIGCONT)`, then wait as in 3. **`bg %N`**: `kill(-job_pgid, SIGCONT)`, mark it running, print
   `[N]+ command &`; the terminal stays with the shell. A background job that wants the terminal stops itself with
   `SIGTTIN` (reading) or `SIGTTOU` (writing with `TOSTOP`, `tcsetattr`); the shell reports `Stopped (tty input)` /
   `Stopped (tty output)` and the user brings it to the front with `fg`.
5. **Collect changes of background jobs before each prompt**: loop `waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED)`
   until it returns 0 or -1, and match the pid to a job: exited/signalled (job done, print and forget when all its
   processes are gone), stopped (mark Stopped), continued (mark Running). The kernel sends `SIGCHLD` for each of those
   events, so a handler that sets a flag works too.
6. **Signals to jobs** go to the whole group: `kill %N` is `kill(-pgid, sig)`. `kill -CONT %N` on a stopped job continues
   it. `wait %N` / `wait pid` wait for *termination*; do not use `WUNTRACED` there (or keep waiting when a stop is reported).
7. **At exit** the shell should warn about stopped jobs (`There are stopped jobs.`) the first time, and on the second
   `exit` send them `SIGHUP` and `SIGCONT`. Jobs that are `disown`ed are simply removed from the table (and, if stopped, are
   best continued first).
8. A shell running inside a pty needs nothing special: the terminal emulator makes the shell a session leader on the slave
   (`setsid`, `open`), which is what the kernel uses as the terminal's session.

### Where gapsh stands (at the time of writing, `user/bin/gapsh/`)

Already right: `main.c` ignores `SIGTSTP`/`SIGTTIN`/`SIGTTOU` when interactive, makes the shell its own group leader and
takes the terminal; `exec.c` puts every job in its own group (`setpgid` on both sides), hands it the terminal with
`tcsetpgrp(0, pgid)` and takes it back, and `reset_signals()` restores the default dispositions in the child before `exec`.
What is missing: nothing waits with `WUNTRACED`, so a stopped foreground job (Ctrl-Z, `kill -STOP`, a background
read) leaves the shell waiting for ever. The call sites to change are `job_wait_fg` (`waitpid(-1, &status, 0)`),
the foreground simple command (`waitpid(pid, &status, 0)` after `tcsetpgrp(0, pid)`), the two pipeline waits and the
`wait` builtin's neighbours in `builtins.c`; `jobs_reap` should add `WUNTRACED | WCONTINUED`; `jobs` prints only `Running`
and needs `Stopped`; there is no `bg`, and `fg` has no `SIGCONT`. Until then, Ctrl-Z in a text console freezes the job and the
prompt (the stopped job can still be killed from another terminal with `kill -9`: `SIGKILL` wakes it).

Things a shell author may trip over: a pipeline's processes can report their stops in any order, a job is stopped when its
processes are stopped, not when the first one is; `waitpid(pid, ..., WUNTRACED)` for an already collected stop returns 0
with `WNOHANG` (stops are reported once); `fg` on a job that exited between the prompt and the command finds no process
(`ECHILD`).

## Priorities

Every process has a **nice value** from -20 (greedy) to 19 (polite), 0 at boot; children inherit it, `exec` keeps it, all
threads of a process share it (`Process.nice`, copied into each `Task.nice`).

* **System calls** (`kernel/sys/priority.c`, numbers 116 and 117): `getpriority(which, who)` and
  `setpriority(which, who, nice)` with `which` = `PRIO_PROCESS` (a pid), `PRIO_PGRP` (a process group) or `PRIO_USER`;
  `who == 0` means the caller's own process, group or user. `getpriority` returns `20 - nice` (1 to 40) so that the return
  code is never negative, and the lowest nice value among the matches; libc turns it back. `setpriority` clamps to the range,
  returns `EPERM` for processes of other users (unless root) and `EACCES` when a non-root caller would *lower* a value.
* **Scheduler** (`slice_for` in `kernel/sched/task.c`): the round robin is unchanged, but a thread's time slice is
  `SCHED_SLICE` (10 ticks) at nice 0, shrinks linearly to 1 tick at nice 19 and grows by 5 ticks per step below 0 (110 at -20).
  Two busy processes therefore share the CPU in proportion to their slices: in the test a nice-19 process got about an eighth
  of what a nice-0 process got (`cpu ticks: nice 0 got 590, nice 19 got 67`). A sleeping thread uses no slice, so niceness
  only matters under competition.
* **libc and tools**: `getpriority`, `setpriority`, `nice` (`<sys/resource.h>`, `<unistd.h>`), the `nice` and `renice`
  applets (`user/bin/kappabox/priority.c`), `ps -l` (NI column), `Nice:` in `/proc/<pid>/status`. `renice` takes the new
  value itself, not an increment (the util-linux meaning).

## Hard links

`link(old, new)` (system call 118, `vfs_link` in `kernel/fs/vfs.c`) gives an existing file a second name. It links the
file itself (not what a final symlink points to), refuses directories (`EPERM`), existing names (`EEXIST`), other mounts
(`EXDEV`) and filesystems that cannot do it (`EPERM`). tmpfs implements it (`tmp_link`): every directory entry owns one
reference to the node, `st_nlink` counts the entries, `unlink` and `rename` over a name drop one, and the data goes away with
the last entry *and* the last open descriptor (an unlinked-but-open file reports `st_nlink == 0`). The root filesystem,
`/tmp` and `/dev` are tmpfs instances, each its own mount, so a link across them fails with `EXDEV`. FAT has no hard links.
The `ln` applet makes them without `-s`. Like Linux's `protected_hardlinks`, a user other than root may only link files they
own or could already read *and* write, and never set-user-id or set-group-id programs (`EPERM`): a link would keep an old
vulnerable copy of `su` alive after the owner replaced it.

## /proc/<pid>/fd, cwd and exe

* `/proc/<pid>/cwd` is a symlink to the working directory (`Process.cwd_path`), `/proc/<pid>/exe` to the program, by the
  normalised path `execve` was given (`Process.exe`; for a `#!` script it is the interpreter).
* `/proc/<pid>/fd/` is a directory with a symlink for every open descriptor, named by its number. The target is the
  absolute path the file was opened with (`File.path`, set by `open`, and `/dev/console` for init's descriptors; symlinks in the
  path are not resolved), `pipe:[N]` / `socket:[N]` (both ends of a pipe show the same `N`: the vnode), or `anon_inode:[...]`
  for things without a name such as the master side of a pty. Following the link opens the file again. The directory is
  `dr-x------` and owned by the process's user, so only that user and root can list it; `cwd` and `exe` are world readable.
  `readdir` and `readlink` look at the descriptor table with interrupts disabled, so a descriptor closed meanwhile simply
  disappears (`ENOENT`). `fuser`-like tools can be built on it.

## Tests

`python tests/run_all.py jobctl` (`tests/case_jobctl.py`) boots a text shell and runs `jctest` (`user/bin/jctest.c`, about
15 s). It covers: stop and continue of a two-thread process (both threads freeze, both resume), the `waitpid` words, `SIGCHLD`
and `SA_NOCLDSTOP`, killing a stopped process, stop signals that are ignored, caught, blocked and discarded by `SIGCONT`,
`SIGCONT` with a handler or blocked, `kill -STOP/-CONT/-TSTP` through the kappabox applet and `ps` showing `T`, Ctrl-Z
through a pty (the stopped job does not take the shell's next line, `fg` continues it, ^D ends it), background read
(`SIGTTIN`, `EIO` when ignored), `TOSTOP` writes, background `tcsetattr`, an orphaned group reading (`EIO`), the orphaned
group rule (`SIGHUP` + `SIGCONT` for a stopped member, nothing without one, `SIGTSTP` dropped, `SIGSTOP` kept), nice values
(`setpriority`/`getpriority`/`nice`, clamping, `EACCES`/`EPERM`, inheritance, `renice`, `ps -l`, `PRIO_PGRP`, and the CPU
share of a polite process), hard links (counts, shared data, unlink of one name, an unlinked open file, the applet, the
errors) and `/proc/<pid>/{fd,cwd,exe}` (files, pipes, sockets, listing, closed descriptors, another process).
The manual pages are checked by `python tests/run_all.py manual` (`nice(1)`, `renice(1)`, `ln(1)`, `ps(1)`, `kill(1)`,
`jobcontrol(7)`, `proc(5)`).

## Limits and deviations

* A thread blocked in a *pipe* or *socket* read of a stopped process still takes the data when it arrives (and then parks
  instead of returning to user code). Only terminals are guarded against this, because they are shared with the shell.
* The foreground group of the console is whatever the last `tcsetpgrp` said; there is no `TIOCSCTTY`, no `tcgetsid`, and
  `setpgid` does not check sessions or `exec` state.
* Stopping is generated at `kill` time; a signal that is already pending when the process is stopped by another signal is
  delivered after the continue.
* A stopped process still counts for `exit_group` races: a group exit or an `execve` wakes parked threads and they leave.
