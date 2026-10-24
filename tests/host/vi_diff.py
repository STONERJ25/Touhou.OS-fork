#!/usr/bin/env python
"""Differential test of the guest `vi` against a real vim on the host.  Not part of run_all.py (it needs vim on the host).

Random but reproducible key sequences (a family of generators below, a seed) are run through the host's vim
(`vim -u NONE -N -n -X -s keys file`, UTF-8, a 24x80 window) and through `vi -s keys file` in the guest; the saved files are
compared byte for byte.  The guest side is a small C program, tests/host/vifuzz.c, that runs all the cases inside one boot.

  python tests/host/vi_diff.py gen FAMILY COUNT SEED     make the cases, run vim on them, put them in initrd/fuzz/FAMILY.dat
  python tests/host/vi_diff.py guest FAMILY [FROM [TO]]  build, boot, run the cases in the guest, write the report
  python tests/host/vi_diff.py sig FAMILY                the differing cases grouped by their keys
  python tests/host/vi_diff.py show FAMILY IDX [IDX...]  input, keys, and a diff of vim's and vi's result for cases
  python tests/host/vi_diff.py clean                     remove initrd/fuzz and user/bin/vifuzz.c again (never commit them)

FAMILY is one of: motion (a motion, then iX<Esc> marks the cursor), op (operator + motion), obj (text objects), seq (random
command sequences), ex (ex commands with ranges).  The runs of the pause (2026-10-23) were: motion 800 seed 21, op 2500 seed 12,
obj 1500 seed 13, seq 2000 seed 14, ex 1500 seed 15.  Keys use the notation of tests/vilib.K.  vim is found through $VIM, PATH, or
Git for Windows' usr/bin/vim.exe.  Cases where Esc is directly followed by O or [ are dropped (a terminal can read them as one key).
"""
import collections
import concurrent.futures
import difflib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build" / "vi_diff"
FUZZ = ROOT / "initrd" / "fuzz"
RUNNER_SRC = Path(__file__).resolve().parent / "vifuzz.c"
RUNNER_DST = ROOT / "user" / "bin" / "vifuzz.c"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))


def find_vim():
    for cand in (os.environ.get("VIM"), shutil.which("vim"), r"C:\Program Files\Git\usr\bin\vim.exe"):
        if cand and Path(cand).exists():
            return cand
    sys.exit("vi_diff: no vim found (set $VIM)")


SAMPLES = {
    "prose": ("The quick brown fox jumps over the lazy dog.\n"
              "  Indented line with   multiple   spaces here\n"
              "foo(bar, baz[1], {qux: \"q\"}) end\n"
              "another line; with punctuation, and more...\n"
              "\n"
              "second paragraph line one. Second sentence here!  Third? yes.\n"
              "last line\n"),
    "code": ("int main(int argc, char **argv)\n"
             "{\n"
             "    if (argc > 1) {\n"
             "        printf(\"hello %s\\n\", argv[1]);\n"
             "    }\n"
             "    return 0;\n"
             "}\n"),
    "misc": ("item 1: 007 x 0x0f -5 foo_bar baz-qux 12345\n"
             "a.b.c  d,e;f  <tag>text</tag> 'single' \"double\"\n"
             "\t\u00e9llo w\u00f6rld\tTab\there\n"
             "\n"
             "\n"
             "  trailing   \n"
             "END\n"),
    "short": "alpha beta\ngamma\n\ndelta epsilon zeta\n",
}

MOTIONS = ["h", "j", "k", "l", "w", "W", "b", "B", "e", "E", "ge", "gE", "0", "^", "$", "g_", "|", "fo", "Fo", "te", "To", "f ", ";", ",",
           "%", "(", ")", "{", "}", "[[", "]]", "G", "gg", "+", "-", "_", "<CR>", "<BS>", "<Space>", "*", "#", "n", "N",
           "/the<CR>", "?e<CR>", "/\\<s<CR>", "/e/e<CR>", "/o/+1<CR>", "/n/s-1<CR>", "'a", "`a", "H", "L", "M", "gj", "gk", "go"]
