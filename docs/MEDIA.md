# Media: archives, pictures and the Gensokyo Gallery

Everything here is original code (no third-party sources) and lives in userland: two libraries, `libz` and `libimg`, the
tools built on them, the picture viewer, and the places where the desktop uses pictures (wallpaper, screenshots, thumbnails).

| Piece | Where | What |
|---|---|---|
| `libz` | `user/lib/z`, `<z.h>` | DEFLATE: streaming inflate and deflate, zlib and gzip framing, CRC-32, Adler-32 |
| `libimg` | `user/lib/img`, `<img.h>` | picture decoders and encoders, scaling, rotation, flipping, compositing |
| media helpers | `user/lib/kappa`, `<kappa_media.h>` | wallpaper setting and rendering, `~/Pictures` and screenshot names, thumbnails for file browsers |
| `gzip` `gunzip` `zcat` `tar` `zip` `unzip` | `user/bin/kappabox/{gzip,tar,zip}.c` | archive tools (kappabox applets) |
| `imginfo` `convert` `wallpaper` `screenshot` | `user/bin/kappabox/media.c` | picture tools (kappabox applets) |
| Gensokyo Gallery | `user/bin/gallery/` | the picture viewer (`gallery`) |
| `SYS_utimes` | `kernel/fs/utimes.c`, `<utime.h>` | set file times (tar, unzip and gunzip restore them) |
| tests | `tests/case_media.py`, `tests/imgdata.py`, `tests/host/` | see the end of this page |

Every command has a manual page (`man gzip`, `man gallery`, ...).

## libz

A streaming DEFLATE implementation (RFC 1951) with the zlib (1950) and gzip (1952) wrappers.

* **Inflate** (`z_inflate_init(format)`, `z_inflate`, `z_inflate_end`; formats `Z_RAW`, `Z_ZLIB`, `Z_GZIP`, `Z_AUTO`): a state
  machine, so input and output may arrive in pieces of any size (the tests go down to one byte). It keeps its own 64 KiB window, so
  the caller's buffers are never read back. Tables are checked (over-subscribed or incomplete codes, bad lengths, distances that
  reach back before the start of the data, bad stored-block lengths), gzip header flags and header CRC are checked, and the CRC-32 /
  ISIZE (gzip) or Adler-32 (zlib) trailer is verified. Bytes after the end of a stream stay in `next_in`/`avail_in`, so
  concatenated gzip members can be read with `z_inflate_reset`. `z_uncompress()` is the one-shot form with an output cap
  (default 256 MiB) against decompression bombs.
