"""Kerning, in font units. FIRST_PASS is the 61 legacy pairs of version 0.3, untouched; SECOND_PASS (version 0.4) adds the pairs of the specification's classes
that the first pass left open (the font specification section 6: A V W Y T P F L against the round lower case, v w y and the punctuation; r against , . - a c d e g o q).
Each value is a little under half of the excess that tools/kernscan.py measures for the pair (the mean gap between the two outlines against the median of its
class), which is how far the first pass went on average, and it was looked at in rendered words at 13, 26 and 44 px (the Te, Tc, Ts, Fa, Fo, Ly, W., T, and r,
pairs left visible holes). Pairs the scan would add for stems against stems (Lb, Lh ...) are left alone on purpose: the foot of an L is part of its shape."""
SECOND_PASS = {
    "Tc": -40, "Td": -38, "Tq": -38, "Ts": -38, "Tv": -30, "Tw": -30, "T,": -62, "T-": -55,
    "Fa": -38, "Fe": -30, "Fo": -30, "Fc": -28, "F,": -66,
    "Pa": -26, "Pe": -22, "Po": -22, "P,": -80,
    "Wa": -26, "We": -22, "W.": -62, "W,": -62,
    "Ly": -34, "Lv": -28, "Lw": -22,
    "Yc": -36, "Ys": -32, "Y,": -78, "V,": -67,
    "Av": -26, "Aw": -20, "Ay": -26,
    "r,": -52, "rc": -16, "rd": -16, "re": -16, "rg": -16, "rq": -16,
}

FIRST_PASS = {
    "AV": -62, "AW": -48, "AY": -62, "AT": -47, "AU": -16, "AO": -16,
    "VA": -62, "WA": -44, "YA": -62, "TA": -45, "LA": -12,
    "LV": -63, "LW": -42, "LY": -62, "LT": -56, "FA": -41, "PA": -49,
    "Te": -43, "To": -50, "Ta": -50, "Ti": -35, "Tu": -36, "Tr": -28, "Ty": -40,
    "Yo": -61, "Ye": -54, "Ya": -59, "Vo": -35, "Va": -40, "Wo": -26,
    "V.": -67, "Y.": -78, "T.": -65, "F.": -66, "P.": -80, "r.": -52,
    "av": -20, "aw": -18, "ay": -20, "fa": -22, "fe": -14, "fo": -18,
    "ii": -5, "ij": -6, "ji": -8, "ft": 18, "rf": 12, "ro": -18, "ra": -14, "rn": 24, "rl": 14,
    "cl": 14, "ta": -12, "te": -10, "to": -12, "va": -26, "vo": -19,
    "wa": -20, "wo": -16, "ya": -24, "yo": -20,
}

assert not set(FIRST_PASS) & set(SECOND_PASS), "the second pass only adds pairs"
KERN = {**FIRST_PASS, **SECOND_PASS}
