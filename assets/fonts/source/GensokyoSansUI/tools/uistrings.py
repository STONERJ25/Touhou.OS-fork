"""Which strings of the OS's programs are wider in Gensokyo Sans (13 px, as the toolkit sets them) than in the 8x16 bitmap font (8 px per character)?

    python3 assets/fonts/source/GensokyoSansUI/tools/uistrings.py [MIN_EXCESS_PX] [--all]      (from the repository root; on Windows: wsl -d Ubuntu -e python3 ...)

The desktop's layouts were made for the bitmap font, so a string that comes out wider in the new face is where a box could overflow. This lists the C string literals
of user/ (without the libc, the web engine, the compiler and the shell; printf conversions replaced by typical values) that are at least MIN_EXCESS pixels wider
(default 3), the share that are narrower, and the mean ratio. It measures with the toolkit's own `kc_text_width` through tests/host/ttftool.c (built with the sanitizers,
as tests/host/test_ttf.py does), so the numbers are what a program gets. Result on 2026-11-03: 5043 strings, 119 wider by 3 px or more (all capitals, up to 10 px),
4773 narrower, mean ratio 0.79.
"""
import codecs
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO / "tests" / "host"))
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SKIP = ("/libc/", "/web/", "/mcc/", "kappabox", "gapsh", "/ssh", "/vi/", "/js/")


def literals():
    seen = {}
    for f in sorted((REPO / "user").rglob("*.c")):
        rel = f.relative_to(REPO).as_posix()
        if any(s in rel for s in SKIP):
            continue
        for m in LIT.finditer(f.read_text(encoding="utf-8", errors="replace")):
            try:
                s = codecs.escape_decode(m.group(1).encode("utf-8"))[0].decode("utf-8", "ignore")
            except Exception:                                                    # noqa: BLE001
                continue
            s = re.sub(r"%[-+ 0#]*\d*(?:\.\d+)?(?:l|ll|z)?[dui]", "88", s)         # numbers: two digits
            s = re.sub(r"%[-+ 0#]*\d*(?:\.\d+)?s", "Name", s)
            s = re.sub(r"%[-+ 0#]*\d*(?:\.\d+)?[fg]", "8.8", s)
            if not (2 <= len(s) <= 90) or "\n" in s or "%" in s or not any(c.isalpha() for c in s) or not all(32 <= ord(c) < 127 for c in s):
                continue
            if "/" in s and " " not in s and len(s) > 20:
                continue                                                          # a path
            seen.setdefault(s, rel)
    return seen


def main():
    import test_ttf
    min_excess = next((int(a) for a in sys.argv[1:] if a.isdigit()), 3)
    strings = literals()
    exe = test_ttf.build_tool(sanitize=True, kappa=True)
    env = dict(os.environ, KAPPA_UI_FONT=str(REPO / "assets" / "fonts" / "GensokyoSans-Regular.ttf"), ASAN_OPTIONS="detect_leaks=0")
    script = "".join("kui measure hex:%s\n" % s.encode().hex() for s in strings)
    out = subprocess.run([str(exe)], input=script, capture_output=True, text=True, env=env).stdout.splitlines()
    rows = []
    for (s, rel), line in zip(strings.items(), out):
        w = int(re.search(r"width=(\d+)", line).group(1))
        rows.append((w - 8 * len(s), w, 8 * len(s), s, rel))
    wider = sorted((r for r in rows if r[0] >= min_excess), reverse=True)
    narrower = sum(1 for r in rows if r[0] < 0)
    print("%d strings: %d wider by %d px or more, %d narrower, mean ratio %.2f" % (len(rows), len(wider), min_excess, narrower, sum(r[1] / r[2] for r in rows) / len(rows)))
    for ex, w, b, s, rel in wider if "--all" in sys.argv else wider[:40]:
        print("  +%3d  gensokyo sans %3d  bitmap %3d  %-40r %s" % (ex, w, b, s, rel))


if __name__ == "__main__":
    main()
