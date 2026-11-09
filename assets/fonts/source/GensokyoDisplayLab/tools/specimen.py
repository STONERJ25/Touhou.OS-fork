"""Render the three display prototypes in context, using the built TTFs."""
from pathlib import Path
from string import ascii_uppercase, digits

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
WIDTH = 1200
PANELS = (
    ("Danmaku", "Spell-card overlay", "SPELL CARD 09", "PERFECT FREEZE",
     "#192038", "#c8f3ff", "#94bcd2", "#4f6d89"),
    ("Gap", "System alert", "BOUNDARY ERROR", "REOPEN GAP?  Y/N",
     "#2a1941", "#f5d8ff", "#d3a9ed", "#71598c"),
    ("Ofuda", "Shrine notice", "PAY UP, MARISA!", "DONATION BOX: 0",
     "#fae3b1", "#a4282a", "#753827", "#cf9267"),
)


def face(style, size):
    return ImageFont.truetype(str(ROOT / "build" / f"Gensokyo{style}-Regular.ttf"), size)


def main():
    image = Image.new("RGB", (WIDTH, 1050), "#f1ebe2")
    draw = ImageDraw.Draw(image)
    label = ImageFont.load_default(size=16)
    title = ImageFont.load_default(size=34)
    draw.text((35, 19), "Gensokyo Display Lab", font=title, fill="#2c2532")
    draw.text((37, 62), "TOUHOU.OS   /   THREE FONTS FOR WHEN THE DESKTOP GETS LOUD",
              font=label, fill="#685b68")
    for index, panel in enumerate(PANELS):
        style, role, headline, subline, bg, ink, soft, rule = panel
        y = 103 + 306 * index
        left, right = 35, 1165
        draw.rounded_rectangle((left, y, right, y + 285), radius=13, fill=bg)
        draw.text((left + 24, y + 17), f"0{index + 1}  /  {style.upper()}",
                  font=label, fill=soft)
        draw.text((right - 24, y + 17), role, font=label, fill=soft, anchor="ra")
        draw.line((left + 24, y + 46, right - 24, y + 46), fill=rule)
        headline_face = face(style, 108)
        assert headline_face.getlength(headline) < right - left - 48
        draw.text((left + 24, y + 141), headline, font=headline_face,
                  anchor="ls", fill=ink)
        draw.text((left + 26, y + 190), subline, font=face(style, 36),
                  anchor="ls", fill=ink)
        draw.line((left + 24, y + 209, right - 24, y + 209), fill=rule)
        draw.text((left + 25, y + 254), ascii_uppercase + "   " + digits,
                  font=face(style, 30), anchor="ls", fill=soft)
    draw.text((37, 1020), "The samples use the generated fonts. Panel labels use the specimen tool font.",
              font=label, fill="#685b68")
    path = ROOT / "specimen" / "Gensokyo-Display-Lab.png"
    path.parent.mkdir(exist_ok=True)
    image.save(path)


if __name__ == "__main__":
    main()
