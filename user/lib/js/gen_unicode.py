#!/usr/bin/env python3
"""Generates user/lib/js/src/unicode_tables.h from the Unicode Character Database.

  python3 user/lib/js/gen_unicode.py [UCD directory]      (default /usr/share/unicode, Ubuntu's unicode-data package)

The tables are data, not code: general categories, the binary properties ECMAScript regular expressions accept,
scripts and script extensions, case mappings and case folding, and the normalization data (combining classes,
decompositions, primary composites).  The output is deterministic, so regenerating with the same UCD gives the same
file.  The engine is built against Unicode 15.1 (what ES2024 names).
"""
import os
import sys

UCD = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unicode"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "unicode_tables.h")
MAXCP = 0x110000

CATS = ["Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf",
        "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co"]
CAT_INDEX = {c: i for i, c in enumerate(CATS)}


def ucd(name):
    return os.path.join(UCD, name)


def lines(name):
    with open(ucd(name), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                yield [x.strip() for x in line.split(";")]


def parse_range(s):
    if ".." in s:
        a, b = s.split("..")
        return int(a, 16), int(b, 16)
    return int(s, 16), int(s, 16)


def version():
    with open(ucd("DerivedCoreProperties.txt"), encoding="utf-8") as f:
        first = f.readline()
    return first.strip("# \n").replace("DerivedCoreProperties-", "").replace(".txt", "")


# ------------------------------------------------------------------ UnicodeData.txt

gc = [0] * MAXCP
simple_upper, simple_lower, simple_title = {}, {}, {}
ccc = [0] * MAXCP
decomp = {}           # cp -> (compat, [cps])
bidi_mirrored = set()
first = None
with open(ucd("UnicodeData.txt"), encoding="utf-8") as f:
    for line in f:
        p = line.rstrip("\n").split(";")
        cp = int(p[0], 16)
        name = p[1]
        if name.endswith(", First>"):
            first = cp
            continue
        lo = first if name.endswith(", Last>") else cp
        first = None
        for c in range(lo, cp + 1):
            gc[c] = CAT_INDEX[p[2]]
            ccc[c] = int(p[3])
            if p[9] == "Y":
                bidi_mirrored.add(c)
        if p[5]:
            parts = p[5].split()
            compat = parts[0].startswith("<")
            if compat:
                parts = parts[1:]
            decomp[cp] = (compat, [int(x, 16) for x in parts])
        if p[12]:
            simple_upper[cp] = int(p[12], 16)
        if p[13]:
            simple_lower[cp] = int(p[13], 16)
        if p[14]:
            simple_title[cp] = int(p[14], 16)


def gc_ranges():
    out = []
    prev = None
    for c in range(MAXCP):
        if gc[c] != prev:
            out.append((c, gc[c]))
            prev = gc[c]
    return out


# ------------------------------------------------------------------ binary properties

def prop_file(name):
    props = {}
    for p in lines(name):
        a, b = parse_range(p[0])
        props.setdefault(p[1], set()).update(range(a, b + 1))
    return props


core = prop_file("DerivedCoreProperties.txt")
plist = prop_file("PropList.txt")
emoji = prop_file(os.path.join("emoji", "emoji-data.txt"))
norm_props = {}
for p in lines("DerivedNormalizationProps.txt"):
    if len(p) == 2:
        a, b = parse_range(p[0])
        norm_props.setdefault(p[1], set()).update(range(a, b + 1))

assigned = {c for c in range(MAXCP) if gc[c] != 0}
binary = {
    "ASCII": set(range(0x80)),
    "ASCII_Hex_Digit": plist["ASCII_Hex_Digit"],
    "Alphabetic": core["Alphabetic"],
    "Any": set(range(MAXCP)),
    "Assigned": assigned,
    "Bidi_Control": plist["Bidi_Control"],
    "Bidi_Mirrored": bidi_mirrored,
    "Case_Ignorable": core["Case_Ignorable"],
    "Cased": core["Cased"],
    "Changes_When_Casefolded": core["Changes_When_Casefolded"],
    "Changes_When_Casemapped": core["Changes_When_Casemapped"],
    "Changes_When_Lowercased": core["Changes_When_Lowercased"],
    "Changes_When_NFKC_Casefolded": norm_props["Changes_When_NFKC_Casefolded"],
    "Changes_When_Titlecased": core["Changes_When_Titlecased"],
    "Changes_When_Uppercased": core["Changes_When_Uppercased"],
    "Dash": plist["Dash"],
    "Default_Ignorable_Code_Point": core["Default_Ignorable_Code_Point"],
    "Deprecated": plist["Deprecated"],
    "Diacritic": plist["Diacritic"],
    "Emoji": emoji["Emoji"],
    "Emoji_Component": emoji["Emoji_Component"],
    "Emoji_Modifier": emoji["Emoji_Modifier"],
    "Emoji_Modifier_Base": emoji["Emoji_Modifier_Base"],
    "Emoji_Presentation": emoji["Emoji_Presentation"],
    "Extended_Pictographic": emoji["Extended_Pictographic"],
    "Extender": plist["Extender"],
    "Grapheme_Base": core["Grapheme_Base"],
    "Grapheme_Extend": core["Grapheme_Extend"],
    "Hex_Digit": plist["Hex_Digit"],
    "IDS_Binary_Operator": plist["IDS_Binary_Operator"],
    "IDS_Trinary_Operator": plist["IDS_Trinary_Operator"],
    "ID_Continue": core["ID_Continue"],
    "ID_Start": core["ID_Start"],
    "Ideographic": plist["Ideographic"],
    "Join_Control": plist["Join_Control"],
    "Logical_Order_Exception": plist["Logical_Order_Exception"],
    "Lowercase": core["Lowercase"],
    "Math": core["Math"],
    "Noncharacter_Code_Point": plist["Noncharacter_Code_Point"],
    "Pattern_Syntax": plist["Pattern_Syntax"],
    "Pattern_White_Space": plist["Pattern_White_Space"],
    "Quotation_Mark": plist["Quotation_Mark"],
    "Radical": plist["Radical"],
    "Regional_Indicator": plist["Regional_Indicator"],
    "Sentence_Terminal": plist["Sentence_Terminal"],
    "Soft_Dotted": plist["Soft_Dotted"],
    "Terminal_Punctuation": plist["Terminal_Punctuation"],
    "Unified_Ideograph": plist["Unified_Ideograph"],
    "Uppercase": core["Uppercase"],
    "Variation_Selector": plist["Variation_Selector"],
    "White_Space": plist["White_Space"],
    "XID_Continue": core["XID_Continue"],
    "XID_Start": core["XID_Start"],
}

# aliases from PropertyAliases.txt (only those of the properties above)
aliases = {}
for p in lines("PropertyAliases.txt"):
    long_name = p[1]
    if long_name in binary:
        for a in [p[0]] + p[2:]:
            if a != long_name:
                aliases[a] = long_name
aliases.pop("Any", None)

# ------------------------------------------------------------------ general category and script value names

gc_names = {}        # name -> bitmask of CATS
script_names = {}    # name -> script index
scripts = []         # long names
for p in lines("PropertyValueAliases.txt"):
    if p[0] == "gc":
        names = p[1:]
        short = names[0]
        if short in CAT_INDEX:
            mask = 1 << CAT_INDEX[short]
        else:
            members = {"C": ["Cc", "Cf", "Cn", "Co", "Cs"], "L": ["Ll", "Lm", "Lo", "Lt", "Lu"], "LC": ["Ll", "Lt", "Lu"],
                       "M": ["Mc", "Me", "Mn"], "N": ["Nd", "Nl", "No"], "P": ["Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps"],
                       "S": ["Sc", "Sk", "Sm", "So"], "Z": ["Zl", "Zp", "Zs"]}[short]
            mask = 0
            for m in members:
                mask |= 1 << CAT_INDEX[m]
        for n in names:
            gc_names[n] = mask
    elif p[0] == "sc":
        idx = len(scripts)
        scripts.append(p[2])
        for n in p[1:]:
            script_names[n] = idx

script_of = [script_names["Zzzz"]] * MAXCP
for p in lines("Scripts.txt"):
    a, b = parse_range(p[0])
    i = script_names[p[1]]
    for c in range(a, b + 1):
        script_of[c] = i
scx = {}
for p in lines("ScriptExtensions.txt"):
    a, b = parse_range(p[0])
    vals = [script_names[x] for x in p[1].split()]
    for c in range(a, b + 1):
        scx[c] = vals

script_sets = [set() for _ in scripts]
scx_sets = [set() for _ in scripts]
for c in range(MAXCP):
    script_sets[script_of[c]].add(c)
    for s in scx.get(c, [script_of[c]]):
        scx_sets[s].add(c)

# ------------------------------------------------------------------ case mapping and folding

special_upper, special_lower = {}, {}
for p in lines("SpecialCasing.txt"):
    if len(p) > 4 and p[4]:
        continue          # conditional (language or context): handled in code where the specification asks
    cp = int(p[0], 16)
    lower = [int(x, 16) for x in p[1].split()]
    upper = [int(x, 16) for x in p[3].split()]
    if lower != [cp]:
        special_lower[cp] = lower
    if upper != [cp]:
        special_upper[cp] = upper
fold_simple = {}
for p in lines("CaseFolding.txt"):
    if p[1] in ("C", "S"):
        fold_simple[int(p[0], 16)] = int(p[2], 16)


def runs(mapping):
    """(start, len, step, delta) runs covering every mapped code point (others map to themselves)"""
    items = sorted(mapping.items())
    out = []
    i = 0
    while i < len(items):
        c, m = items[i]
        delta = m - c
        # try a stride-1 run, then a stride-2 run
        j = i + 1
        while j < len(items) and items[j][0] == c + (j - i) and items[j][1] - items[j][0] == delta:
            j += 1
        best = (j - i, 1)
        k = i + 1
        while k < len(items) and items[k][0] == c + 2 * (k - i) and items[k][1] - items[k][0] == delta:
            k += 1
        if k - i > best[0]:
            best = (k - i, 2)
        n, step = best
        while n > 0:
            take = min(n, 0xFFFF)
            out.append((c, take, step, delta))
            c += take * step
            n -= take
        i += best[0]
    return out


upper_runs = runs(simple_upper)
lower_runs = runs(simple_lower)
fold_runs = runs(fold_simple)

# ------------------------------------------------------------------ normalization

ccc_ranges = []
prev = None
for c in range(MAXCP):
    if ccc[c] != prev:
        ccc_ranges.append((c, ccc[c]))
        prev = ccc[c]

excl = norm_props["Full_Composition_Exclusion"]
compose = []
for cp, (compat, d) in sorted(decomp.items()):
    if not compat and len(d) == 2 and cp not in excl:
        compose.append((d[0], d[1], cp))
compose.sort()

# single code point decompositions in runs; the rest explicit with a pool
single = {cp: (compat, d[0]) for cp, (compat, d) in decomp.items() if len(d) == 1}
multi = {cp: v for cp, v in decomp.items() if len(v[1]) > 1}
decomp_runs = []
items = sorted(single.items())
i = 0
while i < len(items):
    c, (compat, m) = items[i]
    j = i + 1
    while (j < len(items) and items[j][0] == c + (j - i) and items[j][1][0] == compat and
           items[j][1][1] - items[j][0] == m - c):
        j += 1
    decomp_runs.append((c, j - i, 1 if compat else 0, m - c))
    i = j
pool = []
multi_entries = []
for cp, (compat, d) in sorted(multi.items()):
    multi_entries.append((cp, 1 if compat else 0, len(d), len(pool)))
    pool.extend(d)

# ------------------------------------------------------------------ output


def ranges_of(s):
    out = []
    for c in sorted(s):
        if out and out[-1][1] == c - 1:
            out[-1][1] = c
        else:
            out.append([c, c])
    return out


def varint(n, out):
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def encode(s):
    """a range list as varints: (gap from the previous end + 1, length - 1) pairs"""
    out = []
    pos = 0
    rs = ranges_of(s)
    for a, b in rs:
        varint(a - pos, out)
        varint(b - a, out)
        pos = b + 1
    return out, len(rs)


def c_array(name, ctype, values, per_line=12, fmt="0x%x"):
    lines_ = [f"static const {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines_.append("    " + ", ".join(fmt % v for v in values[i:i + per_line]) + ",")
    lines_.append("};")
    return "\n".join(lines_)


def c_string(s):
    return '"' + s + '"'


out = []
out.append("/* Generated by user/lib/js/gen_unicode.py from the Unicode Character Database %s: do not edit. */" % version())
out.append("#define KZ_UNICODE_VERSION \"%s\"" % version())
out.append("enum { %s, KZ_GC_COUNT };" % ", ".join("KZ_GC_" + c for c in CATS))

g = gc_ranges()
out.append(c_array("kz_gc_ranges", "uint32_t", [(c << 5) | k for c, k in g], 10))

for name in ("ID_Start", "ID_Continue"):
    rs = ranges_of(binary[name])
    flat = []
    for a, b in rs:
        flat += [a, b]
    out.append(c_array("kz_%s" % name.lower(), "uint32_t", flat, 10))

# binary properties: one byte stream, an offset table, and names (sorted for binary search)
stream = []
prop_off = []
prop_count = []
bnames = sorted(binary)
for n in bnames:
    enc, cnt = encode(binary[n])
    prop_off.append(len(stream))
    prop_count.append(cnt)
    stream += enc
script_off, script_cnt, scx_off, scx_cnt = [], [], [], []
for i in range(len(scripts)):
    enc, cnt = encode(script_sets[i])
    script_off.append(len(stream))
    script_cnt.append(cnt)
    stream += enc
    enc, cnt = encode(scx_sets[i])
    scx_off.append(len(stream))
    scx_cnt.append(cnt)
    stream += enc
out.append(c_array("kz_uprop_stream", "uint8_t", stream, 24))
out.append("typedef struct KzUProp { const char *name; uint32_t off, count; } KzUProp;")
out.append("static const KzUProp kz_binary_props[%d] = {" % len(bnames))
for i, n in enumerate(bnames):
    out.append("    { %s, %d, %d }," % (c_string(n), prop_off[i], prop_count[i]))
out.append("};")
al = sorted(aliases.items())
out.append("typedef struct KzUAlias { const char *name; const char *target; } KzUAlias;")
out.append("static const KzUAlias kz_binary_aliases[%d] = {" % len(al))
for a, t in al:
    out.append("    { %s, %s }," % (c_string(a), c_string(t)))
out.append("};")
out.append("typedef struct KzUScript { const char *name; uint32_t off, count, scx_off, scx_count; } KzUScript;")
out.append("static const KzUScript kz_scripts[%d] = {" % len(scripts))
for i, n in enumerate(scripts):
    out.append("    { %s, %d, %d, %d, %d }," % (c_string(n), script_off[i], script_cnt[i], scx_off[i], scx_cnt[i]))
out.append("};")
sn = sorted(script_names.items())
out.append("typedef struct KzUName { const char *name; uint32_t value; } KzUName;")
out.append("static const KzUName kz_script_names[%d] = {" % len(sn))
for n, v in sn:
    out.append("    { %s, %d }," % (c_string(n), v))
out.append("};")
gn = sorted(gc_names.items())
out.append("static const KzUName kz_gc_names[%d] = {" % len(gn))
for n, v in gn:
    out.append("    { %s, 0x%x }," % (c_string(n), v))
out.append("};")

out.append("typedef struct KzCaseRun { uint32_t start; uint16_t len; uint8_t step, pad; int32_t delta; } KzCaseRun;")
for name, rs in (("kz_upper_runs", upper_runs), ("kz_lower_runs", lower_runs), ("kz_fold_runs", fold_runs)):
    out.append("static const KzCaseRun %s[%d] = {" % (name, len(rs)))
    for c, n, step, d in rs:
        out.append("    { 0x%x, %d, %d, 0, %d }," % (c, n, step, d))
    out.append("};")
out.append("typedef struct KzSpecialCase { uint32_t cp; uint32_t n; uint32_t m[3]; } KzSpecialCase;")
for name, sp in (("kz_special_upper", special_upper), ("kz_special_lower", special_lower)):
    items = sorted(sp.items())
    out.append("static const KzSpecialCase %s[%d] = {" % (name, len(items)))
    for cp, m in items:
        mm = m + [0] * (3 - len(m))
        out.append("    { 0x%x, %d, { 0x%x, 0x%x, 0x%x } }," % (cp, len(m), mm[0], mm[1], mm[2]))
    out.append("};")

out.append(c_array("kz_ccc_ranges", "uint32_t", [(c << 8) | k for c, k in ccc_ranges], 10))
out.append("typedef struct KzDecompRun { uint32_t start; uint16_t len; uint8_t compat, pad; int32_t delta; } KzDecompRun;")
out.append("static const KzDecompRun kz_decomp_runs[%d] = {" % len(decomp_runs))
for c, n, compat, d in decomp_runs:
    out.append("    { 0x%x, %d, %d, 0, %d }," % (c, n, compat, d))
out.append("};")
out.append("typedef struct KzDecomp { uint32_t cp; uint8_t compat, len; uint16_t off; } KzDecomp;")
out.append("static const KzDecomp kz_decomp_multi[%d] = {" % len(multi_entries))
for cp, compat, n, off in multi_entries:
    out.append("    { 0x%x, %d, %d, %d }," % (cp, compat, n, off))
out.append("};")
out.append(c_array("kz_decomp_pool", "uint32_t", pool, 12))
out.append("typedef struct KzCompose { uint32_t first, second, composite; } KzCompose;")
out.append("static const KzCompose kz_compose[%d] = {" % len(compose))
for a, b, c in compose:
    out.append("    { 0x%x, 0x%x, 0x%x }," % (a, b, c))
out.append("};")

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out) + "\n")

size = (len(g) * 4 + len(stream) + sum(len(ranges_of(binary[n])) * 8 for n in ("ID_Start", "ID_Continue")) +
        (len(upper_runs) + len(lower_runs) + len(fold_runs)) * 12 + len(ccc_ranges) * 4 + len(decomp_runs) * 12 +
        len(multi_entries) * 8 + len(pool) * 4 + len(compose) * 12)
print("wrote %s: Unicode %s, about %d KiB of tables (gc %d ranges, stream %d bytes, %d scripts, %d decomposition runs, "
      "%d multi decompositions, %d composites)" % (OUT, version(), size // 1024, len(g), len(stream), len(scripts),
                                                    len(decomp_runs), len(multi_entries), len(compose)))
