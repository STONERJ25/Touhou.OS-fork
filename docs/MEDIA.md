# Media: archives, images, the Gensokyo Gallery

This page says what exists, what the limits are, and (plainly) what is not finished yet. Status as of the last commit on
this branch; update it when you change something.

## Status

| Piece | State |
|---|---|
| `libz` (inflate, deflate, crc32, adler32) | **done**, tested on the host against zlib (see below) and in the guest through `gzip` |
| `gzip`, `gunzip`, `zcat` | **done**, manual pages written, checked by hand in the guest |
| `utimes()` system call, `<utime.h>` | **done** (`SYS_utimes` = 82, `kernel/fs/utimes.c`); `gunzip` and `gzip` restore file times with it |
| `tar` (create, extract, list, `-z`, `-C`, `-v`, `-f`, `--strip-components`, `--exclude`) | **written**, manual page written; round trip (long paths, symlinks, modes, `-z`, pipes, truncated archive) checked by hand in the guest. Archives made by Python's `tarfile` (pax and GNU formats, long paths, unicode names, symlinks, modes, times) unpack correctly in the guest, and a `tar -czf` archive made in the guest is read correctly by Python |
| `zip`, `unzip` (store and deflate, directories, symlinks, modes, times) | **written**, manual pages written; a zip made by Python's `zipfile` unpacks and tests correctly in the guest, and a zip made in the guest passes Python's `testzip()`. `zip` cannot add to an existing archive yet (it refuses) and has no zip64 |
| `libimg` (PNG, JPEG, GIF, BMP, PNM, TGA decoders; PNG/BMP/PPM encoders; scaling and rotation) | **only a skeleton**: `img.h` (the API), format detection, loading, size limits, scale / rotate / flip / composite are written but **never run**; no decoder and no encoder exists yet, and nothing links libimg. See "State when paused" |
| Gensokyo Gallery (image viewer) | not written yet |
| User wallpaper (`~/.config/hakurei/wallpaper`, `wallpaper` command) | not written yet |
| Screenshots as PNG in `~/Pictures`, `screenshot` command | not written yet (Ctrl+F12 still writes `/tmp/screenshot.ppm`) |
| `k_image_thumbnail()` for file browsers | not written yet |
| `tests/case_media.py` | not written yet; the checks that were run so far were ad hoc |

## State when paused

The work was stopped early (the machine had to be closed), so most of the image half of the task has not been started.

**Done and working** (committed, builds, exercised): `libz`; `gzip` / `gunzip` / `zcat`; `tar`; `zip` / `unzip`; the
`SYS_utimes` system call with `utime()` / `utimes()` / `lutimes()` in libgap; manual pages for all of those applets
(`gzip` with its alias `gunzip`, `zcat`, `tar`, `zip`, `unzip`). "Exercised" means: the libz host differential run
described below (0 failures), and by hand in the guest: gzip round trips and damaged / truncated input, tar and zip round
trips, archives made by Python's `tarfile` / `zipfile` unpacked in the guest, archives made in the guest read by Python.

**Half done:** `user/lib/img` holds `include/img.h` (the intended API, complete), `src/imgint.h` and `src/image.c`
(`img_sniff`, `img_probe`/`img_decode` dispatch, `img_load`, `img_alloc`/`img_free`, `img_scale` with nearest / bilinear /
area modes, `img_rotate90`, `img_flip`, `img_fit`, `img_composite_over`). It compiles (no warnings) and goes into
`libimg.a`, but **none of it has ever been executed**, and `img_scale(..., IMG_AREA)` in particular is a first draft whose
loop bookkeeping should be re-read and tested before it is trusted. `image.c` refers to `png_probe`, `png_decode`,
`bmp_probe`, `bmp_decode`, which do not exist yet, so a program that uses libimg will not link until they are written.
`img.h` also declares `img_write_png`, `img_write_bmp`, `img_write_ppm`, `img_save_*`, which are not implemented.

**Not started:** the PNG / JPEG / GIF / BMP / PNM / TGA decoders, the PNG / BMP / PPM encoders, EXIF orientation, the
Gensokyo Gallery application and its entry in `hakurei-shell/launch.c`, the user wallpaper (`~/.config/hakurei/wallpaper`,
the `wallpaper` command, reload on change in `hakurei-shell`, `hakurei-lock`, `hakurei-greeter`), PNG screenshots in
`~/Pictures` and the `screenshot` command (fairyd still writes only `/tmp/screenshot.ppm`), `k_image_thumbnail()`,
`tests/case_media.py`, the manual pages for `screenshot` / `wallpaper` (they must exist before those applets do, because the
`manual` test fails for an applet without a page), and the mention in `docs/DESKTOP.md`.

**Known problems**
* The `manual` test (`python tests/run_all.py manual`) was run once on this branch and **failed with a boot timeout**: the guest
  printed the kernel log up to "scheduler running" and never produced the `reimu@gensokyo` prompt within 90 seconds. Boots
  with `cmdline="text"` (used for every other check here) were fine, and the test never got as far as checking the manual
  pages. Whether the cause is the host being busy with other agents' QEMU runs or one of this branch's changes is **not
  known**; re-run it first. A shorter check of the new pages (`patchouli --check-all`, `patchouli -l`) was started afterwards,
  see the final report for its result.
* `zip` cannot add to an existing archive (it refuses), no zip64; `tar` cannot extract hard links except as copies and
  does not extract devices; `gzip -l` reads only the last member's trailer.
