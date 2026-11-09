"""Render the revised interface face and all seven display faces."""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FAMILY = ROOT.parent
UI = FAMILY / "GensokyoSansUI" / "build/GensokyoSans-Regular.ttf"
NEW = (
    ("Frost", "ICE WARNING", "Faceted ice blocks", "#143a54", "#cbf3ff"),
    ("Kappa", "PUMP READY", "Segmented instrument display", "#182c29", "#bbf7ba"),
    ("Mischief", "NOT MY FAULT!", "Tumbling ink letters", "#47243c", "#ffdfad"),
    ("Seals", "BARRIER SET", "Letters cut into stamped tiles", "#eadab8", "#732e37"),
)
ORIGINAL = (
    ("Danmaku", "SPELL CARD 09", "#192038", "#c8f3ff"),
    ("Gap", "BOUNDARY ERROR", "#2a1941", "#f5d8ff"),
    ("Ofuda", "PAY UP, MARISA!", "#fae3b1", "#a4282a"),
)


def main():
    image = Image.new("RGB", (1190, 920), "#f3ede3")
    draw = ImageDraw.Draw(image)
    ink = "#302931"

    def line(x, y, text, size, color=ink, file=UI):
        draw.text((x, y), text, font=ImageFont.truetype(str(file), size), fill=color,
                  anchor="ls")

    line(36, 61, "Touhou.OS Font Set", 43)
    line(37, 91, "Revised everyday Sans  /  Seven display faces", 18, "#655a63")

    draw.rounded_rectangle((35, 116, 1155, 288), 11, fill="#fffdf8", outline="#ded3c9")
    line(57, 147, "Gensokyo Sans Regular 0.3", 19)
    line(57, 200, "Tea  Time   /   iii   iji   ii   Il1|", 42)
    line(57, 236, "12 px", 13)
    line(113, 236, "Title   Test   Tea   To   Ti   iii   iji   Reimu's Notebook", 12)
    line(57, 263, "16 px", 13)
    line(113, 263, "Title   Test   Tea   To   Ti   iii   iji   Reimu's Notebook", 16)
    line(691, 160, "Capital T", 19)
    line(691, 185, "Shorter crossbar, same cap height.", 16)
    line(691, 222, "Repeated i", 19)
    line(691, 247, "Narrower advance, centered dot, and pair tuning.", 16)

    for index, (style, sample, description, bg, fg) in enumerate(NEW):
        x, y = 35 + index % 2 * 568, 316 + index // 2 * 185
        draw.rounded_rectangle((x, y, x + 552, y + 169), 11, fill=bg)
        file = ROOT / "build" / f"Gensokyo{style}-Regular.ttf"
        line(x + 22, y + 36, style, 21, fg)
        line(x + 22, y + 111, sample, 61, fg, file)
        line(x + 22, y + 143, description, 15, fg)

    line(36, 728, "Original display trio", 20)
    for index, (style, sample, bg, fg) in enumerate(ORIGINAL):
        x, y = 35 + index * 379, 749
        draw.rounded_rectangle((x, y, x + 362, y + 136), 11, fill=bg)
        file = FAMILY / "GensokyoDisplayLab" / "build" / f"Gensokyo{style}-Regular.ttf"
        line(x + 20, y + 34, style, 17, fg)
        line(x + 20, y + 90, sample, 36, fg, file)
    out = FAMILY / "specimen" / "TouhouOS-Font-Set-v2.png"
    out.parent.mkdir(exist_ok=True)
    image.save(out)


if __name__ == "__main__":
    main()
