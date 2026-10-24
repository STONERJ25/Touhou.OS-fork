# Building and testing on Linux

The build and the QEMU test suite were written on Windows; since the `cloud/linux-tooling` work they run on Linux as well (checked on Ubuntu 24.04)
with the same commands and the same results. Nothing changed for Windows: with no environment variable set, the tools are found where they were before.

## Packages (Ubuntu 24.04)

```sh
sudo apt-get install clang-20 lld-20 llvm-20 nasm qemu-system-x86 ovmf python3 python3-pil python3-numpy python3-cryptography
export TOUHOU_LLVM_DIR=/usr/lib/llvm-20/bin
```

Use clang 20, not Ubuntu's default `clang` (18): the Windows machine builds with LLVM 22, and the awk that clang 18 makes uses more stack
per call level, so `awk_checks` (recursion 5000 deep) runs into awk's own 6 MiB stack guard. Everything else passes with either.

| Package | What it gives the build |
|---------|-------------------------|
| `clang-20` | the C compiler for loader, kernel and userland (cross-targets `x86_64-unknown-windows` and `x86_64-unknown-none-elf`; no other cross toolchain needed) |
| `lld-20` | `lld-link` (the loader, PE/COFF) and `ld.lld` (kernel and programs, ELF64) |
| `llvm-20` | `llvm-ar`, `llvm-objcopy`, `llvm-nm` |
| `nasm` | the kernel's assembly files |
| `qemu-system-x86` | `qemu-system-x86_64` (version 8.2 on Ubuntu 24.04; the Windows machine runs a newer one) |
| `ovmf` | the UEFI firmware, `/usr/share/OVMF/OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd` |
| `python3-pil`, `python3-numpy`, `python3-cryptography` | screenshots and pixel checks, audio measurements, the TLS tests (those skip without `cryptography`) |

Python 3.11 or newer is needed (24.04 ships 3.12). `pip install pillow numpy cryptography` works as well as the Debian packages. The host-side fuzzers in `tests/host/`
(`imgtest.py`, `ztest.py`) additionally need `gcc` with AddressSanitizer (`build-essential`); they are not part of `tests/run_all.py`.

Other distributions: the same tools under their names there. The firmware is looked for in the Fedora (`/usr/share/edk2/ovmf/`) and Arch (`/usr/share/edk2/x64/`) places too.

## Commands

```sh
python3 tools/build.py                                   # everything into build/ (about 35 s on 4 cores)
python3 tests/run_all.py selftest,utest,shell,desktop_boots,image_,hwaudit_stage_marks    # the smoke set (about 75 s)
python3 tests/run_all.py                                 # the whole suite, one VM at a time
python3 tests/run_all.py --retry-failed usbstor          # rerun each failure alone once: flaky or failed
python3 tools/suite.py                                   # the whole suite in parallel groups, one worktree each
python3 tools/qemu.py run                                # a window plus the serial console on the terminal
python3 tools/qemu.py boot --until "boot complete" --shot build/x.png      # headless boot and a screenshot
python3 tools/mkimage.py --zip                           # the USB stick image, build/touhou-os.img(.zip)
```

The whole suite in parallel: one `git worktree` per group (two runs in one checkout fight over `build/esp/cmdline.txt`), each with its own build;
the seven groups are listed in [CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md). `tools/suite.py` does that for you (see "Test runner" below).

## Environment variables

Each tool is found from, in order: its environment variable, the `PATH`, then the Windows install folders the project was written with.