* The libz host-test driver (a small C program plus a Python script comparing with `zlib`) is in my scratch directory, not in the
  repository. It should be added as `tests/host/` or folded into `tests/case_media.py`.
* No test file `tests/case_media.py` exists; nothing in the automated suite covers the new tools yet.

**How to resume**
1. `git merge main` if main has moved, then `python tools/build.py`.
2. Re-run `python tests/run_all.py manual` (and the `shell` test) to see whether the boot timeout was load.
3. Write `user/lib/img/src/png.c` first (inflate rows straight from IDAT chunks with `z_inflate`, unfilter, expand to ARGB;
   interlaced and tRNS; header CRCs with `z_crc32`), then `write.c` with the PNG / BMP / PPM encoders (`z_deflate` is streaming), then
   `bmp.c`, `pnm.c`, `gif.c`, `tga.c`, and `jpeg.c` last (baseline first, then progressive; the integer "islow" IDCT and
   libjpeg's fancy upsampling make the output match Pillow almost exactly). Develop each decoder on the host first: the
   libraries only need `<stdint.h>`, `<string.h>`, `<stdlib.h>` and POSIX file calls, WSL has gcc with the sanitizers and
   Python with Pillow, which is how libz was checked.
4. Then the Gallery, wallpaper and screenshot pieces from the task list, `tests/case_media.py`, and the rest of this page.

## libz (`user/lib/z`, header `<z.h>`)

A streaming DEFLATE implementation with the zlib and gzip wrappers.

* **Inflate** (`z_inflate_init`, `z_inflate`, `z_inflate_end`; formats `Z_RAW`, `Z_ZLIB`, `Z_GZIP`, `Z_AUTO`): a state
  machine, so input and output can be fed in pieces of any size (the tests used 1-byte pieces). It keeps its own 64 KiB
  window, so the caller's buffers are never read back. Tables are checked (over-subscribed or incomplete codes, bad
  lengths, distances that reach back before the start of the data, bad stored-block lengths), gzip header flags and the
  header CRC are checked, the CRC-32 / ISIZE (gzip) and Adler-32 (zlib) trailers are verified. Bytes after the end of the
  stream stay in `next_in`/`avail_in`, so concatenated gzip members can be read with `z_inflate_reset`.
  `z_uncompress()` is the one-shot form with an output cap (default 256 MiB) as a guard against decompression bombs.
* **Deflate** (`z_deflate_init(level 0..9)`, `z_deflate(flush)`, `Z_NO_FLUSH` / `Z_SYNC_FLUSH` / `Z_FINISH`): LZ77 with
  hash chains and lazy matching (greedy for levels 1 to 3, the same chain/lazy parameters as zlib), 32 KiB blocks, and for
  every block the exact bit cost of a dynamic-Huffman, a fixed-Huffman and a stored encoding is computed and the cheapest
  is written, so incompressible data costs five bytes per 32 KiB. Huffman code lengths are limited to 15 bits with the
  usual Kraft-sum repair. Level 0 writes stored blocks only. `z_compress()` is the one-shot form.
* Memory: about 400 KiB per compressor, 70 KiB per decompressor.
* Not implemented: preset dictionaries, `Z_FULL_FLUSH`, zip64-style sizes in the gzip trailer (the 32-bit ISIZE wraps, as it
  does in the format).

Host testing: the library has no system dependencies, so it was also compiled on a Linux host (WSL, gcc 13) with
`-fsanitize=address,undefined` and compared with Python's `zlib` / `gzip` modules. The run that was made: 11 inputs (empty,
one byte, short text, 768 bytes, 100 000 zeros, 70 000 random bytes, word soup, source files, a two-letter alphabet, a
sequence with long periodic repeats) x levels 0, 1, 3, 6, 9 x raw / zlib / gzip x three buffer-size pairs (including
1-byte input and 1-byte output buffers for the small inputs). Python decompressed everything our deflate wrote, and our
inflate read all of it back through small pieces; our inflate also read Python-made raw, zlib and gzip streams (levels
0, 1, 6, 9; input pieces of 1, 7, 4096 and 1 048 576 bytes). 450 damaged streams (bit flips, truncation, random tails)
were fed to the inflater: no sanitizer report, no hang, only clean error codes. Result: 0 failures. Compressed sizes are
within about 1 percent of zlib's at level 6 (sometimes smaller, sometimes larger; the zero-run case is 158 bytes against
zlib's 120). The driver for this lives in my scratch directory, not in the repository yet.

## Tools

`gzip [-cdfklnNqrtv1-9] [FILE...]`, `gunzip`, `zcat`: see `man gzip`. They compress to `FILE.gz` and remove the original
(unless `-k`), keep the mode and the modification time, refuse to overwrite without `-f`, read several concatenated gzip
members, and report a damaged or truncated file with exit status 1.

## System call added

`SYS_utimes` (82): `utimes(path, struct gtimespec times[2] or NULL, flags)`, with `UTIME_NOW` / `UTIME_OMIT` in `tv_nsec`
like `utimensat()`. The caller must own the file (or be root); "now" for both only needs write permission. It sets the
`atime`/`mtime` fields of the vnode; a filesystem that stores times on disk (the FAT32 work) should write them from there
when it flushes. The libc wrappers are `utime()`, `utimes()` and `lutimes()` in `<utime.h>`. Shared files touched:
`shared/syscall.h` (the number, two constants).