OPS = ["d", "c", "y", ">", "<", "gu", "gU", "g~", "g?", "gq", "!"]
DOUBLE = {"d": "dd", "c": "cc", "y": "yy", ">": ">>", "<": "<<", "gu": "guu", "gU": "gUU", "g~": "g~~", "g?": "g??", "gq": "gqq", "!": "!!"}
OBJECTS = ["iw", "aw", "iW", "aW", "is", "as", "ip", "ap", "i(", "a(", "ib", "ab", "i{", "a{", "iB", "aB", "i[", "a[", "i<", "a<",
           "i\"", "a\"", "i'", "a'", "i`", "a`", "it", "at"]
TEXTS = ["foo", "bar baz", "x y z", "12", "(a)", "\"q\"", "two\nlines", "  lead", "tail  "]
EDIT_TOKENS = ["x", "X", "D", "J", "gJ", "p", "P", "gp", "gP", "Y", "yy", "dd", ">>", "<<", ".", "u", "<C-r>", "U", "<C-a>", "<C-x>", "&",
               "~", "rx", "r<CR>", "5x", "3dd", "2yy", "3p", "2P", "3J", "5<C-a>", "10<C-x>", "ddp", "yyP", "xp", "ddkP", "guu", "gUU", "g~~"]
INSERT_CMDS = ["i", "a", "I", "A", "o", "O", "gi", "gI", "cw", "cc", "C", "s", "S", "cap", "ciw", "c$", "R"]
RANGES = ["", "", "", "%", "1,3", "2,$", ".,+2", ".", "$", "1", "2;+1", "'a,'b", "/the/,/line/", "/e/", "?o?", "$-1", ".+1", "1,$", "3,2",
          ".,.+1", "-1,+1", "/the/;+1", "0", "1,2"]
EX_CMDS = ["d", "y", "m0", "m$", "m.", "m+1", "t0", "t$", "t.", "co 1", "j", "j!", "j 3", ">", "<", "> 2", ">>", "normal Ax", "normal dd", "normal ~",
           "normal 2x", "s/o/0/", "s/o/0/g", "s/\\w\\+/[&]/g", "s/^/> /", "s/$/;/", "s/\\s\\+/ /g", "&&", "&", "g/the/d", "g!/the/d", "g/^$/d",
           "v/e/s/e/E/", "g/a/m0", "g/e/t.", "g/e/normal Ax", "g/./j", "s/a/\\r/", "s/\\(\\w\\)\\(\\w\\)/\\2\\1/", "s/e/E/gc", "s/\\<./\\u&/g", "s/.*/\\U&/",
           "s/\\w\\+/\\L&/", "sort", "sort!", "sort n", "sort u", "sort i", "sort /\\w\\+/", "pu", "pu!", "ka", "mark b", "retab", "left 4", "right 30",
           "center 30", "noh", "s/\\(.\\)\\1/<\\1\\1>/g", "s/[aeiou]/*/g", "s/\\d\\+/N/", "s/./&&/", "s/o\\|e/X/g", "s/\\(a\\|e\\)\\+/_/", "s/ \\+$//",
           "s/\\S\\+/(&)/2", "s/$/ END/", "g/e/s//E/", "g/^/m0", "d x", "y a", "pu a", "norm! u", "s/\\n//", "s/a\\nb/X/", "undo", "redo", "noh"]


def to_bytes(keys):
    from vilib import K
    return K(keys).encode("utf-8")


def positions(rng, text):
    """A key string that moves to a random position (line, column) of the text."""
    lines = text.split("\n")[:-1]
    l = rng.randrange(len(lines))
    c = rng.randrange(max(1, len(lines[l])))
    return f"{l + 1}G{c + 1}|"


# ------------------------------------------------------------------------------------------------ the generators

