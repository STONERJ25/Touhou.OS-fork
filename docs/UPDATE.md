# Updates over the network: Yukari's delivery gap

The owner's goal: stop reflashing the stick for every new build. The development PC runs a small server that offers the
files an installed TouhouOS boots from, together with a manifest signed by a key that only the owner holds; the OS fetches
them over the LAN, checks the signature and every hash, and swaps the files on its EFI System Partition (ESP) in a way that
survives a power cut and falls back to the previous kernel when the new one does not come up.

| Half | Where | State |
|------|-------|-------|
| Server, manifest, signing | `tools/serve-update.py`, `tests/host/test_serve_update.py` | done (this page), host-tested |
| Client `update URL` on the OS | not written | task 5 of `docs/CLOUD_AGENT_BRIEF.md` |
| Loader fallback to `kernel.old` | not written (`boot/main.c`) | task 5; the design is below |

## Quick start (on the development PC)

```
python tools/serve-update.py keygen                     # once: ~/.touhou-os/update-signing-key.pem (private; back it up)
python tools/serve-update.py pubkey --format c          # the public key as a C header, to bake into the image (task 5)
python tools/build.py                                   # build/esp/ holds what will be offered
python tools/serve-update.py serve                      # http://0.0.0.0:8080/ ; prints the LAN address to give the OS
python tools/serve-update.py check http://127.0.0.1:8080/   # from another terminal: fetch and verify everything, like the OS will
```

Other commands: `manifest --out FILE` (sign without serving), `verify FILE [--pub KEY] [--src DIR]` (check a manifest and,
with `--src`, the files in a directory). `serve` and `manifest` take `--src DIR` (default `build/esp`), `--cmdline TEXT`,
`--no-cmdline`, `--version V` (default `git describe --always --dirty --tags`), `--issued SECONDS`; `serve` takes `--bind`
(default `0.0.0.0`; an address with `:` listens on IPv6) and `--port` (default 8080, 0 picks a free one). The server reads
the files once at start and serves exactly those bytes, so rebuilding while it runs changes nothing: restart it to publish
a new version. It needs the Python package `cryptography` and runs on Linux and Windows.

**The private key.** `keygen` writes a PKCS#8 PEM file with mode 0600 (`--encrypt` adds a passphrase, which the other commands
ask for or take from `TOUHOU_UPDATE_KEY_PASSPHRASE`). The path is `--out`, else `$TOUHOU_UPDATE_KEY`, else
`~/.touhou-os/update-signing-key.pem`; every command looks for the key in the same order (`--key` overrides). It refuses a path
inside the repository that git does not ignore, and it never replaces an existing key (images carrying the old public key would
stop accepting updates). `.gitignore` also lists `*update-signing-key*` and `.touhou-os/` as a second fence. Losing the key, or
having it stolen, means building an image with a new public key and flashing it the old way once.

**`cmdline.txt`.** `build/esp/cmdline.txt` is rewritten by every test run, so with the default `--src` the manifest carries
the default command line of `tools/mkimage.py` (`video=native blk.ro`) instead. **This replaces the installed command line**,
including words like `tz=EST5EDT,M3.2.0,M11.1.0`: pass the full line with `--cmdline "..."`, or `--no-cmdline` to leave the
installed `cmdline.txt` alone. With another `--src`, its `cmdline.txt` is used as it is.

## The manifest, format 1

An example (the values are illustrative; `manifest` prints a real one):

```
touhou-update 1
version 6df1c46-dirty
issued 1792866567
key 059309b9949415bb
file EFI/BOOT/BOOTX64.EFI 61440 6e1dd1cea0838fd30330930f4f5793b22a4a81333002837a9e10949865d782d0
file cmdline.txt 20 fdffa0a2ed6bf1c5a20afd074c0271eb4fc6ce1cc1acceca8a7b9083a3e1308d
file initrd.img 9437184 8cb98da706b53095cd217273ff6979674c8a84be52cb0d6a3968c5b5025ce3c5
file kernel.elf 1048576 c7c7fa131758d2e97e2ca28c304f8e323c66a81b60d862e6f9b29eef63e058df
end
signature 7a834581720c02d73dc942b7cd499d7b16dd600a893202a1633a29bb36effc1ff537c10089e09e7c6337641745d84d3d46e8e14ce71794eb52d5102247d98446
```

