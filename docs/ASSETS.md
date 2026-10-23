# Asset policy

ZUN's fan-content guidelines (https://touhou-project.news/guidelines_en/, the page the README links) say fan content must not
include anything extracted from the official games. They make no exception for the PC-98 titles, and "abandonware" is a
community label, not a legal status, so the repository does not ship game assets of any era.

What the project does instead:

- **Original and procedural art.** Icons, wallpapers, the yin-yang orb, torii, moon and bamboo are drawn in code or authored
  here, in a PC-98 flavoured style (limited palette, dithering, 8x16 text, hard pixels). The aesthetic is free to use.
- **Fonts.** `shared/font8x16.c` is generated from DejaVu Sans Mono (Bitstream Vera license, `assets/fonts/LICENSE_DEJAVU`)
  plus procedural box-drawing glyphs.
- **User-supplied files stay local.** `assets/local/` is git-ignored. Anything the user puts there (their own game copies,
  wallpapers, music) is for their own machine and is never committed. Loaders for such files are fine; the files are not.
- **No downloads.** The build never fetches third-party assets.

## Long-term goal: playing PC-98 games

Run the user's own copies of PC-98 software in a userland emulator ("Hakurei emulator"), loading disk images from
`assets/local/` or a mounted FAT volume. No game data is shipped; only the emulator is part of the OS.

Rough scope (stretch, after the desktop and applications are solid):

1. 8086/286-class x86 real-mode interpreter (V30/286 instruction set, segmentation, interrupts).
2. PC-98 chipset basics: interrupt controllers, PIT, DMA, uPD765 floppy, SASI/IDE disk, calendar clock.
3. Graphics: text VRAM + kanji font, uPD7220 GDC planar 640x400 (16 colours), GRCG/EGC raster ops.
4. Sound: YM2608 (OPNA) FM + SSG synthesis, output through an AC97/Intel HDA driver.
5. Disk image formats: D88, FDI, HDI/NHD; keyboard and mouse mapping.
