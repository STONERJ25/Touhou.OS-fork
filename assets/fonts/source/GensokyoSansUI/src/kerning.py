"""Kerning of the accented letters: every variant of a letter inherits the pairs of its base.

spacing.py holds the pairs of the plain letters (KERN, keyed by two characters). An accented letter, a letter with a cedilla or a bar, takes its kerning
from the letter it is made of: T followed by ó is kerned as T followed by o, and so on for every pair, in both positions. The accent marks sit above the
x-height (or the capitals), well clear of the part of the letter that the kerning is about (the foot, the arm, the bowl), so the value of the base fits.
"""
from collections import defaultdict

from glyphset import glyph_name

# Variants whose shape beside the base letter is different where kerning looks: the apostrophe-like marks of d l t stand to the right of the letter.
SKIP_AS_LEFT = {"uni010F", "uni013E", "uni0165"}


def expand(face, kern):
    """kern: {"AV": -62, ...} for the plain letters; returns {(left glyph, right glyph): value} for them and for all their variants."""
    variants = defaultdict(list)
    for name, letter in face.base_of.items():
        variants[letter].append(name)
    pairs = {}
    for key, value in kern.items():
        left, right = key[0], key[1]
        lefts = sorted({glyph_name(ord(left))} | set(variants.get(left, ())))
        rights = sorted({glyph_name(ord(right))} | set(variants.get(right, ())))
        for lv in lefts:
            if lv in SKIP_AS_LEFT:
                continue
            for rv in rights:
                pairs[(lv, rv)] = value
    return pairs
