# Running programs in a guest: `tools/run-in-guest.py` and `runstatus`

A compiler that targets TouhouOS needs to run what it makes and learn how each program ended: what it printed, whether it **exited** with a code or a **signal killed** it,
whether it ran out of time. The shell cannot say that (it folds a death by signal 8 into the status 136, which a program can also exit with), and starting a guest for every
test program takes half a minute. `tools/run-in-guest.py` boots one guest, hands it as many programs as you like, and prints one JSON line per program. The first user is the
ShrineC compiler's test runner (its target `gensokyo-x86_64`); nothing in the tool knows about ShrineC.

**What it has been used for (2026-11-17).** The ShrineC test runner ran every test case that can run on a guest through it: 440 of 440, later 545 of 545, in one boot each (output and exit codes, a failed check with
its message on stderr, a division by zero as signal 8, files, `mmap` arenas). Then the compiler itself ran on TouhouOS: `marisa1` built for the native target (a 614 KB static ELF with a 372 MiB `.bss`, which
costs only what it touches since the loader leaves a `.bss` demand-zero, `9d69965`) was handed to the guest as a data file next to its own source (35 files); a driver program, itself written in ShrineC and run
with `--split` in the default 512 MB guest, copied the compiler to a 0755 file, `fork`ed and `execve`d it, ran the programs it wrote, and had it compile its own source into `marisa2`, which is byte for byte
the file it started from (the fixed point, found on Linux by their `tools/fixed_point.py --elf`): 4.3 s in the guest, no kernel report, no signal. Their driver is `tools/run_guest_selfhost.py` in the ShrineC repository.

## Using it

Build the system first (the tool boots `build/esp`; it does not build anything):

```
cd <checkout>
python tools/build.py
```

**One program** (its output on stdout, its status as ours):

```
python tools/run-in-guest.py [--timeout 10] [--stdin FILE] [--json] PROGRAM [ARGUMENT...]
```

`PROGRAM` is a native TouhouOS executable on your machine: a static ELF64 `ET_EXEC` linked at `0x400000` (`docs/COMPAT.md`, "The native personality"; `tests/rig_elf.py` makes
the smallest ones). The tool's status is the program's own, except **128+N** when signal N killed it, **124** when it ran out of time, **125** when the tool failed (no image,
the guest did not boot: a line on stderr says what) and **126** when the program could not be started (not an executable). A line on stderr names the signal. With `--split` the program's
standard output is the tool's standard output and its standard error is the tool's standard error (without it both come out on stdout, merged, in the order they were written).

**Many programs in one boot** (what the ShrineC runner uses):

```
python tools/run-in-guest.py --batch MANIFEST.json [--timeout 10] [--mkdir build] [--max-output BYTES] [--split]
```

The manifest is a JSON list (or `{"programs": [...]}`) of objects:

