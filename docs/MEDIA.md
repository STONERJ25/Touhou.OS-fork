# Media: archives, images, the Gensokyo Gallery

This page says what exists, what the limits are, and (plainly) what is not finished yet. Status as of the last commit on
this branch; update it when you change something.

## Status

| Piece | State |
|---|---|
| `libz` (inflate, deflate, crc32, adler32) | **done**, tested on the host against zlib (see below) and in the guest through `gzip` |
| `gzip`, `gunzip`, `zcat` | **done**, manual pages written, checked by hand in the guest |
| `utimes()` system call, `<utime.h>` | **done** (`SYS_utimes` = 82, `kernel/fs/utimes.c`); `gunzip` and `gzip` restore file times with it |
| `tar` (create, extract, list, `-z`, `-C`, `-v`, `-f`, `--strip-components`, `--exclude`) | **written**, manual page written; round trip (long paths, symlinks, modes, `-z`, pipes, truncated archive) checked by hand in the guest. Archives made by Python's `tarfile` (GNU and pax formats) have **not** been tried yet |
| `zip`, `unzip` | not written yet |
| `libimg` (PNG, JPEG, GIF, BMP, PNM, TGA decoders; PNG/BMP/PPM encoders; scaling and rotation) | not written yet |
| Gensokyo Gallery (image viewer) | not written yet |
| User wallpaper (`~/.config/hakurei/wallpaper`, `wallpaper` command) | not written yet |
| Screenshots as PNG in `~/Pictures`, `screenshot` command | not written yet (Ctrl+F12 still writes `/tmp/screenshot.ppm`) |
| `k_image_thumbnail()` for file browsers | not written yet |
| `tests/case_media.py` | not written yet; the checks that were run so far were ad hoc |

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
