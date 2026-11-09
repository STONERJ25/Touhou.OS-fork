# Touhou.OS Font Set v2

- **GensokyoSansUI** contains Gensokyo Sans Regular 0.5, the everyday
  interface font. See its README for the spacing passes (0.3: T and i;
  0.4: 37 more kerning pairs; 0.5: the space is 264 units, the owner's
  decision).
- **GensokyoDisplayLab** contains Danmaku, Gap and Ofuda. Its three
  font binaries match the original release byte for byte.
- **DesignLab** adds Frost (beveled ice blocks), Kappa (instrument
  segments), Mischief (rotated and staggered Ofuda silhouettes), and
  Seals (Sans letterforms cut out of stamped tiles).

Each folder contains its TTFs under build and editable source under
src. The display fonts cover capital shapes, digits, spaces, and
seven punctuation characters. Lowercase input maps to capital shapes.
The four new display fonts reuse original outlines or placement grids
from this project.

Run each folder's src/build.py and tools/check.py from that folder
with Python 3.10+ and fontTools. Keep the sibling folders together
when rebuilding DesignLab. Run DesignLab/tools/specimen.py with
Pillow and FreeType to recreate the combined specimen in specimen/.
Touhou.OS libttf rendering remains to be tested. License: the
fonts and these sources are under the SIL Open Font License 1.1
with no Reserved Font Name (LICENSE in each folder, the same text
as ../OFL.txt).