| key | meaning |
|---|---|
| `file` | the executable on your machine (required) |
| `name` | a label that comes back in the result (default: the file's name) |
| `args` | the arguments after the program's name, a list of strings (default none) |
| `timeout` | seconds the program may run (default `--timeout`); a program still running then is killed with SIGKILL |
| `stdin` | a file on your machine that becomes the program's standard input (default: `/dev/null`) |
| `files` | `{"relative/name": "file on your machine"}`: copied into the program's working directory before it runs |
| `argv0` | what the program sees as `argv[0]` (default: the path of its copy in the guest) |

Relative paths are taken from the current directory. Every program runs in a **fresh working directory** (`/tmp/rig/w/N`, removed afterwards; `--mkdir DIR` makes `DIR` inside it
first, for programs that write under `build/`), as the user `reimu`, one at a time, in the order of the manifest.

The tool prints **one JSON line per program, in manifest order, as each one finishes**:

```
{"name": "div", "output": "before the fault", "exit": null, "signal": 8, "timed_out": false, "ms": 4}
{"name": "ok", "output": "hi\n", "exit": 0, "signal": null, "timed_out": false, "ms": 2}
```

| field | meaning |
|---|---|
| `output` | standard output and standard error **merged**, in the order they were written (the guest keeps both in one file), as text; bytes that are not valid UTF-8 become U+FFFD and the exact bytes come as `output_b64` |
| `stdout`, `stderr` | with `--split` instead of `output`: the two streams **apart** (each as text, with `stdout_b64` / `stderr_b64` when the bytes are not valid UTF-8); the order of writes between the two is lost then |
| `exit` | the exit code 0..255, or `null` when a signal killed the program |
| `signal` | the signal number (4 SIGILL, 8 SIGFPE, 9 SIGKILL, 11 SIGSEGV ...), or `null` when the program exited |
| `timed_out` | `true` when the time limit ended it (then `exit` is `null` and `signal` is 9) |
| `ms` | how long it ran, measured in the guest |
| `truncated` | only when set: it wrote more than `--max-output` bytes (256 KiB by default) and was killed |
| `error` | only when set: the program could not be started (`cannot run '...': Exec format error`), or the guest failed while it ran (then `exit` and `signal` are `null` too) |
| `kernel` | only when set: lines the kernel wrote to the console while the program ran, such as the report of a fault |

`exit` and `signal` are never both set. A program that exits with 136 has `exit: 136, signal: null`; one that divides by zero has `exit: null, signal: 8`.

The tool's own status in the batch form is **0** when every program was run (whatever became of the programs), **1** when a guest failed (the line of the program it failed on has
`error`; the tool starts a new guest for the programs that were still waiting, up to six guests, and gives up on a guest that cannot even be set up twice in a row), 2 or 125 for
bad usage or a missing image.

`TOUHOU_TIMEOUT_SCALE` (the factor the test harness uses on slow or busy machines) multiplies every time limit, the programs' too.

### From WSL

QEMU and the firmware are the Windows ones, so the tool runs with **Windows Python**; a script that runs in WSL (the ShrineC test runner does) calls it through interop:

```
python.exe <checkout>/tools/run-in-guest.py --batch <repos>/ShrineC/build/manifest.json
```

File names of the form `/mnt/c/...` (on the command line and inside a manifest) are translated to `C:/...`, so the paths a WSL tool knows work as they are. The exit status and
the JSON lines come back through the pipe as usual.

### A tool that takes a slot by itself

QEMU guests slow each other down, so every run goes through `tools/qemu-slot.py` (two slots on this PC). The tool does that by itself; `--no-slot` skips it for a caller that
already holds a slot (`tools/qemu-slot.py` sets `TOUHOU_QEMU_SLOT_HELD` for the command it runs, which the tool checks, so nesting cannot deadlock). Under `tests/run_all.py` the
test holds the slot already.

## How it works

1. The tool copies `build/esp` (12 MB) to `build/rig-PID/` and boots it in QEMU with `text` on the kernel command line (so a shell session on the serial console logs in as
   `reimu`) and QEMU's user-mode network. The private copy means a test run in the same checkout, which rewrites `build/esp/cmdline.txt`, is not disturbed.
2. It packs the programs (each distinct file once, mode 0755), the stdin files and the extra files into one tar, writes a **manifest** for the guest, and serves both over HTTP on
   `127.0.0.1`; the guest fetches them from `10.0.2.2` with `fetch` (retrying until its network is up) and unpacks the tar under `/tmp/rig`.
3. It types one command, `runstatus -b /tmp/rig/manifest ...`. The guest applet [`runstatus`](../user/bin/kappabox/runstatus.c) runs each job: a fresh directory, standard input
   from `/dev/null` or the named file, standard output and standard error to **one output file** (so their order is kept), the program in a **process group of its own** with a time
   limit, `waitpid` for the raw status (the exit code and the signal are taken apart there, not folded), then SIGKILL for whatever is left of the group, so that a program that
   started helpers does not leak them into the next one.