| Variable | Meaning | Default when unset |
|----------|---------|--------------------|
| `TOUHOU_LLVM_DIR` | folder holding `clang`, `lld-link`, `ld.lld`, `llvm-ar`, `llvm-objcopy`, `llvm-nm` (e.g. `/usr/lib/llvm-18/bin`) | `PATH`, then `C:\Program Files\LLVM\bin` on Windows |
| `TOUHOU_NASM` | the `nasm` program, or the folder holding it | `PATH`, then `%LOCALAPPDATA%\bin\NASM` on Windows |
| `TOUHOU_QEMU` | the `qemu-system-x86_64` program | `PATH`, then `C:\Program Files\qemu\qemu-system-x86_64.exe` |
| `TOUHOU_OVMF_CODE` | the UEFI firmware code image (read-only pflash) | `share/edk2-x86_64-code.fd` next to QEMU, then the distribution's OVMF |
| `TOUHOU_OVMF_VARS` | the UEFI variable store template (copied to `build/<vm>-vars.fd`) | `share/edk2-i386-vars.fd` next to QEMU, then the distribution's OVMF |
| `TOUHOU_TIMEOUT_SCALE` | multiplies every wait of the test harness (`expect`, the GUI and shell helpers, the tests' own deadlines) | `1` |
| `TOUHOU_CPU` | QEMU `-cpu` model; `Skylake-Client-noTSX-IBRS` looks like the owner's i5-7500 | `max` |

The firmware code and variable images must belong together (same build, same size): set both variables or neither. Copies of the variable store in `build/*-vars.fd` survive between runs;
delete them after switching firmware.

`TOUHOU_TIMEOUT_SCALE` is for slow hosts: QEMU runs the guest with TCG (no KVM, no WHPX) everywhere, so a busy machine or a CI runner sees the guest clock crawl. `3` is what the GitHub
Actions workflow uses. It only stretches how long the harness waits; it changes nothing the tests check. Two kinds of checks are left unscaled on purpose because they measure speed rather than
wait for something: the "opening took too long" limits in `tests/case_notebook.py` and the dead-controller check in `tests/case_blockdev.py`.

## Test runner: retries, expected failures, parallel groups

### `tests/run_all.py`

```sh
python3 tests/run_all.py usb,blk_,~clock,=selftest      # filters: prefix, ~contains, =exact name; none = everything
python3 tests/run_all.py --retry-failed notebook        # after the run, each failed test runs once more, alone
python3 tests/run_all.py --list storage                 # the names a filter selects, nothing runs
python3 tests/run_all.py --json build/r.json usbstor     # also write status and seconds per test as JSON
```

The report lines are as before (`PASS name  12.3s  description`, `FAIL ...` with the error below it) and the last line still starts with
`N tests, M failed`. New kinds of lines:

| Line | Meaning | Fails the run? |
|------|---------|----------------|
| `FLAKY name` | with `--retry-failed`: failed in the run, passed when rerun alone (the usual cause is host load: mouse-driven GUI tests) | no |
| `FAIL name ... failed again alone` | with `--retry-failed`: failed twice, a real failure | yes |
| `XFAIL name` | failed, and an `@xfail` marker expected that here (the reason is printed) | no |
| `XPASS name` | passed although an `@xfail` marker expected a failure: the marker's condition is too wide, narrow it | only with `strict=True` |

The summary under the last line lists the failed tests, the flaky ones and the expected failures with their reasons. The exit status is 1 when a test
failed (after the retry with `--retry-failed`) or a strict XPASS happened, else 0. The retry runs in the same process after the whole run, one test
at a time, so a test is never retried while others run; expected failures are not retried. Without `--retry-failed` nothing changed.

### Expected failures: `@xfail`

```python
from harness import test, xfail, qemu_version

@test
@xfail(reason="QEMU before 9.0: usb-storage does not hand logical_block_size to its SCSI disk (docs/LINUX.md)",
       when=lambda: qemu_version() < (9, 0))
def usbstor_4k_blocks(): ...
```

A marker always needs a reason. `when` is called at run time; while it is true the test still runs and its result is reported as XFAIL or XPASS;
while it is false (a newer QEMU) the marker does nothing and the test is an ordinary one. Without `when` the failure is always expected; do not use
that for bugs of this project: a marker is for causes outside it (an old QEMU, a host tool), and a broken test should be fixed or fail. XPASS does not
fail the run by default because the condition describes the environment, which the test cannot know exactly (a distribution may backport a fix);
pass `strict=True` when an unexpected pass must be looked at. If `when` itself raises, the marker is ignored with a note and the test counts as an
ordinary one. `qemu_version()` (in `tools/qemu.py`) runs `QEMU --version` of the QEMU the harness uses (`TOUHOU_QEMU`, then `PATH`, ...) once per
process and returns `(major, minor, micro)`, or `(0, 0, 0)` when that fails. Today only `usbstor_4k_blocks` is marked (QEMU 8.2, see below).

### `tools/suite.py`: the whole suite in parallel worktrees

```sh
python3 tools/suite.py                          # the seven groups of the brief, plus "rest"
python3 tools/suite.py --groups usb,usbstor     # some of the named groups
python3 tools/suite.py --jobs 4                 # four groups balanced by the durations of earlier runs
python3 tools/suite.py --group selftest,utest --group image_    # groups of your own (run_all.py filters)
python3 tools/suite.py --only usbstor --jobs 2  # any of the above, limited to what --only selects
python3 tools/suite.py --print-groups           # which tests each group would run
python3 tools/suite.py --max-parallel 3         # at most three groups at once (each runs one VM, but builds first)
```

The named groups are `usb` (`usb_`), `storage`, `usbstor`, `kernel` (`blk,crash,selftest,utest,kshell,jobctl,hwaudit`), `gui` (`notebook,sketchbook`),
`desktop` (`explorer,media,apps,desktop,...,shrine`) and `tools` (`net,audio,threads,...,image`), as in the brief; a test goes to the first group whose
filters select it, and a test that no group names goes to `rest`, so a new prefix is never skipped silently (today `rest` holds the eight `install_*`
tests). Every group gets a fresh detached worktree of HEAD in a temporary folder (`--workdir` to choose it), builds once there and runs
`tests/run_all.py --retry-failed` (`--no-retry` to leave that out) on its tests, passed as exact names; two groups never share a checkout.
**Uncommitted changes are not tested** (the worktrees are made from the last commit; the tool says so when the checkout is dirty).
Output in `build/suite/<date-time>/` (`--logdir` to choose): `<group>.log` (build and test output), `<group>.json` (results), `<group>-files/` (serial
and QEMU logs and screenshots, only for a group with failures) and `summary.md`, which is also printed: one row per group and the lists of failed,
flaky and expected failures. A progress line comes every minute (`--status-every`). The worktrees are removed when a group ends and again at the
end, also after Ctrl-C or SIGTERM, which stop the groups' builds and VMs first (`--keep-worktrees` to look inside). Exit status 1 when a test failed
or a group did not finish (build failure, interrupt). Durations of every run are merged into `build/test-durations.json`; `--jobs` uses them, with the
median for tests never measured, so the first `--jobs` run is balanced only by the number of tests.

The host-side unit tests of all this (no QEMU): `python3 tests/host/test_test_infra.py -v`.

## Continuous integration

`.github/workflows/smoke.yml` runs on pushes to `main` and `cloud/**`, on pull requests against `main`, and by hand (`workflow_dispatch`): it installs the packages above on
`ubuntu-24.04`, builds with clang 20, and runs the smoke set with `TOUHOU_TIMEOUT_SCALE=3` (about two minutes in all). Serial and QEMU logs and screenshots of a failed run are kept for a week as the `smoke-logs` artifact.
The whole suite is not run there (about 25 minutes in seven parallel groups on a fast desktop); `full-suite.yml` does that.

`.github/workflows/full-suite.yml` runs the whole suite by hand (`workflow_dispatch`, with an optional list of groups such as `usb,rest`) and every
Monday at 03:23 UTC (`schedule`). A `plan` job asks `tools/suite.py --ci-plan` for the matrix: the named groups plus `rest`, with `usb` and `storage`
split into two parts each (every second test, `--part 1/2` and `2/2`), ten jobs today. Each matrix job installs the same packages as the smoke run on
`ubuntu-24.04`, builds with clang 20 and `TOUHOU_TIMEOUT_SCALE=3`, runs `tools/suite.py --run-group GROUP --part K/N` (the group's tests with
`--retry-failed`, in that checkout) and writes a job summary: counts, a table of the tests that did not simply pass (failed with the first error line,
flaky, XFAIL, XPASS) and all tests with their times. Results are kept as `results-<group>` artifacts (30 days), serial and QEMU logs and screenshots
only for a failed group as `logs-<group>` (7 days). A last `summary` job combines the results into one table for the whole run; it fails when a test
failed or when a planned group left no results (build failure, crash, time limit). A group job may take 150 minutes (GitHub's limit is 360); to split
another long group, add it to `CI_PARTS` in `tools/suite.py`, the workflow follows.

Two things to know: GitHub offers `workflow_dispatch` (the "Run workflow" button) and runs `schedule` only for workflow files on the default branch,
so the workflow appears once it is merged into `main`; and in a private repository the runner minutes count against the account's monthly allowance
(ten jobs of very roughly 20 to 50 minutes each is several hundred minutes per run, once a week): change the `cron` line or remove `schedule` if that
is too much.

## Results of the whole suite on Linux

Ubuntu 24.04, 4 cores, no KVM, QEMU 8.2.2, OVMF 2024.02, the seven groups in three worktrees side by side, `TOUHOU_TIMEOUT_SCALE=2`, 262 tests:

| Build | Passed | Failed in the full run | Alone |
|-------|--------|------------------------|-------|
| clang 20 | 259 | `notebook_eol_switch`, `notebook_unsaved_close` (mouse-driven, load) | pass |
| | | `usbstor_4k_blocks` | fails: QEMU 8.2, see below (now reported as XFAIL) |
| clang 18 | 258 | `notebook_gui_fuzz` (load), `explorer_progress_cancel` (an `ex:` log line landed inside the `ls -l` it parses) | pass |
| | | `awk_checks` | fails: clang 18, see above |
| | | `usbstor_4k_blocks` | fails: QEMU 8.2 |

`usbstor_4k_blocks` needs a QEMU newer than 8.2: there, `usb-storage` accepts `logical_block_size=4096` but does not hand it to the SCSI disk
inside it (`hw/usb/dev-storage-classic.c` passes only a few fields to `scsi_bus_legacy_add_drive()`; current QEMU passes the whole block
configuration), so the guest sees 512-byte sectors and finds no partition table. The test is right and stays as it is; it passes on the Windows
machine's QEMU 11. A newer QEMU on Linux (built from source, or a later distribution) should make it pass; not tried here. Since the test runner
work it carries `@xfail(when=qemu_version() < (9, 0))`: on QEMU 8.2 it is reported as XFAIL and does not fail the run, on QEMU 9 or newer it is an
ordinary test again. Which 9.x release first passes the block size on is not checked; if a QEMU 9.0 still fails it, raise the bound in
`tests/case_usbstorage.py`.

## State when paused

Done and verified on Ubuntu 24.04 (see the table above): `python3 tools/build.py` with clang 18 and 20, `python3 tools/mkimage.py --zip`, the smoke set
`selftest,utest,shell,desktop_boots,image_,hwaudit_stage_marks` with scale 1 and 3, the whole suite, and the GitHub Actions run. The Windows paths are untouched
except that the `PATH` is now looked at before the install folders (the order the brief asked for): a different `clang` or `qemu-system-x86_64` earlier on a
Windows `PATH` would now win over `C:\Program Files\...`. Not verified: the scripts on Windows itself (no Windows machine here).

Test runner work (task 15 of the brief, branch `cloud/test-infra`, 2026-10-24): `run_all.py --retry-failed/--list/--json` and `=exact` filters, `@xfail` with
`qemu_version()`, `tools/suite.py` and `full-suite.yml`, all described above. Verified on Ubuntu 24.04 with QEMU 8.2.2: `run_all.py --retry-failed
usbstor_4k_blocks,selftest` (XFAIL and PASS, exit 0); a throwaway test that failed once and then passed (FLAKY, exit 0) next to one that always failed
(failed again alone, exit 1); the unit tests in `tests/host/test_test_infra.py`; `tools/suite.py` with two groups of a small subset (`--group` and
`--jobs 2 --only ...`) including its cleanup after SIGTERM; the CI path (`--ci-plan`, `--run-group --only`, `--aggregate --expect-plan`) by hand; the
workflow file with PyYAML and actionlint 1.7.7 with shellcheck 0.10 (no findings). The smoke workflow passed on the branch with the new `run_all.py`.
**Not verified:** a run of `full-suite.yml` itself (it can only be started once it is on `main`), the whole suite through `tools/suite.py` (this
machine was shared with other agents), `tools/suite.py` on Windows (it uses `taskkill` there instead of process groups; not tried).

## How to resume

- `usbstor_4k_blocks` on Linux: try a QEMU 9 or newer (source build) and note the first version that passes; adjust its `@xfail` bound to it.
- After `full-suite.yml` is on `main`: start it once by hand, check the job summaries, and look at the group times; split a group that comes near the
  150-minute limit with `CI_PARTS` in `tools/suite.py`. If the runner minutes matter, make the schedule less frequent.
- `tools/suite.py --jobs N` balances well only after one full run has written `build/test-durations.json`; the CI results (`results-*` artifacts) hold
  the seconds per test too and could seed it.
- If CI time matters: GitHub's Linux runners can use KVM (`/dev/kvm` after a udev rule); the harness would need an opt-in `-accel kvm`, which also changes guest
  timing, so keep TCG the default.
- `explorer_progress_cancel` can fail under load when the Explorer's serial log interleaves with the shell output `file_size()` parses; a sturdier parse
  (look for the line that names the file) would remove that flake.
