# Gensokyo Display Lab

Three independent display faces for Touhou.OS.

- **Danmaku** uses a grid of circles and four-point stars to evoke a
  spell-card attack. It works best at headline sizes, where the two pellet
  shapes remain distinct.
- **Gap** reads the placement grid as horizontal ink ribbons. The upper
  rows move one cell to the right, with an extra break across the middle.
- **Ofuda** uses individually drawn polygonal silhouettes, uneven cap
  tops, and angular counters. It has a short legacy kerning table for
  pairs such as AY and PA. The silhouettes live in src/designs.py.

All three cover A–Z, digits, spaces and seven punctuation characters.
Lowercase input maps to the same uppercase glyphs. The fonts are
unhinted TrueType display prototypes with 1000 units per em. The preview
uses Pillow/FreeType. Touhou.OS libttf remains to be tested.

Run with Python 3.10 or later and fontTools. The specimen also needs
Pillow 10.1 or later with FreeType.

```
python src/build.py
python tools/check.py
python tools/specimen.py
```

The checker validates coverage, table checksums, glyph bounds, contour
direction, intersections on sampled outlines, and the Ofuda kerning
entry for AY. The checked-in fonts can be regenerated from source.
License: SIL Open Font License 1.1, no Reserved Font Name (`LICENSE`).