4. After each program `runstatus` prints one record line on the console: `@@RS ID EXIT SIGNAL FLAGS MS LENGTH BASE64` (with `--split` a second `LENGTH BASE64` pair for standard error follows), where `BASE64` is the output (so no byte of it can be mistaken
   for the framing, and the tty's newline translation cannot touch it) and `-` stands for the one of EXIT and SIGNAL that does not apply. The tool reads the records from the serial
   console as they appear, checks the length, and prints the JSON line. Console lines that are not records (the kernel's report of a fault) go to `kernel`.
5. If the guest stops answering or goes away, the program it was running gets an `error` result with the tail of the console in `kernel`, and the remaining programs run in a new guest.

The manifest the guest reads is a sequence of NUL-terminated strings (so any argument fits): `RSM1`, the number of jobs, then for each job its id, the time limit in milliseconds
(0 = the default), the stdin file or an empty string, the number of extra files and a `DEST SRC` pair for each, the path to execute, the number of arguments and the arguments
(argument 0 first). `tools/run-in-guest.py` has `pack_manifest`; `user/bin/kappabox/runstatus.c` starts with the whole format.

`runstatus` also works by hand, one program at a time (`man runstatus`):

```
reimu@gensokyo ~ $ runstatus -t 5 /tmp/a.out one two
...                          (the program's output)
runstatus: killed by signal 8 (SIGFPE)
```

## Limits

- **One stream or two, not both.** By default standard output and standard error of a program come back merged, in the order they were written (one file in the guest, like the
  console they would share). A runner that compares them separately (ShrineC's does: exact standard output, a text the standard error must contain) uses `--split`: two files in the guest,
  `stdout` and `stderr` in the result, and the order between the two streams is lost. Keeping the order and the separation at once would need the kernel to tag each write; it does not.
- **Native programs only.** The programs run with the native system-call personality. A Linux executable needs `linux PROGRAM` (docs/COMPAT.md) and is not wired in.
- **Sequential.** Programs run one after another in one guest. For parallelism start several tools, each takes a QEMU slot.
- **Time is the guest's.** `ms` and the time limits use the guest's clock, which runs slower than a real machine under QEMU's software emulation and slower still when the PC is
  busy; give generous limits or let `TOUHOU_TIMEOUT_SCALE` do it.
- **The console is shared.** Records and the kernel's own messages use the same serial line. A kernel message cannot split a record (a record is written after the program has
  ended and been reaped), but a record that arrives damaged is reported (`error`) instead of being believed.
- **Output over the console.** The output of a program passes through the serial console as base64, and the console is slow: measured in QEMU on 2026-11-17, 400 KB took 12.5 s, about 30 KB/s
  (the guest draws every character on its framebuffer console as well). A program's output is sent when it has ended, so a program that prints 256 KiB makes the batch wait about 11 s; the default
  cap is 256 KiB per program (`--max-output`; the stall limit of the tool grows with it). Ordinary test output is a few hundred bytes.
- **Speed.** Measured with 20 programs: the guest boots in 7 s, takes 7 MB of programs in 2 s and unpacks them in 2 s, and each trivial program costs about 15 ms in the guest (fork, exec, wait).
- **Memory.** The guest has 512 MB (`--mem`); the tar and the unpacked programs live in its `/tmp` (a RAM file system), so a batch of hundreds of megabytes needs more.

## Tests

- `tests/host/test_run_in_guest.py`: the manifest, the tar and the guest manifest (a second parser reads what `pack_manifest` writes), the records (exit and signal kept apart,
  output that is not UTF-8, damaged records), the restart of a failing guest (with a fake guest), the exit status of the single form.
  `python -m unittest discover -s tests/host -p "test_run_in_guest.py" -v`
- `tests/case_rig.py` (prefix `rig_`): `rig_runstatus_applet` (the applet by hand), `rig_batch_tool` (16 generated programs in one boot: exit codes, signals from real CPU faults,
  a time-out, an output flood, standard input, arguments with spaces and quotes and non-ASCII, a file that is not a program, `files`, `--mkdir`), `rig_split_streams` (`--split`), `rig_single_form`.
  `python tools/qemu-slot.py -- python tests/run_all.py rig_`
- `tests/rig_elf.py` makes the test programs: static ELF64 files around a few hand-assembled instructions (it is also the shortest possible example of a native program).