Exactly one byte sequence is valid for a given content, so a parser on the OS can be strict and small:

- Printable ASCII (0x20-0x7E) and LF only; no CR, no tab, no empty line, single spaces between fields, no trailing space, the
  file ends with LF. At most 4096 bytes in all.
- Line 1 `touhou-update 1`. Line 2 `version V`, V matching `[A-Za-z0-9][A-Za-z0-9._+-]{0,63}` (informational, shown to the user).
  Line 3 `issued N`, Unix seconds in decimal without leading zeros (at most 16 digits): the anti-rollback number. Line 4
  `key K`, 16 lowercase hex digits: the first 8 bytes of SHA-256 over the 65-byte public key (below).
- Then 1 to 16 lines `file PATH SIZE SHA256`, **in strictly ascending byte order of PATH** (so no duplicates). PATH is 1 to 8
  components joined by `/`, each matching `[A-Za-z0-9_][A-Za-z0-9._-]{0,63}`, at most 128 bytes: no leading `/`, no `.` or `..`,
  no backslash, no empty component. SIZE is decimal without leading zeros, at most 12 digits. SHA256 is 64 lowercase hex digits.
  Today the four paths are always `EFI/BOOT/BOOTX64.EFI`, `cmdline.txt`, `initrd.img`, `kernel.elf` (three if `--no-cmdline`).
- Then the line `end`. **The body, the signed bytes, is everything from the first byte up to and including the LF after `end`.**
- The last line is `signature ` and 128 lowercase hex digits: ECDSA over P-256 (secp256r1) with SHA-256 of the body, the
  signature as `r || s`, each 32 bytes big-endian (not DER). Nothing may follow its LF.

**The public key** that goes into the image is the uncompressed point `04 || X || Y` (65 bytes): `pubkey --format hex` prints
it, `--format c` prints `touhou_update_pubkey[65]` and `TOUHOU_UPDATE_KEY_ID`. These fit the primitives that exist in `libtls`
on the `parked/network-https` branch (`user/lib/tls/src/tlsint.h`), so the client needs no new crypto:

```c
/* body, body_len: the bytes up to and including "end\n"; sig: the 64 bytes decoded from the signature line */
u8 h[32];
tlsc_hash_once(TLSC_SHA256, body, body_len, h);
if (tlsc_ecdsa_verify(TLSC_P256, touhou_update_pubkey, 65, h, 32, sig, 32, sig + 32, 32) != 0) refuse();
```

`tlsc_ecdsa_verify` takes `r` and `s` as plain big-endian bytes and rejects zero or out-of-range values, which is why the
signature is raw `r || s` rather than DER. Both low and high `s` are valid; nothing depends on the signature being unique.
The client compares the `key` line with its own key id first only to say "signed by another key" instead of "bad signature".

## The HTTP side

`GET` or `HEAD`, HTTP/1.0 (one request per connection), always a `Content-Length`:

| Path | Answer |
|------|--------|
| `/manifest` | the signed manifest, `text/plain; charset=us-ascii` |
| `/files/PATH` | the file, `application/octet-stream`, for a PATH that is in the manifest, compared byte for byte |
| `/` | a short plain-text index |
| anything else | 404 |

A query string is ignored. Nothing is percent-decoded or normalised, so `/files/../x`, `/files/%2e%2e/x` and files that
are in the source directory but not in the manifest can never be reached (the tests try a list of such paths). There are no
ranges and no compression: the files are small and the client should be simple. The server logs one line per request to
standard error.

## Threats this covers, and those it does not