def fam_motion(rng, text):
    pos = positions(rng, text)
    m = rng.choice(MOTIONS)
    cnt = rng.choice(["", "", "2", "3"])
    pre = "majmb" if "'a" in m or "`a" in m else ""
    if m in ("fo", "Fo", "te", "To", "f ", ";", ","):
        pre += "fo"
    return pos + pre + cnt + m + "iX<Esc>"


def fam_op(rng, text):
    pos = positions(rng, text)
    op = rng.choice(OPS[:10])
    m = rng.choice(MOTIONS)
    c1 = rng.choice(["", "", "", "2"])
    c2 = rng.choice(["", "", "", "2", "3"])
    pre = "majmb" if "'a" in m or "`a" in m else ""
    if m in (";", ","):
        pre += "fo"
    force = rng.choice(["", "", "", "", "v", "V"])
    body = f"{c1}{DOUBLE[op]}" if rng.random() < 0.12 else f"{c1}{op}{c2}{force}{m}"
    tail = "X<Esc>" if op == "c" else "Gp" if op == "y" else ""
    return pos + pre + body + tail


def fam_obj(rng, text):
    pos = positions(rng, text)
    op = rng.choice(["d", "c", "y", "gU", ">", "g?", "gu"])
    obj = rng.choice(OBJECTS)
    cnt = rng.choice(["", "", "", "2", "3"])
    if rng.random() < 0.15:     # visual mode
        return pos + rng.choice(["v", "V"]) + cnt + obj + rng.choice(["d", "y", "c", "o", ">", "U"])
    return pos + f"{op}{cnt}{obj}" + {"c": "X<Esc>", "y": "Gp"}.get(op, "")


def insert_seq(rng):
    cmd = rng.choice(INSERT_CMDS)
    cnt = rng.choice(["", "", "", "2", "3"]) if cmd in ("i", "a", "I", "A", "o", "O", "gi", "gI", "R") else ""
    t = rng.choice(TEXTS).replace("\n", "<CR>")
    extra = rng.choice(["", "", "", "<C-w>", "<C-u>", "<BS>", "<C-t>", "<C-d>", "<C-r>a", "<C-a>", "<C-o>x", "<Left>x", "<C-h>"])
    return f"{cnt}{cmd}{t}{extra}<Esc>"


def one_token(rng, text):
    r = rng.random()
    if r < 0.25:
        return rng.choice(MOTIONS) if rng.random() < 0.5 else rng.choice(["2", "3"]) + rng.choice(MOTIONS[:20])
    if r < 0.50:
        return rng.choice(EDIT_TOKENS)
    if r < 0.68:
        return insert_seq(rng)
    if r < 0.82:
        return rng.choice(["", "", "2"]) + rng.choice(OPS[:9]) + rng.choice([x for x in MOTIONS if x != "/e/e<CR>"])
    if r < 0.88:
        return positions(rng, text)
    if r < 0.92:
        return rng.choice(['"ayy', '"ap', '"Ayy', '"add', '"aP', '"_dd', '"1p', '"2p', '"-p', 'ma', "'a", '`a', "d'a", "y`a"])
    if r < 0.96:
        return rng.choice(["vwd", "vey", "Vjd", "Vjy", "vjy", "vawd", "viwc", "vipd", "vee<Esc>", "VjjJ", "vlU", "vlu", "v$d", "vj>", "Vk<"])
    return rng.choice([":s/o/0/<CR>", ":s/o/0/g<CR>", ":%s/e/E/g<CR>", ":1,3d<CR>", ":2,4m0<CR>", ":t.<CR>", ":j<CR>", ":>", ":normal Ax<CR>",
                       ":g/the/d<CR>", ":%j<CR>", ":2,3j!<CR>", ":$<CR>", ":%norm Ax<CR>", ":.,+1s/\\w\\+/<&>/<CR>",
                       ":s/\\(\\w\\+\\) \\(\\w\\+\\)/\\2 \\1/<CR>", ":g/^/m0<CR>", ":v/e/d<CR>", ":%s/\\s\\+$//e<CR>", ":%s/^/> /<CR>",
                       ":2,3><CR>", ":sort<CR>", ":sort!<CR>"])


