# `update pull`: the second review (2026-11-24)

A second agent read the pull feature (`user/bin/kappabox/updpull.c`, the install sequence of `update.c`, `start_update_pull` in init, `tools/serve-update.py`) the hour after it
was written, looking for two things only: what could leave the machine unable to start, and what could let files in that the owner did not sign. This is what was found, what
was changed, and what is left. The design is described in [UPDATE.md](UPDATE.md); nothing here redesigns it.

Severity: **brick** (the machine does not come up without a person), **wrong files** (something unsigned or older than intended is installed), **DoS** (updates stop, or the
machine wastes time or power, but what runs is right), **cosmetic**.

## Findings

| # | Finding | Why it matters | Severity | Status |
|---|---|---|---|---|
| 1 | `update.iss` (the anti-rollback number) was written **after** the renames, in a second step. A power cut between the two left the new files on the ESP with the old number (or none). | The trial start's self-confirmation compares the manifest's `issued` with `update.iss`, so it could never confirm: two watchdog restarts and a fallback for a build that was fine. And once a person resolved that, every signed manifest between the old number and the new one was installable again. | wrong files (an older signed build), DoS | **fixed** `df94bad`: the number is written before the first rename, "done" after the last; test `update_pull_number_survives_a_power_cut` |
| 2 | A damaged, empty or missing `update.iss` read as "no pull was ever installed": any signed manifest, however old, was accepted. | One bad sector or one careless `rm` on the ESP removed the rollback protection without a word. | wrong files (an older signed build) | **fixed** `df94bad`: the number is the largest of `update.iss`, `update.mf` (when it says `pulled`) and `update.try` (when it says `pulled`); all three must be lost. Not tested on its own (the three sources are read by one function; the power-cut test covers `update.iss`) |
| 3 | `update.autoconfirm=` (empty value) parsed as 0 seconds; `update.pull.delay=` and `update.pull.interval=` likewise; a value that overflows `long` became `LONG_MAX` and was then cut to 32 bits for `sleep`. | A typo in a signed command line changed the timing of the self-confirmation (at once, before the network is up: three failed tries, then the watchdog) or of the pull loop. | DoS | **fixed** `df94bad`: digits only, at most nine; anything else keeps the default. Read, not tested in QEMU |
| 4 | `update.pull.interval=1` pulled every second, for ever. | A typo makes the machine hammer the server and its own ESP mount. | DoS | **fixed** `df94bad`: an interval between 1 and 59 is raised to 60 with a message; 0 still means once per start. Read, not tested in QEMU |
| 5 | A redirect from `https://` to `http://` was followed silently (libhttp reports it in `HttpResult.downgraded`; nobody looked). | Integrity does not depend on the transport (the signature covers everything), but an owner who writes `https` wants nobody on the way to see or stall the update. | DoS / privacy | **fixed** `df94bad`: refused ("redirected from https to plain http"). No test (it needs a TLS server in the test) |
| 6 | `restart_later` closed descriptors 0..63 only; the table's soft limit is 1024 now. | A descriptor of the ssh connection above 63 would stay open in the detached restart process and keep the push tool waiting. | cosmetic | **fixed** `df94bad`: closes up to the soft `RLIMIT_NOFILE`. Covered only by the push tests still passing |
| 7 | The deadline of a fetch is only looked at when body bytes arrive (`sink_body`). Before the body, only libhttp's silence timeout (20 s) applies. | A server that sends one header byte every 19 s holds `update auto` for as long as libhttp's header limit allows; with `update.pull.then=poweroff` the machine stays on meanwhile. Nothing is installed. | DoS | **open**: the clean fix is a whole-request deadline in `user/lib/http` (another agent's files). Not verified by experiment |
| 8 | Redirects to another host are followed (up to 10). | Harmless for integrity (signature, sizes and hashes are checked whatever host answered); it lets the configured server send the machine to fetch from somewhere else. | cosmetic | **open**, left as it is: static hosts do redirect; say if it should be refused |
| 9 | A manifest with a very large `issued` (up to 16 digits) locks out every later manifest with a smaller number, for good. | Only the owner's key can sign one, so it is a mistake, not an attack; but it cannot be undone from the server. A person at the machine removes `update.iss`, `update.mf` on the ESP. | DoS (self-inflicted) | **open: the owner's decision.** Documented in UPDATE.md. `serve-update.py` writes the time of signing unless `--issued` is given |
| 10 | Between the renames and the write of "done" a power cut leaves the number without "done". If that build then fails and is rolled back by hand, the next pull installs it once more (the files differ and the number is not marked finished). | One extra round of trial and fallback for a build that already failed once. It cannot loop: the second install writes "done". | DoS (one round) | **open**, accepted: the alternative (mark first) is finding 1 again |
| 11 | The self-confirmation proves the network stack, HTTP, SHA-256 and ECDSA of the new build, and nothing else: a build whose desktop or keyboard is broken confirms itself. | By design (UPDATE.md says what the test of health is). The previous set stays on the ESP as `.old`, and `update rollback` over ssh brings it back. | cosmetic (known limit) | **open**, documented |
| 13 | The loader counts the starts of a trial per hash of the text of `update.try` (`boot/update.c`). A push's text has the time in it; a pull's had only the version and the manifest's `issued`, so it was the same text every time the same manifest was installed. | The second install of one manifest (after a power cut during the first, findings 1 and 10) went on counting where the first stopped: one try instead of two, or straight to the previous set without ever starting the new files. Safe, but the update would never be tried. | DoS | **fixed** `1a1bdd6`: `update.try` of a pull has a line `written TIME`. Read against the loader's code, not tested on its own |
| 12 | After a power cut during an install the ESP is flagged dirty. `update recv` then needs `--force`; `update pull` has no such word. | An unattended machine may refuse every later pull until a person looks ("not cleanly unmounted"). | DoS | **fixed** (the lead's decision, 2026-11-24). First shown by the power-cut test: after the cut the next pull verified everything and stopped at "not cleanly unmounted", and an `update confirm` with nothing on trial did not clear the flag. A volume that was not cleanly unmounted is mounted with force by `update pull` and `update auto` only when the ESP itself explains the dirt: `update.iss` has `issued N` without `done` (a pull of this machine was cut short) or `update.try` is there (a trial is open). The safe sequence copes with what its own cut left. Dirt that nothing explains (another system or a crash wrote there) is still refused, with a hint; the read-only look at the number and the hashes always works. Test: `update_pull_number_survives_a_power_cut` (the pull after the cut goes through to `done`). The refusal of unexplained dirt has no test of its own |
| 14 | (This review's own bug, in the first version of the fix for finding 1.) The number was recorded through `esp_update` with the session's trial marker already set, so `update.try` was on the disk before any file was staged. | A power cut right there left the **old** files "on trial" with no previous set kept: counted starts, a watchdog, for a system nobody changed. | DoS | **fixed** `eb42fe8`. Found in the serial log of the power-cut test (it passed all the same, because it ended a trial it thought the cut had armed); the test now asserts that only `update.iss` changed |

## What was tried and held

- **The signed bytes are the parsed bytes.** `check_signature` hashes `d[0..body_len)`, and `body_len` is where the parser found the last line; every field the installer
  uses comes from inside that range. A second `signature` line, a line after it, bytes after the final newline, CR, NUL, tabs, doubled spaces, uppercase hex, leading zeros and
  `+`/`-` signs are all refused by the grammar. Duplicate paths cannot occur (strictly ascending order). The key id is inside the body and only chooses the message.
- **The two parsers agree** (see the fuzzing below): the C parser and `serve-update.py`'s read every accepted manifest identically, and refuse the same inputs.
- **Paths.** Only the four fixed names pass (`PULLABLE`), after the grammar's own rule (components of `[A-Za-z0-9_][A-Za-z0-9._-]*`): no `..`, no leading `/`, no `%`, `?`
  or `#` can reach the URL or the ESP. The names handed to the installer are the installer's own constants, not the manifest's strings.
- **The URL.** Printable ASCII without spaces, at most 159 characters, `http://` or `https://`: no CR/LF can reach the request; `join_url` always fits its buffer.
- **Sizes.** A size has at most 12 digits (no overflow in 64 bits), `issued` at most 16 (fits `long long`), and each file is held to the installer's limit (64 MiB kernel,
  256 MiB initrd, 4 MiB loader, 200 bytes of command line) before a byte is fetched; the sink refuses the first byte beyond the listed size, so a `Content-Length` lie or an
  endless body costs at most the listed size of memory. `no_decode` is set: a gzip-encoded answer fails its hash. At most four files exist, so the worst case is about
  324 MiB of buffers; `malloc` failing is reported ("out of memory") and nothing is changed. Every error branch of `do_pull`, `fetch` and `health` frees its buffers and
  unmounts the read-only ESP (read, not measured).
- **A trial cannot be confirmed by the wrong system.** `health` refuses on a start that fell back, on a start without `update.trial=`, on a start that has written the ESP
  (`/tmp/.update-written`), and when `update.try` does not say `pulled`. A fell-back start neither pulls automatically nor by hand (tested).
- **No key, no pull**: `update pull`, `update auto` and `update autoconfirm` all stop at "no signing key in this image".
- **init**: `update auto` is forked into its own session and init does not wait for it; init reaps it like every other child.
- **`--loader`**: without it a differing loader is skipped before any fetch.

Not checked for lack of time: TLS certificate handling in `user/lib/tls` as the pull uses it, chunked bodies cut in the middle (libhttp's own tests cover them), and what the
loader does on the fourth and later starts after a fallback that nobody resolved (the push tests show `update.try` stays and the third start is the previous set).

## The fuzzing

`tests/host/test_updpull_parser.py` (`python tools/hosttest.py discover -s tests/host -p "test_updpull_parser.py" -v`). For it the parser moved, unchanged, from `updpull.c`
into `user/bin/kappabox/updmanifest.c` and `.h`, which the applet and the host harness (`tests/host/updpull_parser_host.c`) both compile. gcc 13 with AddressSanitizer and
UBSan; each candidate sits in a heap block of exactly its length. A corpus of seven valid manifests written by `serve-update.py`'s encoder (the limits: 16 files, 64-character
components, 8 components, 12-digit sizes, a 16-digit `issued`), 31 hand-written cases, and 12000 mutations from three fixed seeds (bit flips, truncation, duplicated, deleted
and swapped lines, inserted NUL/CR/space, long fields, numbers at and over the limits, up to 70 extra file lines, CRLF). For each, both parsers must refuse, or both accept and
read the same version, `issued`, key id, body length, signature bytes, paths, sizes and hashes.

**Result: no disagreement, no sanitizer report.** 940 of the 12000 mutations were accepted by both (and read the same).

## The power-cut test

`update_pull_number_survives_a_power_cut` pulls the plug (QMP `quit`) right after the line "recorded issued N in update.iss", while the files are being staged. On the disk
afterwards: `update.iss` is `issued N`, nothing else of the update (no `update.try`, the old kernel). The image is started again: an older manifest is refused ("rollback
protection"); the manifest whose install was cut is not "nothing new" but "was begun here and not finished: comparing the files", and is fetched and verified; the install
is then installed to its end although the cut left the ESP flagged dirty (finding 12: the number without `done` explains the dirt), and `update.iss` says `done`. Only this one point of the sequence is
cut; the points during and after the renames follow from the order of the writes (UPDATE.md, "The anti-rollback number and a power cut") and from the push's own
`espgrant_power_cut`, and were not run with a pull.

## What was not run against the old code

The tests were written with the fixes; none of them was run against the code before the fix to watch it fail. `update_pull_number_survives_a_power_cut` waits for a line the
old code never printed and for a number the old code wrote last, so it cannot pass there; the others (`update_pull_auto_words`,
`update_pull_hung_kernel_falls_back_and_is_not_pulled_again`) test behaviour that was already right and had no test. Findings 2 to 6 and 13 are fixed by reading, without a
test of their own.