The LAN is not trusted: anyone on it can read and change the traffic. The signature covers the list, sizes and hashes (and
with them the kernel command line, so `blk.ro` cannot be stripped in transit), and each file is checked against its hash
before it is used, so a changed byte anywhere is refused. Replaying an older signed manifest is refused by `issued` (the
client keeps the last one it installed). Not covered: someone withholding updates (the client only learns that the server
did not answer), confidentiality (nothing here is secret), a stolen signing key (reflash with a new key), and a broken build
that is correctly signed (that is what the fallback below is for). The key has no expiry and format 1 has one key; a later
format could carry a list of accepted key ids.

## The intended update protocol (the client, task 5)

The ESP is never mounted by the policy today (only `HUMANVILLAGE` is), so the client, run as root, mounts it read-write for the
update and unmounts it at the end; on an installed disk under `blk.ro` that takes the ESP write grant (task 10, "Writing the ESP" below).
The steps, with the reason for each (steps 4 and 5 as written here leave a moment without `kernel.elf`; "Writing the ESP" below has an
order without that moment, which the client should use):

1. **Fetch and verify the manifest** (at most 4096 bytes): strict parse, key id, signature. Compare `issued` with the installed
   manifest (`update.mf` on the ESP, written by the previous update; none on a fresh install): refuse an equal or older one
   unless `--force`. Nothing is written yet.
2. **Plan.** Files whose installed copy already has the listed size and SHA-256 are skipped. Check the free space: the worst case
   is the current files, the `.new` files and the `.old` files at once, and the ESP is 64 MiB. Not enough room: stop here.
3. **Download each changed file to `PATH.new`** (`kernel.elf.new`, `initrd.img.new`, `cmdline.txt.new`, `EFI/BOOT/BOOTX64.EFI.new`),
   reading at most SIZE bytes, then flush, then **read it back from the disk and check size and SHA-256 again** (this catches a bad
   write as well as a bad download). Any failure: delete the `.new` files and stop; the installed system is untouched.
4. **Keep the previous set**: delete old `kernel.old` / `initrd.old` / `cmdline.old`, rename `kernel.elf` to `kernel.old`, `initrd.img`
   to `initrd.old`, `cmdline.txt` to `cmdline.old` (only for files that change). The kernel and its initrd come from the same
   build (syscall numbers move between builds), so they are kept and restored as a pair, not the kernel alone. The `.old` names
   are 8.3 names, so the loader can open them through the firmware without long-name support.
5. **Rename each `PATH.new` to `PATH`**, the loader (`BOOTX64.EFI`) last. A power cut between steps 4 and 5 leaves `kernel.elf`
   missing and `kernel.old` present: the loader's fallback (below) boots the previous set.
6. **Arm the trial**: write `update.try` (one line: `issued` and `version` of the new manifest) and copy the new manifest to
   `update.mf`; flush, unmount the ESP, ask for a reboot.

**The loader cannot be protected this way.** The firmware always starts `EFI/BOOT/BOOTX64.EFI`, so a broken new loader means
booting from the stick once. The client should skip the loader when its hash did not change (the usual case) and could offer
`--no-loader`.

## Writing the ESP (task 10: the ESP write grant and a power-cut-safe order)

