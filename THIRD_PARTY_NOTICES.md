# Third-party notices

TouhouOS is released under the MIT License ([LICENSE](LICENSE)). The code was written for this project from public specifications; no source code of other projects is copied into it. This page lists everything in (or used by) the repository that comes from somebody else, and the terms that go with it. A copy of this page and of the licenses it names is installed on the system itself, in `/ScarletDevilMansion/share/doc/touhouos/`, so that every image carries them.

## Included in the repository

### The 8x16 bitmap font (`shared/font8x16.c`)

The glyph bitmaps were rasterised by `tools/genfont.py` from **DejaVu Sans Mono**, so the data is a derivative of the DejaVu fonts, which are derived from **Bitstream Vera**.

* Bitstream Vera Fonts: Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a trademark of Bitstream, Inc.
* Changes made for DejaVu are in the public domain; glyphs imported from the Arev fonts are Copyright (c) 2006 by Tavmjong Bah.
* The license permits use, copying, modification, merging, publishing, distribution and sale of the font software, with the copyright and permission notice kept in all copies, **no sale of the font by itself**, and **no use of the names "Bitstream" or "Vera"** for modified versions.
* The full text is in [assets/fonts/LICENSE_DEJAVU](assets/fonts/LICENSE_DEJAVU).

## Used by the tests, not included

These are downloaded or generated at test time into `build/` (git-ignored) and never become part of the repository or of an image.

| Material | Used by | Terms |
|---|---|---|
| SingleStepTests 8088 and 80386 vectors (github.com/SingleStepTests) | `tests/host/x86sst.py`, a cross-check of the 386 model | MIT for the software, public domain for the data; recorded from real chips |
| html5lib-tests tokenizer suite (github.com/html5lib/html5lib-tests) | `tests/web/` conformance runs | MIT |
| web-platform-tests HTML parsing data (github.com/web-platform-tests/wpt) | `tests/web/` conformance runs | 3-Clause BSD |
| test262 (github.com/tc39/test262) | the JavaScript engine's conformance runs | BSD-style, per file |

The files under `tests/web/tree/` use the html5lib *file format*; their cases were written for this project.

Known-answer vectors that are committed are facts, not code from another project: `user/bin/sshkat_vectors.h` holds the published vectors of FIPS 180 and 197 and RFC 4231, 5869, 7748, 8032 and 8439 plus answers computed with Python's `cryptography` package and `hashlib` from a fixed seed (`tools/gen_sshcrypto_vectors.py`); `tests/host/ntfs_lznt1_vectors.py` holds the output of Windows' own LZNT1 compressor for inputs written for the purpose and the worked example of the MS-XCA specification. `tests/host/test_tls_vectors.py` also compares against `openssl` and Python's `ssl` module on the developer's machine; those tools are not shipped.

## Not included, and never will be

* **Game data.** The PC-98 emulator runs programs from disk images the user provides. No game, ROM, BIOS or font ROM of any machine is in the repository, in an image built from it, or in the tests.
* **Linux programs and libraries.** The Linux compatibility layer ([docs/COMPAT.md](docs/COMPAT.md)) runs programs and shared libraries that the user installs; none are shipped. The test programs in `tests/linux/src` are our own sources; the tests build them on the developer's Linux (in WSL) with the compiler and C library found there, so the resulting static binaries contain that C library's code under its own license (the LGPL for glibc). They live in `build/linux-inputs` (git-ignored), are downloaded into a test guest from the host, and are never committed or put into an image.
* **The Gensokyo typefaces** (`assets/fonts/Gensokyo*.ttf`, drawn for this project) are not part of the public repository until their license is chosen ([docs/LICENSING.md](docs/LICENSING.md)).

## Names and characters

TouhouOS is an unofficial fan work. Touhou Project, its characters, names and settings belong to ZUN and Team Shanghai Alice; this repository neither contains their artwork, music or game data nor grants any right to them. The MIT License covers the code, documentation and original assets of this project only.