* **Deflate** (`z_deflate_init(level 0..9, format)`, `z_deflate(flush)`, `Z_NO_FLUSH` / `Z_SYNC_FLUSH` / `Z_FINISH`): LZ77 with hash
  chains and lazy matching (greedy for levels 1 to 3, zlib's chain/lazy parameters), 32 KiB blocks, and for every block the exact
  bit cost of a dynamic-Huffman, a fixed-Huffman and a stored encoding is computed and the cheapest is written, so incompressible data
  costs five bytes per 32 KiB. Huffman code lengths are limited to 15 bits (7 for the code-length code) with the usual Kraft-sum
  repair. `z_compress()` is the one-shot form.
* Memory: about 400 KiB per compressor, 70 KiB per decompressor. Not implemented: preset dictionaries, `Z_FULL_FLUSH`.

## libimg

```c
Image im;                                           // w, h, px[] = 0xAARRGGBB (straight alpha), has_alpha
if (img_load("/HumanVillage/reimu/moon.jpg", &im) == IMG_OK) { ... img_free(&im); }
ImgInfo info; img_probe_file(path, &info);          // format, size (as displayed), alpha, frames: no pixel decoding
```

| Format | Read | Notes |
|---|---|---|
| PNG | all colour types and bit depths (1, 2, 4, 8, 16), Adam7, `tRNS` (palette alpha and colour keys), chunk CRCs | 16-bit samples keep the high byte; gamma and colour profiles are ignored |
| JPEG | baseline, extended sequential and progressive Huffman; grayscale, YCbCr, RGB, Adobe CMYK/YCCK; any sampling that divides evenly (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1); restart intervals; EXIF orientation applied | libjpeg's integer IDCT, colour conversion and "fancy" chroma upsampling, so the pixels match libjpeg-turbo exactly in every test file |
| GIF | LZW, global and local palettes, transparency, interlacing; `ImgAnim` plays animations (frame list, disposal methods, delays) | `img_decode` returns the first frame on the logical screen |
| BMP | 1, 4, 8, 16, 24, 32 bit; RLE4 and RLE8; bitfields and alpha masks; OS/2 and V3 to V5 headers; top-down and bottom-up | the fourth byte of a 32-bit `BI_RGB` pixel is "reserved" and ignored (as GDI and Pillow do); alpha comes from bitfield masks |
| PNM | P1 to P7 (ASCII and binary, PAM), maxval up to 65535 | |
| TGA | colour-mapped, true-colour, gray; raw and RLE; 8/15/16/24/32 bit; any origin | |

Written: **PNG** (`img_write_png(level 0..9)`: adaptive row filters, picks grayscale, gray+alpha, RGB or RGBA from the pixels,
IDAT chunks of 32 KiB, so memory stays small), **BMP** (24-bit, or 32-bit with alpha in a V4 header) and **PPM** (P6, transparency
flattened onto a background colour). `img_save_*` write `name.part` and rename it, so a full disk never leaves half a picture.

Transformations: `img_scale` (`IMG_NEAREST`, `IMG_BILINEAR`, `IMG_AREA`: the exact fractional-coverage average, which is what to use for
shrinking, weighted by alpha so transparent edges do not turn dark), `img_rotate90`, `img_flip`, `img_fit` (aspect-preserving size),
`img_composite_over` (flatten onto a colour).

**Hostile input.** Every length, offset, dimension and table is checked against the data that is really there. Sizes are capped before
anything is allocated (16384 per side, 64 million pixels, 256 MiB per file). JPEG arithmetic is done in 64 bits so absurd
coefficients cannot overflow, and a JPEG may have at most 64 scans (a progressive scan made of end-of-block runs costs a few bytes to store and a pass over the
whole picture to decode, so thousands of them would keep the decoder busy for minutes). A truncated or damaged file gives `IMG_E_TRUNC` / `IMG_E_CORRUPT`; the partial picture is not returned.
Not supported: arithmetic-coded, lossless or 12-bit JPEG, JPEG with a frame height of 0 (DNL), interlaced or animated PNG beyond the
first image, ICC profiles, CMYK TIFF-style tricks, GIF frames larger than the pixel cap. There is no decode-time downscaling: a very
large photograph is decoded in full (a 24 megapixel 4:2:0 JPEG needs about 36 MB of planes plus 96 MB of pixels) and then scaled.

**Speed** (`imginfo -t`, in QEMU on the development PC, so only an order of magnitude): a 1920x1080 JPEG (4:2:0) decodes in 0.4 to 0.7 s, a 3840x2160 one in 1.4 s,
a 12 megapixel one in 2.2 s, a 3.8 MB 1920x1080 PNG in 0.4 s, a BMP in 60 ms. The Gallery and the wallpaper code decode in the event loop and the desktop shell, so a
photograph of that size makes the window (or the shell, once, when the wallpaper changes) wait for about that long.

## Tools

* `gzip [-cdfklnNqrtv1-9]`, `gunzip`, `zcat`: compress to `FILE.gz` and remove the original (unless `-k`), keep mode and mtime, refuse to
  overwrite without `-f`, read concatenated members, exit status 1 for damaged or truncated data.
* `tar -c|-x|-t [-zvpkOhm] [-f FILE] [-C DIR] [--strip-components=N] [--exclude=PAT] [MEMBER...]`: writes ustar (GNU long-name entries for long
  paths), reads ustar, GNU and pax archives, gzip detected automatically when reading. Keeps modes, symlinks and mtimes. Refuses `..`,
  absolute names and extraction through symlinks. Hard links are extracted as copies (the system has none), devices are skipped.
* `zip [-rjqv0-9] ARCHIVE FILE... [-x PAT...]`, `unzip [-ltpoqn] [-d DIR] ARCHIVE [MEMBER...]`: store and deflate, directories, symlinks, Unix modes,
  DOS time plus the UTC timestamp extension; bounds-checked reading with CRC and size verification; no zip64, encryption or spanning. `zip` refuses
  to add to an existing archive.
* `imginfo [-s] [-t] FILE...`: format, size, alpha, frames, bytes; `-s` adds the CRC-32 of the decoded pixels (red, green, blue, alpha bytes in reading order),
  `-t` the time the decoding took in milliseconds.
* `convert [-resize WxH | -fit WxH | -scale N] [-rotate 90|180|270] [-flip] [-flop] [-level N] [-background RRGGBB] IN OUT`: PNG, BMP or PPM out.
* `wallpaper [-m cover|fit|center|tile|stretch] IMAGE`, `wallpaper -c`, `wallpaper`: see below.
* `screenshot [-d SECONDS] [FILE]`: see below.

`SYS_utimes` (82): `utimes(path, struct gtimespec times[2] or NULL, flags)` with `UTIME_NOW` / `UTIME_OMIT` in `tv_nsec` like `utimensat()`. The caller
must own the file (or be root); "now" for both only needs write permission. It sets the vnode's `atime`/`mtime` and then calls the filesystem's optional
`VNodeOps.settimes(vnode, atime, mtime)` hook, which a filesystem that keeps its own copy of the times must implement: the FAT driver does (the times go into
its node and the directory entry, at FAT's resolution of two seconds for the modification time), tmpfs needs nothing. libgap: `utime()`, `utimes()`,
`lutimes()` in `<utime.h>`.

## The Gensokyo Gallery (`gallery`)

Start menu: "Gensokyo Gallery" (Programs). `gallery [FILE | FOLDER]`: opens the picture (and walks the pictures of its folder, sorted by name with numbers
compared as numbers) or the first picture of the folder; with nothing it opens the newest picture in `~/Pictures` if there is one (screenshots are named by
date, so that is the latest one), and Ctrl+O opens any other. The Explorer opens every picture it recognises by extension with
`execv("/bin/gallery", {"gallery", path, NULL})` (and a selection of several pictures starts one Gallery, which walks the folder); there is no
general file-type association table in the system yet.

* Navigation: Left/Right, PageUp/PageDown, Backspace/Space, Home/End (wraps around). Zoom: `+`/`-`, the wheel around the pointer, `0`/`F` fit
  (shrinks, never enlarges), `1` 100%, `2`..`4` 200% to 400%; double-click toggles fit and 100%; dragging pans. `R`/`L` rotate, `H`/`V` flip (the file is never
  changed). `F5`/`S` slideshow (`--slideshow SECONDS`, Esc stops it). `W` makes the picture the wallpaper (with the placement chosen before, `cover` at first). `I` shows details. Menu bar and toolbar do the
  same; the status bar shows name, size, format, bytes, position in the folder and zoom.
* Animated GIFs play with their own delays. Transparent pictures sit on a checkerboard. A picture that cannot be decoded is explained in the window and the status bar
  (and in the `--trace` log) and the next one can still be opened. Big files show a "Loading..." frame first.
* `gallery --info FILE...`, `gallery --thumb SIZE FILE [OUT.ppm]` (what a file browser gets from `k_image_thumbnail`) and `--trace` (one line per action on
  standard output; started from the panel that goes to `/tmp/gallery.log`) are for scripts and tests.

## Wallpaper

`~/.config/hakurei/wallpaper` (see `kappa_media.h`): first line the full path of the picture, optional second line the mode: `cover` (default: fill, crop the edges),
`fit` (whole picture, night-blue bars), `center`, `tile`, `stretch`. `hakurei-shell` looks at the file and the picture's modification time and size about
every second and redraws when either changes; `hakurei-lock` uses the same setting, and the login screen (nobody is signed in) uses `/etc/hakurei/wallpaper`
if it exists. When nothing is configured, or the picture cannot be read, the procedural night scene (`k_scenery_night`) is drawn, so a missing file never
leaves a black desktop. The picture is decoded once, scaled to the screen size (area average when shrinking), and the original freed.
Three ways to set it: the `wallpaper` command, "Set as wallpaper" in the Gallery (`W`), and the Control Panel's Appearance pane ("Wallpaper": choose a picture
with the file dialog, a radio button per mode, "Night scene" to go back).

## Screenshots

Ctrl+F12 and the desktop menu's "Take screenshot" write `~/Pictures/Screenshot-YYYY-MM-DD-HHMMSS.png` (the folder is created on demand) and, as before,
`/tmp/screenshot.ppm` (the Sketchbook and the tests use it). The window server (`fairyd`) writes the PPM at once and hands a copy of the screen to a child process
(Aya) that compresses the PNG at level 2 and renames it into place, so the desktop does not stall; the kernel log says `[aya] Aya took a photo of the screen`.
`screenshot [-d SECONDS] [FILE]` asks fairyd for the same thing and waits until the file is complete; `fairy_screenshot(display, path)` is the library call
(`.png`, `.bmp` or anything else = PPM; `""` = both defaults).

## Thumbnails (for the Explorer)

```c
KCanvas thumb;
if (k_image_thumbnail("/HumanVillage/reimu/moon.png", 64, &thumb))      // reads and decodes on a miss
    kc_blit(&ui->cv, x, y, thumb.px, thumb.w, thumb.h, thumb.stride);
bool hit = k_image_thumbnail_cached(path, 64, &thumb);                  // only looks in the cache: use it while painting
```

The picture is scaled to fit inside SIZE x SIZE (never enlarged, area average), transparency is flattened onto the paper colour, and the pixels stay in a
32-entry least-recently-used cache keyed by path, size, modification time and file size; draw them at once and do not keep the pointer. Files over 8 MiB or 24
megapixels, and files that are not pictures, are refused, and the refusal is remembered. A browser should paint with `k_image_thumbnail_cached` and fill the
cache between frames (one `k_image_thumbnail` per idle iteration), because decoding a large photograph takes a moment on a slow machine.

## Tests

* `python tests/run_all.py media` runs the guest tests (`tests/case_media.py`; the host makes every test file with Pillow, `zlib`, `tarfile`, `zipfile` and the hand-written
  encoders in `tests/imgdata.py`, and puts them in a copy of the initrd; results come back as base64 over the serial console):
  * `media_decoders`: 101 pictures (every PNG colour type and depth, plain and Adam7, with and without tRNS, specified independently of the decoder; JPEG in every sampling
    (4:4:4, 4:2:2, 4:2:0 from Pillow; 4:4:0, 4:1:1, 4:1:0 and mixed layouts from the small encoder in `tests/jpegenc.py`), with restart markers, baseline and progressive, gray,
    pictures only one to five pixels wide, EXIF orientations; GIF static, transparent, interlaced and animated; BMP including RLE4, RLE8 and 565 top-down; PNM P1 to P6 and 16-bit;
    TGA raw and RLE). `imginfo -s` must agree with the reference on format, size, alpha, frame count and the CRC of the pixels.
  * `media_encoders`: `convert` writes PNG (levels 0, 1, 6, 9), BMP, PPM, rotates, flips and scales; Pillow reads every result and finds the expected pixels.
  * `media_hostile_files`: about 150 truncated, bit-flipped and absurd files, and a progressive JPEG with 200 repeated scans; `imginfo` must answer 0 or 1 for each (1 for the last), and the kernel log must not gain a fault.
  * `media_archives`: tar (GNU, pax, ustar), tgz, zip, gzip made on the host unpack in the guest with the right bytes, modes, times and symlinks; archives made in the guest are
    read by `tarfile`, `zipfile` and `gzip`; corrupt and truncated archives are refused.
  * `media_utimes_fat`: `tar -x` and `unzip` restore times on a FAT volume (a disk image from `tools/mkdisk.py`); the independent FAT checker reads them back from the image.
  * `media_thumbnails`, `media_gallery` (opens, walks a folder, zooms, rotates, flips, plays a GIF, runs a slideshow, sets the wallpaper, reports a broken file), `media_screenshot`
    (Ctrl+F12, the `screenshot` command, every wallpaper mode), `media_panel_wallpaper` (the Control Panel's placement buttons, "Night scene" and the file dialog).
* `tests/host/imgtest.py` (Linux or WSL with gcc, AddressSanitizer and Pillow; not part of `run_all.py`) builds libimg with `-fsanitize=address,undefined` and compares it with Pillow on
  thousands of files; `ONLY=fuzz FUZZ_N=6000 FUZZ_SEED=7 python3 tests/host/imgtest.py` is the long hostile-input run (every mutation of the 100-odd corpus files must be decoded or refused cleanly).
  `tests/host/ztest.py` does the same for libz against Python's `zlib` and `gzip` (0 failures: 11 inputs x levels 0, 1, 3, 6, 9 x raw, zlib, gzip x three buffer sizes, Python-made streams,
  450 corrupted streams).

## Known limits

* No JPEG decode-time downscaling, no progressive display of truncated files (they are refused), no animated PNG, WebP or TIFF.
* `zip` cannot update an archive, no zip64; `tar` has no multi-volume, sparse or device support.
* The Gallery decodes in the event loop (the Loading frame hides most of it); there are no threads in the viewer. It keeps one picture in memory (plus a rotated or pre-scaled copy).
* The wallpaper picture is decoded again on every change of the setting; it is not cached between runs.

## State when paused

Paused on 2026-10-23 in the evening, at the request of the user. The branch `worktree-agent-a651f6420dbc8cc2c` is committed, builds, and has `main` merged in
(the merge was clean when it was done; `git merge-tree --write-tree HEAD main` says whether it still is).

* **Done**: everything on this page: libz and the archive tools, the libimg decoders (PNG, JPEG baseline and progressive, GIF with animation, BMP, PNM, TGA) and
  encoders (PNG, BMP, PPM), scaling and rotation, the Gallery, the wallpaper (`wallpaper`, Gallery, Control Panel), PNG screenshots, `k_image_thumbnail`, `imginfo`,
  `convert`, `screenshot`, the manual pages, `SYS_utimes` and its FAT hook, and the Explorer opening pictures in the Gallery.
* **Half done**: nothing; no uncommitted edits.
* **Not started** (ideas, most useful first): thumbnails inside the Explorer's icon view (the API is there; decoding a big photograph blocks a window for 0.5 s or more, so
  it wants a budget per frame or a helper process, and the icon cells are only 32 pixels); a JPEG encoder (so `convert` and screenshots can write `.jpg`) and a GIF writer;
  the Sketchbook reading and writing PNG through libimg; DCT-domain downscaling in the JPEG decoder (1/2, 1/4, 1/8: faster thumbnails and wallpapers of big photographs);
  `zip` adding to an existing archive; a decoded-wallpaper cache; animated PNG.