def fam_seq(rng, text):
    toks = [positions(rng, text)] if rng.random() < 0.6 else []
    for _ in range(rng.randint(2, 7)):
        toks.append(one_token(rng, text))
    s = "".join(toks)
    if rng.random() < 0.2:      # a recorded macro
        body = "".join(one_token(rng, text) for _ in range(rng.randint(1, 3)))
        s += "qa" + body + "q" + rng.choice(["@a", "2@a", "@a@@", "3@a"])
    return s


def fam_ex(rng, text):
    toks = ["majmb" if rng.random() < 0.5 else "", positions(rng, text)]
    for _ in range(rng.randint(1, 3)):
        toks.append(":" + rng.choice(RANGES) + rng.choice(EX_CMDS) + "<CR>")
        if rng.random() < 0.15:
            toks.append(rng.choice(["u", "<C-r>", ".", "@:", "&", "g&", "n", "x", "p", "yyp"]))
        if rng.random() < 0.2:
            toks.append("n")
    return "".join(toks).replace(":s/e/E/gc<CR>", ":s/e/E/gc<CR>yny")


FAMILIES = {"motion": fam_motion, "op": fam_op, "obj": fam_obj, "seq": fam_seq, "ex": fam_ex}


def gen_cases(family, count, seed):
    rng = random.Random(seed)
    cases, guard = [], 0
    while len(cases) < count and guard < count * 20:
        guard += 1
        sname = rng.choice(sorted(SAMPLES))
        keys = FAMILIES[family](rng, SAMPLES[sname])
        if re.search(r"<Esc>[O\[]", keys):
            continue
        cases.append({"sample": sname, "input": SAMPLES[sname], "keys": keys + "<Esc><Esc>:wq<CR>"})
    return cases


# ------------------------------------------------------------------------------------------------ the host side

def run_vim(case, workdir, vim):
    d = tempfile.mkdtemp(dir=workdir)
    f, k = os.path.join(d, "x"), os.path.join(d, "k")
    Path(f).write_bytes(case["input"].encode("utf-8"))
    Path(k).write_bytes(to_bytes(case["keys"]))
    env = dict(os.environ, TERM="xterm", LINES="24", COLUMNS="80")
    try:
        p = subprocess.run([vim, "-u", "NONE", "-N", "-n", "-X", "-i", "NONE", "--cmd", "set encoding=utf-8", "-c", "set nf=hex", "-s", k, f],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15, env=env)
    except subprocess.TimeoutExpired:
        return None
    out = Path(f).read_bytes() if p.returncode == 0 else None
    shutil.rmtree(d, ignore_errors=True)
    return out


def cmd_gen(family, count, seed):
    vim = find_vim()
    WORK.mkdir(parents=True, exist_ok=True)
    cases = gen_cases(family, count, seed)
    tmp = tempfile.mkdtemp(prefix="vifz")
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as ex:
        results = list(ex.map(lambda c: run_vim(c, tmp, vim), cases))
    print(f"vim: {len(cases)} cases in {time.time() - t0:.1f} s")
    keep = []
    for c, r in zip(cases, results):
        if r is not None:
            c["expected"] = r.decode("latin-1")
            keep.append(c)
    print(f"{len(keep)} usable cases ({len(cases) - len(keep)} dropped because vim failed)")
    buf = bytearray(b"VFZ1")
    for i, c in enumerate(keep):
        for part in (f"{family}{i}".encode(), c["input"].encode("utf-8"), to_bytes(c["keys"]), c["expected"].encode("latin-1")):
            buf += struct.pack("<I", len(part)) + part
    FUZZ.mkdir(parents=True, exist_ok=True)
    (FUZZ / f"{family}.dat").write_bytes(bytes(buf))
    shutil.copyfile(RUNNER_SRC, RUNNER_DST)
    (WORK / f"cases_{family}.json").write_text(json.dumps(keep), encoding="utf-8")
    print(f"wrote {FUZZ / (family + '.dat')} and {RUNNER_DST} (temporary: `vi_diff.py clean` removes them)")