Under `blk.ro` an installed system's ESP is read-only; root gets it read-write for one mount through the **ESP write grant**
(`BLKESPGRANT` on the ESP's partition node, then `mount -o rw,espgrant`; rules, scope and release in [INSTALL.md](INSTALL.md), "The ESP
write grant"). The grant ends with the unmount or with the program, so a crashed updater leaves the ESP mounted read-only, not writable.
`user/bin/kappabox/espgrant.h` has the pieces for the client, and the `espgrant` applet uses them:

```c
EspSession s;
char err[400];
if (esp_open(&s, NULL, 0, err, sizeof err)) fail(err);          // root; the grant and a read-write mount on /BambooForest/espgrant.PID
EspFile f[] = { { "kernel.elf", k, klen }, { "initrd.img", i, ilen } };
if (esp_update(&s, f, 2, 1, say, err, sizeof err)) fail(err);    // the sequence below; 1 = keep the current files as .old
esp_close(&s);                                                   // sync, unmount, hand the grant back
```

**The sequence** (`esp_update()`; `espgrant [-k] put SRC NAME [SRC NAME]...` from the shell):

1. Check the free space: the new files, plus the `.old` copies with `-k`, next to the current files. Not enough: stop, nothing changed.
2. **Stage**: write every file as `NAME.new`, then `sync`. Data, FAT and directory entry reach the disk in that order (the FAT cache's
   flush classes), with a device cache flush between them.
3. **Compare from the disk**: unmount, take a fresh grant, mount again (the page cache is gone), and compare every `NAME.new` byte for
   byte with what it should hold. Any difference: delete the `.new` files and stop; nothing else changed. (The client compares the
   SHA-256 of the manifest here as well.)
4. **Keep** (`-k`): copy each current `NAME` to `OLD.new`, `sync`, rename `OLD.new` over `OLD` (`kernel.elf` -> `kernel.old`,
   `initrd.img` -> `initrd.old`, `cmdline.txt` -> `cmdline.old`, `EFI/BOOT/BOOTX64.EFI` -> `EFI/BOOT/BOOTX64.old`: the extension
   replaced, so the names stay 8.3), `sync`. A copy rather than a rename, so `NAME` exists at every moment.
5. **Replace**: for each file, the loader (`EFI/...`) last, rename `NAME.new` over `NAME` and `sync` before the next one.
6. Unmount, close the grant.

**The rename over an existing file** is what makes step 5 safe, and it needed a change in the FAT driver (`fatd_replace_file()` in
`kernel/fs/fat_dir.c`, called by `fat_rename()`). Before, the new entry went into free slots, the target's old entry was marked deleted
and its clusters freed, all in the cache, and the cache writes the FAT before the directory: a cut in between left `kernel.elf` pointing
at clusters the FAT already called free. Now, when the new entry set has as many slots as the replaced one (always, for the same name):
(1) the source's old entry is removed and everything the cache holds is flushed; (2) the target's entry is rewritten **in place** to point
at the source's clusters and flushed; (3) only then are the target's old clusters freed. A cut after (1) leaves the old file and the new
data as lost clusters; after (2) the new file and the old data as lost clusters; never a missing name, a cross-link or an entry that points
at free clusters. Names whose entry sets differ in size (a long name over a short one) take the old path.

What a cut leaves, step by step (checked by `espgrant_power_cut`, see Tests):

| Cut during | `NAME` on the ESP | Leftovers the checker may report | Boots |
|------------|-------------------|----------------------------------|-------|
| 2, writing or flushing `NAME.new` | old | a partial `NAME.new` or none, lost clusters, FAT copy 2 behind, stale FSInfo, the dirty flags | old |
| 3, comparing | old, `NAME.new` complete | (clean, or the dirty flags) | old |
| 4, keeping | old; `OLD` the previous copy or the new one | `OLD.new`, lost clusters | old |
| 5, inside the rename, before the in-place write | old, `NAME.new` gone | its clusters lost | old |
| 5, after the in-place write | new | the old clusters lost until freed, FSInfo stale | new |
| 5, after the rename | new | FAT copy 2 behind, stale FSInfo, the dirty flags | new |

With two files (kernel and initrd) a cut between their renames leaves a new kernel with the old initrd. Both are complete, so the
loader starts the kernel, but they belong to different builds; that is a failed trial for the fallback below. So the client should
**arm the trial (`update.try`) and keep the `.old` set before the first rename**, not after the last one as step 6 above says: then a cut
anywhere in step 5 boots on trial and falls back to the `.old` pair if the mix does not come up.

**After a cut the ESP is flagged dirty** (the flags are set before the first write and cleared by every `sync` that finds nothing left to
write). The loader does not care, but the next `espgrant` mount fails with "was not cleanly unmounted" until the volume has been checked
or `-f` is given (it then stays flagged). The client should run the dirty-volume check of the `parked/usb-dirty-fsck` work (on
`cloud/staging`) before it writes; given the table above, `-f` after a cut is safe in practice but leaves lost clusters behind.

The loader's **"confirm this boot"** step (fallback step 3 below) is `espgrant rm update.try` (or `esp_open()`, `unlink`, `esp_close()`).

## The fallback in the loader (task 5)

What the loader does at each start, using the NVRAM trick of `start_count()` in `boot/main.c` (a non-volatile variable counting
starts):

1. No `update.try` on the ESP: normal start (`kernel.elf`, `initrd.img`, `cmdline.txt`). If `kernel.elf` is missing or not a valid
   ELF (a cut between steps 4 and 5) and `kernel.old` exists, boot the old set and add `update.fellback` to the command line.
2. `update.try` exists: read one fixed-name variable (for example `TouhouUpdateTry`, holding a hash of the `update.try` contents
   and a count). A different hash means a new trial: count from 0. Increment and write it back. While the count is at most N
   (2 is a fair default; a word `update.tries=N` in `cmdline.txt` could change it), boot the new set with `update.trial=COUNT` added
   to the command line. Above N, boot `kernel.old` + `initrd.old` + `cmdline.old` and add `update.fellback`. One fixed variable
   name (instead of one name per update, as `start_count()` does per build) keeps NVRAM from filling up with dead counters.
   If the variable store refuses the write, do not trust the count: treat it as above N only if `kernel.old` exists, and log it.
3. **Confirming the boot** is the OS's job, since the kernel has no UEFI runtime services after the exit call: when the system is
   up (later than the kernel's own `boot complete` line: init has started and the login screen or desktop came up, so a broken
   initrd also counts as a failed update) and the command line has `update.trial`, a root step mounts the ESP, deletes `update.try`
   and unmounts (`espgrant rm update.try`, through the ESP write grant). A kernel that panics, hangs or reboots before that never confirms, and after N starts the loader takes the old set.
4. With `update.fellback` the OS tells the user (the `update` tool's status, and a kernel log line in the theme: the shrine's
   gap closed and Yukari brought the previous kernel back) and `update --rollback` renames the `.old` set back into place and deletes
   `update.try`, so the fallback is not repeated on every start.

## Tests

`python -m unittest discover -s tests/host -p "test_*.py" -v` (also `python tests/host/test_serve_update.py -v`), 31 cases, a few
seconds, no QEMU and no compiler; skipped without `cryptography`. They cover the exact canonical bytes, rejection of every
non-canonical variant (CRLF, tabs, double spaces, unsorted or duplicate files, `..` and absolute paths, upper-case hex, leading
zeros, a DER signature, trailing data), a sign/verify round trip and the OS-side check done independently, a changed byte at
every seventh position of the body, changed signatures (`r`/`s` swapped, zero, equal to the group order), a wrong key, changed,
truncated and extended files, key storage (mode 0600, no overwrite, encrypted keys, refusal inside the repository), and the
server on `127.0.0.1` port 0: files byte-exact, `HEAD`, the reference client, a server that sends a changed file, and 404 for
traversal attempts and unlisted files. The client tests in QEMU (good update, bad signature, bad hash, VM killed during the write)
belong to task 5 and will reach this server at `10.0.2.2` through QEMU's user-mode network.

The ESP side has its QEMU tests already (task 10, `python tests/run_all.py espgrant_`, `tests/case_espgrant.py`; INSTALL.md lists all
three). `espgrant_power_cut` is the "VM killed in the middle of the write" test for the sequence above: a copy of an installed NVMe disk
(`text user=root blk.ro`) boots alone, `/tmp/new.elf` is the installed kernel plus 400000 bytes (an ELF with trailing bytes still loads),
the disk's writes are throttled to 4 per second (QMP `block_set_io_throttle`), `espgrant put /tmp/new.elf kernel.elf` starts, and QMP
`quit` pulls the plug a random 0 to 2.5 s after a given step line. On the host: no checker error worse than lost clusters, a FAT copy 2
that lags, a stale FSInfo or a chain longer than its file (the dirty flags are warnings); `kernel.elf` byte for byte the old or the new
kernel; the other files unchanged; then the disk boots alone, the loader's `kernel.elf N bytes` line names that kernel, and `espgrant run
true` is refused on a dirty ESP and works on a clean one. `TOUHOU_ESP_CUT_RUNS` (default 4), `TOUHOU_ESP_CUT_SEED`, `TOUHOU_ESP_CUT_AT`
(a step: `staging`, `staged`, `synced`, `verified`, `replacing`, `replaced`) and `TOUHOU_ESP_CUT_IOPS` change it.

Results (Ubuntu 24.04, QEMU 8.2, TCG, `TOUHOU_TIMEOUT_SCALE=3`): 4 default runs (cuts after `replacing`, `staging`, `replacing`,
`verified`) pass; 16 runs cut after `replacing` (seeds 1000-1015) pass and cover every state of the table above: the old kernel with
the new data lost (seed 1012), the new kernel with the old clusters lost (1005, 1006, 1011), the new kernel with only FAT copy 2 or
FSInfo behind, and the old kernel untouched. **Control**: the same 10 seeds (1000-1009) with the in-place replacement switched off (the
FAT driver's old rename) failed once: seed 1006 left `/kernel.elf: chain runs into a free cluster` (the entry still pointed at the
old kernel, whose clusters the FAT had already freed), an ESP that does not boot.

## State when paused

Written on 2026-10-24, branch `cloud/update-server` (from `main` `6df1c46`).

- **Done:** `tools/serve-update.py` (keygen, pubkey, manifest, serve, verify, check), the format above, the host tests (all 31 pass
  on Linux with Python 3.13 and `cryptography` 50.0.1), `.gitignore` entries for signing keys, this page.
- **Not verified:** the tool on Windows (written for it: `pathlib`, no POSIX-only calls except the file mode, which Windows ignores);
  a real `build/esp` (the tests use made-up files; `python tools/build.py` could not run here before task 1); serving to another
  machine on a real LAN or to QEMU at `10.0.2.2`.
- **Not started:** everything on the OS side: the `update` tool, the baked-in public key, the loader fallback, the boot confirmation,
  the client's QEMU tests.
- **Task 10 (2026-10-24, branch `cloud/esp-grant`):** the ESP mount for the update exists: the ESP write grant, `espgrant` and
  `espgrant.h` (`esp_open`, `esp_update`, `esp_close`), the power-cut-safe sequence and the FAT rename order above, tested in QEMU
  (`espgrant_`). Not on real hardware.

## How to resume

1. Merge `parked/network-https` first (task 2): the client needs `libtls`'s SHA-256 and `tlsc_ecdsa_verify`, which are internal
   (`tlsint.h`); expose the two calls (or link the client against the library's internals) rather than writing new crypto.
2. Generate the owner's key on the owner's PC (`keygen`), then `pubkey --format c --out shared/update_pubkey.h` (a public key may be
   committed; the build could also take it from an environment variable so that forks use their own).
3. Write the client following "The intended update protocol" and the loader part of "The fallback in the loader"; the parser must
   accept exactly what `parse_body`/`split_manifest` in `tools/serve-update.py` accept (port the cases of
   `tests/host/test_serve_update.py` to the client's own test). Write the ESP with `esp_open()` / `esp_update()` / `esp_close()` of
   `user/bin/kappabox/espgrant.h` (make `update` a kappabox applet next to it), in the order of "Writing the ESP": `update.try` and the
   `.old` copies before the first rename; the boot confirmation is `espgrant rm update.try`.
4. QEMU tests: run `serve-update.py serve --bind 127.0.0.1 --port 0` from the test (or `make_server()` in-process, as the host tests
   do), point the guest at `http://10.0.2.2:PORT/`, and cover a good update, a bad signature (sign with another key), a bad hash
   (`UpdateServer` with a changed file, as in `test_tampered_file_on_the_wire_refused`), and the VM killed in the middle of the write.