* **Tests**: the whole suite on the merged tree (at the commit "docs/MEDIA.md: measured decode times", with the Explorer change in) ended with `201 tests, 0 failed`.
  The later commits (the 64-scan cap, `stat()` before `open()`, manual pages, test changes) were tested with the groups they touch (`media_*` several times, `manual`,
  `explorer_open_apps`) on a scratch build, and a second whole run on the final head was stopped at test 57 (all passed) when the pause came. On the host: the sanitizer fuzz
  (6000 mutations seed 7, 4000 seed 11, 12000 seed 99) and the corpus check found nothing; libz's `ztest.py` 0 failures. `audio_mix_two_clients`, `notebook_eol_switch` and
  `sketchbook_guard_save` (other people's tests) failed once each in a run on a busy host and passed when run again; none touches this code.

## How to resume

1. `git merge main`, `python tools/build.py`. New kappabox applets need a manual page or `manual` fails; tests that wait for a shell prompt need `text` on their command line;
   the guest clock is slow when the host is busy, so wait for results, never for a time.
2. `python tests/run_all.py media,explorer_open_apps,manual` (about five minutes). Add `storage` when the FAT driver or `utimes` changes. The whole suite is about an hour.
   Two test runs in one checkout share `build/esp`: to run a second group in parallel, copy the sources (everything but `build/` and `.git`) to another folder and build there.
3. Host checks (WSL with gcc and Pillow; `ASAN_OPTIONS=detect_leaks=0`): `ONLY=corpus python3 tests/host/imgtest.py`, `ONLY=fuzz FUZZ_N=6000 FUZZ_SEED=7 python3 tests/host/imgtest.py`,
   `python3 tests/host/ztest.py`. Set `IMGTEST_DIR` to something else for a second run at the same time.
4. Where things are: decoders and encoders `user/lib/img/src/*.c` (API in `include/img.h`), libz `user/lib/z`, wallpaper and thumbnails `user/lib/kappa/src/{wallcfg,media}.c`,
   screenshots `user/bin/fairyd/comp.c`, the viewer `user/bin/gallery/main.c`, the tools `user/bin/kappabox/{media,gzip,tar,zip}.c`, the Control Panel group
   `user/bin/kappa-panel/panes.c`, test pictures `tests/imgdata.py` and `tests/jpegenc.py`, guest tests `tests/case_media.py`.
5. Lessons that cost time: GUI tests must not assume window positions (windows cascade; find the picture area by its colour) and should repeat a click whose effect does not
   show (`click_until`: a PS/2 mouse packet can be lost); a file that is written as `NAME.part` and renamed must be waited for under its final name; Pillow's PNG `tRNS` handling for
   low-bit gray and 16-bit colour differs from the specification, so the PNG expectations come from `tests/imgdata.py`, not from Pillow.