def cmd_clean():
    shutil.rmtree(FUZZ, ignore_errors=True)
    RUNNER_DST.unlink(missing_ok=True)
    print("removed initrd/fuzz and user/bin/vifuzz.c")


# ------------------------------------------------------------------------------------------------ the guest side

def disp(b):
    """Bytes (or a latin-1 str holding bytes) shown as UTF-8 text."""
    if isinstance(b, str):
        b = b.encode("latin-1")
    return b.decode("utf-8", "backslashreplace")


def cmd_guest(family, first=0, last=1 << 30):
    from vilib import Guest, Qemu
    subprocess.run([sys.executable, str(ROOT / "tools" / "build.py")], check=True, stdout=subprocess.DEVNULL)
    cases = json.loads((WORK / f"cases_{family}.json").read_text(encoding="utf-8"))
    with Qemu(cmdline="text", name="t-vi-diff") as vm:
        g = Guest(vm)
        s = g.mark()
        vm.send(f"vifuzz /fuzz/{family}.dat {first} {last}\n")
        t0 = time.time()
        m = vm.expect(r"DONE ran=(\d+) diff=(\d+) hang=(\d+) crash=(\d+)", timeout=3000)
        print(f"guest: {time.time() - t0:.1f} s", m.group(0))
        data = vm.serial_path.read_bytes()[s:].decode("latin-1")
    rep = []
    for mm in re.finditer(r"(DIFF|HANG|CRASH) (\d+) (\S*)[^\n]*\r?\n(?:GOT ([0-9a-f]*)\r?\n)?", data):
        rep.append({"idx": int(mm.group(2)), "kind": mm.group(1), "got": mm.group(4)})
    (WORK / f"report_{family}.json").write_text(json.dumps(rep), encoding="utf-8")
    print(f"{len(rep)} problem cases; look at them with `sig {family}` and `show {family} IDX`")


def load_report(family):
    cases = json.loads((WORK / f"cases_{family}.json").read_text(encoding="utf-8"))
    rep = json.loads((WORK / f"report_{family}.json").read_text(encoding="utf-8"))
    return cases, rep


def cmd_sig(family):
    cases, rep = load_report(family)
    groups = collections.defaultdict(list)
    for r in rep:
        k = cases[r["idx"]]["keys"].replace("<Esc><Esc>:wq<CR>", "")
        k = re.sub(r"^\d+G\d+\|", "", k)
        k = re.sub(r"(iX<Esc>|X<Esc>|Gp|majmb)", "", k)
        groups[re.sub(r"\d+", "N", k)].append(r["idx"])
    for sig, idxs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"{len(idxs):4}  {sig!r:40} {' '.join(map(str, idxs[:6]))}")


def cmd_show(family, idxs):
    cases, rep = load_report(family)
    by_idx = {r["idx"]: r for r in rep}
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    for i in idxs:
        c, r = cases[i], by_idx.get(i)
        print(f"# case {i} {r['kind'] if r else 'ok'} sample={c['sample']} keys={c['keys']!r}")
        for n, l in enumerate(c["input"].split("\n"), 1):
            print(f"   {n:2}| {l}")
        vi = disp(bytes.fromhex(r["got"])) if r and r["got"] is not None else ""
        for l in list(difflib.unified_diff(disp(c["expected"]).split("\n"), vi.split("\n"), "vim", "vi", lineterm="", n=0))[2:16]:
            print("   ", l)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "gen" and len(sys.argv) == 5:
        cmd_gen(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    elif cmd == "guest" and len(sys.argv) >= 3:
        cmd_guest(sys.argv[2], *[int(a) for a in sys.argv[3:5]])
    elif cmd == "sig" and len(sys.argv) == 3:
        cmd_sig(sys.argv[2])
    elif cmd == "show" and len(sys.argv) >= 4:
        cmd_show(sys.argv[2], [int(a) for a in sys.argv[3:]])
    elif cmd == "clean":
        cmd_clean()
    else:
        print(__doc__)
        sys.exit(2)
