"""The Gensokyo typefaces are licensed under the SIL Open Font License 1.1 without a Reserved Font Name (the owner's decision of 2026-11-08).

These tests fail when the pieces drift apart: the licence text itself, the notice above it, the name tables of the fonts, the generators that write
those name tables, the LICENSE copies next to the font sources, and the files the image and the notices carry. No build is needed:

    python tools/hosttest.py discover -s tests/host -p "test_font_license.py"

(the name-table checks need fontTools and are skipped without it).
"""
import re
import hashlib
import unittest
from pathlib import Path

try:
    from fontTools.ttLib import TTFont
    HAVE_FONTTOOLS = True
except ImportError:                                                     # pragma: no cover
    HAVE_FONTTOOLS = False

REPO = Path(__file__).resolve().parents[2]
FONTS = REPO / "assets" / "fonts"
SOURCES = ("GensokyoSansUI", "GensokyoDisplayLab", "DesignLab")

COPYRIGHT = "Copyright (c) 2026 STONERJ25 (https://github.com/STONERJ25/Touhou.OS-fork)"
LICENSE_DESCRIPTION = ("This Font Software is licensed under the SIL Open Font License, Version 1.1. "
                       "This license is available with a FAQ at: https://openfontlicense.org")
LICENSE_URL = "https://openfontlicense.org"

# The SHA-256 of the licence body (from the title line "SIL OPEN FONT LICENSE Version 1.1" to the last word of the DISCLAIMER) with every run of
# white space made one space. It was computed over the plain text at https://openfontlicense.org/documents/OFL.txt on 2026-11-08 and over the copy
# that matplotlib ships (LICENSE_STIX), and both gave this value: the file is the official text, word for word. The licence may be copied, but not edited.
OFL_BODY_SHA256 = "0339f4642e8741dea4841c236d258f360ffbe6e2cd3d032fbd0c10d1de8f58d1"

TITLE = "SIL OPEN FONT LICENSE Version 1.1"

if not (FONTS / "OFL.txt").is_file():                                   # the public export leaves the typefaces out until the owner says they may go
    raise unittest.SkipTest("the Gensokyo typefaces are not in this tree")


def read(rel):
    return (REPO / rel).read_text(encoding="utf-8")


class OpenFontLicenseText(unittest.TestCase):
    def test_the_licence_body_is_the_official_text(self):
        text = read("assets/fonts/OFL.txt")
        body = re.sub(r"\s+", " ", text[text.index(TITLE):]).strip()
        self.assertEqual(hashlib.sha256(body.encode()).hexdigest(), OFL_BODY_SHA256,
                         "assets/fonts/OFL.txt must hold the SIL Open Font License 1.1 unchanged; only the copyright notice above the title may be edited")

    def test_the_notice_above_it_names_no_reserved_font_name(self):
        text = read("assets/fonts/OFL.txt")
        header = text[:text.index("-----")]
        self.assertEqual(header.splitlines()[0], COPYRIGHT)
        self.assertIn("This Font Software is licensed under the SIL Open Font License, Version 1.1.", header)
        self.assertNotIn("Reserved Font Name", header, "the owner chose no Reserved Font Name: the notice must not claim one")
        self.assertNotIn("<", header, "a template placeholder is still in the notice")

    def test_the_text_uses_unix_line_ends_and_ends_with_a_newline(self):
        raw = (FONTS / "OFL.txt").read_bytes()
        self.assertNotIn(b"\r", raw)
        self.assertTrue(raw.endswith(b"FONT SOFTWARE.\n"))

    def test_each_font_source_folder_carries_a_copy(self):
        want = (FONTS / "OFL.txt").read_bytes()
        for folder in SOURCES:
            self.assertEqual((FONTS / "source" / folder / "LICENSE").read_bytes(), want, "%s/LICENSE differs from assets/fonts/OFL.txt" % folder)


