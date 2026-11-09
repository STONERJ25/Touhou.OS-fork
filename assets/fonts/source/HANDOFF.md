# Touhou.OS font integration handoff

The `assets/fonts/` directory is laid out relative to the Touhou.OS
repository root. Copy its eight TTFs and the accompanying `LICENSE`
into the repository's `assets/fonts/` directory. Keep `font-source/`
with the design project for rebuilding the fonts. The source does not
need to ship in the OS image.

| File | Intended use |
| --- | --- |
| `GensokyoSans-Regular.ttf` | Default interface and reading face, version 0.3 |
| `GensokyoDanmaku-Regular.ttf` | Spell-card overlays |
| `GensokyoGap-Regular.ttf` | Boundary errors and glitch effects |
| `GensokyoOfuda-Regular.ttf` | Shrine notices |
| `GensokyoFrost-Regular.ttf` | Ice warnings and Cirno screens |
| `GensokyoKappa-Regular.ttf` | Instrument displays |
| `GensokyoMischief-Regular.ttf` | Comic alerts and exclamations |
| `GensokyoSeals-Regular.ttf` | Barrier notices, stamps and badges |

## Integration

1. Include the TTF assets in the OS build using the repository's existing
   asset-loading and packaging path.
2. Select Gensokyo Sans for the proportional UI. Keep the existing terminal
   font and keep the configured glyph fallback for characters outside the
   Sans repertoire. Sans covers printable ASCII and selected UI symbols.
   The display faces map lowercase input to capitals and are intended for
   selected labels and headlines.
3. Follow the host validation path in the Touhou.OS font specification.
   From the OS repository root, run:

   ```sh
   python tools/hosttest.py discover -s tests/host -p "test_ttf.py"
   ```

   With the resulting `ttftool`, check the UI at actual menu sizes. Use
   `docs/TTF.md` in the OS repository for the local tool path.

   ```sh
   ttftool draw ui-12.ppm assets/fonts/GensokyoSans-Regular.ttf 12 "Title  Tea  To  Ti  iii  iji  Il1|  0O"
   ttftool draw ui-16.ppm assets/fonts/GensokyoSans-Regular.ttf 16 "File  Edit  View  Save As..."
   ttftool draw display.ppm assets/fonts/GensokyoSeals-Regular.ttf 42 "BARRIER SET"
   ```

   Smoke-test all seven display assets in the actual picker or the screens
   that use them. In particular, check the counters in Seals and the
   disconnected segments in Kappa at their intended display sizes.

## Source and validation

See `font-source/specimen/TouhouOS-Font-Set-v2.png` for the approved
specimen. It uses FreeType. The Touhou.OS repository was unavailable
for this export, so its asset build and `libttf` tests remain open.

With Python 3.10+ and fontTools, run `python src/build.py` followed by
`python tools/check.py` in each of the three source subdirectories
described in `font-source/README.md`. Keep them as siblings because
DesignLab imports design sources from the other two. Rebuild outputs
go into each source folder's local `build/` directory. On a shell with
GNU coreutils, run `sha256sum -c SHA256SUMS` at the export root to check
the shipped binaries.

The exported TTFs are byte-for-byte copies of the approved v2 set,
except Gensokyo Sans, which was rebuilt in the OS repository as version
0.4 on 2026-11-03 (37 kerning pairs added, nothing else changed) and as
version 0.5 the same day (the space is 264 units, the owner's decision;
see `GensokyoSansUI/README.md`). Since 2026-11-08 the fonts and their sources are under the SIL Open Font
License 1.1 with no Reserved Font Name (`OFL.txt`); the generators write the
notice into every font's name table, so the shipped binaries differ from the
ones above in the name table only.
