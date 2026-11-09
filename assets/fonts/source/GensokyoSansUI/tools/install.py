"""Copy the built Gensokyo Sans faces to assets/fonts and bring assets/fonts/SHA256SUMS up to date.

    python src/build.py && python tools/check.py && python tools/install.py          (from this folder)

Every Gensokyo*.ttf in assets/fonts gets a line (sorted by name, "digest  assets/fonts/NAME"); the display faces are not touched, only listed. The host tests
(tests/host/test_ttf.py, OwnerFonts) fail when a font differs from its line, so a rebuilt face is installed in one command and cannot be forgotten.
"""
import hashlib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT.parents[1]                       # assets/fonts


def main():
    for built in sorted((ROOT / "build").glob("Gensokyo*.ttf")):
        target = FONTS / built.name
        if not target.exists() or target.read_bytes() != built.read_bytes():
            shutil.copyfile(built, target)
            print("installed", built.name)
    lines = []
    for ttf in sorted(FONTS.glob("Gensokyo*.ttf")):
        lines.append("%s  assets/fonts/%s" % (hashlib.sha256(ttf.read_bytes()).hexdigest(), ttf.name))
    (FONTS / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("SHA256SUMS: %d fonts" % len(lines))


if __name__ == "__main__":
    sys.exit(main())