@unittest.skipUnless(HAVE_FONTTOOLS, "fontTools is not installed")
class FontMetadata(unittest.TestCase):
    NAMES = sorted(p.name for p in FONTS.glob("Gensokyo*.ttf"))

    def test_there_are_fonts(self):
        self.assertGreaterEqual(len(self.NAMES), 10)

    def test_every_font_carries_the_notice_in_its_name_table(self):
        for name in self.NAMES:
            font = TTFont(str(FONTS / name), recalcTimestamp=False)
            for rec in font["name"].names:
                if rec.nameID == 0:
                    self.assertEqual(str(rec), COPYRIGHT, (name, rec.platformID))
                elif rec.nameID == 13:
                    self.assertEqual(str(rec), LICENSE_DESCRIPTION, (name, rec.platformID))
                elif rec.nameID == 14:
                    self.assertEqual(str(rec), LICENSE_URL, (name, rec.platformID))
                self.assertNotIn("undecided", str(rec).lower(), (name, rec.nameID))
                self.assertNotIn("Reserved Font Name", str(rec), (name, rec.nameID))
            ids = {r.nameID for r in font["name"].names}
            self.assertTrue({0, 13, 14} <= ids, (name, sorted(ids)))

    def test_embedding_is_not_restricted(self):
        for name in self.NAMES:
            font = TTFont(str(FONTS / name), recalcTimestamp=False)
            self.assertEqual(font["OS/2"].fsType, 0, "%s: fsType restricts embedding, which the OFL does not allow a font to do" % name)

    def test_the_checksums_follow_the_fonts(self):
        sums = dict(line.split("  ", 1)[::-1] for line in read("assets/fonts/SHA256SUMS").splitlines() if line.strip())
        for name in self.NAMES:
            digest = hashlib.sha256((FONTS / name).read_bytes()).hexdigest()
            self.assertEqual(sums.get("assets/fonts/" + name), digest, name)


class GeneratorsAndCarriers(unittest.TestCase):
    GENERATORS = ("assets/fonts/source/GensokyoSansUI/src/fontbuild.py", "assets/fonts/source/GensokyoDisplayLab/src/build.py",
                  "assets/fonts/source/DesignLab/src/build.py")

    def test_the_generators_write_the_same_strings_as_the_fonts_carry(self):
        for rel in self.GENERATORS:
            src = read(rel)
            self.assertIn('COPYRIGHT = "%s"' % COPYRIGHT, src, rel)
            self.assertIn('LICENSE_URL = "%s"' % LICENSE_URL, src, rel)
            first, second = LICENSE_DESCRIPTION.split("Version 1.1. ")
            self.assertIn('"%sVersion 1.1. "' % first, src, rel)
            self.assertIn('"%s"' % second, src, rel)
            self.assertNotIn("undecided", src, rel)

    def test_every_image_carries_the_licence_next_to_the_fonts(self):
        build = read("tools/build.py")
        self.assertIn('"assets/fonts/OFL.txt"', build, "tools/build.py must copy OFL.txt into share/doc/touhouos: the fonts are in every image")
        self.assertIn("OFL.txt", read("tests/case_license.py"))

    def test_the_notices_and_the_licensing_page_say_what_is_true(self):
        for rel in ("THIRD_PARTY_NOTICES.md", "docs/LICENSING.md"):
            text = read(rel)
            self.assertIn("SIL Open Font License", text, rel)
            self.assertIn("OFL.txt", text, rel)
        for rel in ("assets/fonts/README.md", "assets/fonts/source/README.md", "assets/fonts/source/HANDOFF.md", "assets/fonts/source/GensokyoSansUI/README.md",
                    "assets/fonts/source/GensokyoDisplayLab/README.md", "docs/ASSETS.md", "docs/TTF.md", "docs/LICENSING.md", "THIRD_PARTY_NOTICES.md"):
            for n, line in enumerate(read(rel).splitlines(), 1):
                self.assertNotRegex(line.lower(), r"licen[sc]e[^.]*(undecided|is not decided|not chosen|to be chosen)|undecided[^.]*licen[sc]e", "%s:%d" % (rel, n))


if __name__ == "__main__":
    unittest.main()
