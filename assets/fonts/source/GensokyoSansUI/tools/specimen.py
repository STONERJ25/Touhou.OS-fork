"""Render the neutral font at actual UI sizes and next to the display trio."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "build" / "GensokyoSans-Regular.ttf"
DISPLAY = ROOT.parent / "GensokyoDisplayLab" / "build"
PAPER, INK, MUTED, RULE = "#fffdf8", "#302931", "#62575f", "#ddd2c8"


def main():
    image = Image.new("RGB", (1120, 740), "#f1ebe3")
    draw = ImageDraw.Draw(image)

    def line(x, y, string, size, fill=INK, path=FONT, **kwargs):
        font = ImageFont.truetype(str(path), size)
        draw.text((x, y), string, font=font, fill=fill, **kwargs)

    line(33, 19, "Gensokyo Sans", 47)
    line(36, 79, "Touhou.OS  /  Everyday interface type", 18, MUTED)
    draw.rounded_rectangle((35, 127, 613, 428), radius=11, fill=PAPER, outline=RULE)
    draw.rounded_rectangle((632, 127, 1085, 428), radius=11, fill=PAPER, outline=RULE)
    line(57, 145, "Notebook", 24)
    draw.line((57, 186, 590, 186), fill=RULE)
    for y, size in [(214, 11), (247, 12), (280, 14), (316, 16)]:
        line(58, y, f"{size} px", 13, MUTED, anchor="ls")
        line(113, y, "File   Edit   View    •    Open Notebook    Save As…", size, anchor="ls")
    draw.line((57, 339, 590, 339), fill=RULE)
    line(58, 357, "Reimu counted the donations twice.", 18)
    line(58, 386, "The total stayed at 009.", 18)
    line(653, 145, "Details", 24)
    draw.line((653, 186, 1064, 186), fill=RULE)
    line(653, 197, "ijtf  Il1|  0O  5S  2Z", 44)
    line(653, 259, "AV  To  Yo  rn  cl", 27)
    line(653, 312, "0123456789", 20)
    line(653, 342, "[] {} () <>   →  ←   “OK”", 19)
    line(653, 378, "Rounded diamond dots and a slight lift on the t.", 13, MUTED)
    line(36, 457, "The display trio", 20)
    panels = (
        ("Danmaku", "SPELL CARD 09", "#192038", "#c8f3ff"),
        ("Gap", "BOUNDARY ERROR", "#2a1941", "#f5d8ff"),
        ("Ofuda", "PAY UP, MARISA!", "#fae3b1", "#a4282a"),
    )
    for i, (style, sample, bg, fg) in enumerate(panels):
        x = 35 + 355 * i
        draw.rounded_rectangle((x, 497, x + 340, 642), radius=11, fill=bg)
        line(x + 18, 513, style, 17, fg)
        line(x + 18, 594, sample, 35, fg,
             DISPLAY / f"Gensokyo{style}-Regular.ttf", anchor="ls")
        line(x + 18, 613, ("Spell-card overlay", "System alert", "Shrine notice")[i], 13, fg)
    line(36, 673, "All interface labels on this sheet use Gensokyo Sans.", 15, MUTED)
    line(36, 698, "View at 100% to judge the small menu samples.", 15, MUTED)
    out = ROOT / "specimen" / "Gensokyo-Sans-UI.png"
    out.parent.mkdir(exist_ok=True)
    image.save(out)


if __name__ == "__main__":
    main()
