"""Build Gensokyo Sans Regular: the original quadratic paths (outlines.py, ui_outlines.py), the Latin-1 and Latin Extended-A letters made from them
(glyphset.py, latin_outlines.py), the signs (symbols.py), kerning for all of it (spacing.py, kerning.py)."""
from pathlib import Path

import bold
import mono
import fontbuild
import glyphset
import kerning
from spacing import KERN

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build" / "GensokyoSans-Regular.ttf"
VERSION = "0.6"
# The advance of the space and the no-break space, in font units. The specification said 240; the owner decided on 264 (2026-11-03) after seeing running
# text at 13 px, where 240 left the words of a line a little too close (see README.md, "Space width in 0.5").
SPACE_ADVANCE = 264


def build_face_font(face, style, order, pairs, weight=400):
    return fontbuild.make_font(face, family="Gensokyo Sans", style=style, version=VERSION, kern=pairs, weight=weight, order=order)


def main(out_dir=None):
    """Builds the faces into out_dir (default: build/ of this folder) and returns the paths."""
    import tempfile
    out = Path(out_dir) if out_dir else OUTPUT.parent
    out.mkdir(parents=True, exist_ok=True)
    paths = []

    reg = glyphset.build_regular(SPACE_ADVANCE)
    reg_order = glyphset.glyph_order(reg)
    pairs = kerning.expand(reg, KERN)
    path = out / "GensokyoSans-Regular.ttf"
    build_face_font(reg, "Regular", reg_order, pairs).save(path)
    print(path)
    print("Regular: %d glyphs (%d composite), %d code points, %d kern pairs" % (len(reg.names), len(reg.comps), len(reg.cmap), len(pairs)))
    paths.append(path)

    face = bold.build_bold(reg)
    known = set(reg_order)
    order = [n for n in reg_order if n in face.adv] + [n for n in face.names if n not in known]
    with tempfile.TemporaryDirectory() as tmp:                           # the kerning is measured on the Bold's own outlines
        plain = Path(tmp) / "bold-nokern.ttf"
        build_face_font(face, "Bold", order, None, 700).save(plain)
        measured = bold.measure_kerning(plain, KERN)
    pairs = kerning.expand(face, measured)
    path = out / "GensokyoSans-Bold.ttf"
    build_face_font(face, "Bold", order, pairs, 700).save(path)
    print(path)
    print("Bold: %d glyphs (%d composite), %d code points, %d kern pairs" % (len(face.names), len(face.comps), len(face.cmap), len(pairs)))
    paths.append(path)

    mface = mono.build_mono(reg)
    known = set(reg_order)
    cp_of = {name: cp for cp, name in mface.cmap.items()}
    order = [n for n in reg_order if n in mface.adv] + sorted((n for n in mface.names if n not in known), key=lambda n: cp_of.get(n, 1 << 20))
    path = out / "GensokyoMono-Regular.ttf"
    fontbuild.make_font(mface, family="Gensokyo Mono", style="Regular", version=VERSION, kern=None, mono=True, order=order).save(path)
    print(path)
    print("Mono: %d glyphs (%d composite), %d code points, cell %d" % (len(mface.names), len(mface.comps), len(mface.cmap), mono.CELL))
    paths.append(path)
    return paths


if __name__ == "__main__":
    import sys
    main(sys.argv[1] if len(sys.argv) > 1 else None)
