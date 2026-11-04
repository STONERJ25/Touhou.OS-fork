#!/usr/bin/env python3
"""hwreport.py -- reads the hardware reports that TouhouOS's `hwreport` command writes (docs/HARDWARE_DB.md) and tells what is in them.

  python tools/hwreport.py report.txt                  one line per report, ready for the table in docs/HARDWARE_DB.md
  python tools/hwreport.py reports/                    every *.txt in a folder; with two or more reports the table of PCI devices
                                                       that have no driver follows, most common first: this orders the driver backlog
  python tools/hwreport.py --drivers a.txt             the driver table for one report as well
  python tools/hwreport.py --all reports/              the driver table without hiding bridges and other devices that never need a driver
  python tools/hwreport.py --check reports/            only check that each file is a whole report and that nothing private is in it

A report is plain text: a first line `format: hwreport 1`, then sections `[name]` holding `key: value` lines (a key may repeat), and a last line
`end: hwreport 1`. This module is the one reader of that format (tests/host/test_hwreport.py uses it); `parse()` returns a Report whose
`.section(name)` is a list of (key, value) pairs in file order.
"""
import argparse
import re
import sys
from collections import OrderedDict
from pathlib import Path

FORMAT = 1
SECTIONS = ["system", "cpu", "memory", "firmware", "acpi", "pci", "storage", "usb", "network", "audio", "input", "interrupts", "sensors", "log", "cmdline", "quirks"]

# PCI classes (class and subclass) whose functions this OS never needs a driver for: listed only with --all.
INFRASTRUCTURE = {0x0600, 0x0601, 0x0604, 0x0680, 0x0c05}


class ReportError(Exception):
    pass


class Report:
    def __init__(self, name=""):
        self.name = name
        self.head = OrderedDict()                       # format, build, date, redaction (and anything else before the first section)
        self.sections = OrderedDict()                   # name -> [(key, value)]
        self.ended = False

    def section(self, name):
        return self.sections.get(name, [])

    def get(self, section, key, default=""):
        for k, v in self.section(section):
            if k == key:
                return v
        return default

    def all(self, section, key):
        return [v for k, v in self.section(section) if k == key]

    @property
    def redacted(self):
        return self.head.get("redaction", "").startswith("on")


def parse(text, name=""):
    """Reads one report. Tolerates a byte order mark, CRLF line ends, comment lines (#), sections and keys it does not know."""
    r = Report(name)
    text = text.lstrip("﻿").replace("\r\n", "\n").replace("\r", "\n")
    cur = None
    for raw in text.split("\n"):
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"\[([A-Za-z0-9_ -]+)\]", line)
        if m:
            cur = r.sections.setdefault(m.group(1), [])
            continue
        key, sep, value = line.partition(": ")
        if not sep:
            key, sep, value = line.partition(":")
            if not sep:
                key, value = line, ""
        if key == "end" and value.startswith("hwreport"):
            r.ended = True
            continue
        if cur is not None:
            cur.append((key, value))
        else:
            r.head[key] = value
    fmt = r.head.get("format", "")
    if not fmt.startswith("hwreport"):
        raise ReportError("not a hwreport file (the first line should be 'format: hwreport N')")
    try:
        r.format = int(fmt.split()[1])
    except (IndexError, ValueError):
        raise ReportError(f"unreadable format line {fmt!r}")
    return r


def load(path):
    p = Path(path)
    return parse(p.read_text(encoding="utf-8", errors="replace"), p.stem)


def expand(paths):
    out = []
    for p in map(Path, paths):
        out += sorted(p.glob("*.txt")) if p.is_dir() else [p]
    return out


# ---------------------------------------------------------------------------------------------------- PCI

class PciFn:
    def __init__(self, bdf, value):
        self.bdf = bdf
        m = re.match(r"([0-9a-f]{4}):([0-9a-f]{4}) class=([0-9a-f]{2})([0-9a-f]{2}):([0-9a-f]{2}) driver=(\S+) msi=(\w+) msix=(\w+) pcie=(\w+) link=(\S+) max=(\S+) bars=(\S+) name=(.*)", value)
        if not m:
            raise ReportError(f"unreadable PCI line {bdf}: {value!r}")
        self.vendor, self.device = m.group(1), m.group(2)
        self.cls, self.sub, self.pif = int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
        self.driver = m.group(6)
        self.msi, self.msix, self.pcie = m.group(7) == "yes", m.group(8) == "yes", m.group(9) == "yes"
        self.link, self.max_link = m.group(10), m.group(11)
        self.bars = m.group(12)
        self.name = m.group(13)

    @property
    def id(self):
        return f"{self.vendor}:{self.device}"

    @property
    def classcode(self):
        return self.cls << 8 | self.sub


def pci_functions(r):
    out = []
    for k, v in r.section("pci"):
        if k.startswith("pci."):
            out.append(PciFn(k[4:], v))
    return out


def driver_table(reports, show_all=False):
    """[(count, id, classcode, name, [machine names])] for PCI functions without a driver, most common first. `count` is the number of reports in
    which the id appears at least once without a driver."""
    seen = {}
    for r in reports:
        for fn in pci_functions(r):
            if fn.driver != "none" or (not show_all and fn.classcode in INFRASTRUCTURE):
                continue
            e = seen.setdefault((fn.id, fn.classcode), {"name": fn.name, "machines": []})
            if r.name not in e["machines"]:
                e["machines"].append(r.name)
    rows = [(len(e["machines"]), ident, cc, e["name"], e["machines"]) for (ident, cc), e in seen.items()]
    rows.sort(key=lambda t: (-t[0], t[1]))
    return rows


def format_driver_table(rows):
    if not rows:
        return "no PCI function without a driver"
    lines = [f"{'reports':>7}  {'id':<9}  {'class':<5}  {'name':<34}  machines"]
    for count, ident, cc, name, machines in rows:
        who = ", ".join(machines[:4]) + (f", +{len(machines) - 4}" if len(machines) > 4 else "")
        lines.append(f"{count:>7}  {ident:<9}  {cc:04x}   {name[:34]:<34}  {who}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------- the one-line summary

def _short_cpu(brand):
    b = re.sub(r"\((?:R|TM|tm|r)\)", "", brand)
    b = re.sub(r"\b(CPU|Processor|Core2|with Radeon.*|w/.*)\b", "", b)
    b = re.sub(r"@\s*[\d.]+\s*[GM]Hz", lambda m: m.group(0), b)
    return re.sub(r"\s+", " ", b).strip() or "?"


def summary(r):
    """The columns of the compatibility table in docs/HARDWARE_DB.md as a dict. What a report can know is filled in; what only a person can
    judge (shutdown, notes) is left as '?'."""
    manu = r.get("system", "system.manufacturer") or r.get("system", "baseboard.manufacturer")
    prod = r.get("system", "system.product") or r.get("system", "baseboard.product")
    machine = (f"{manu} {prod}".strip() if (manu or prod) else "(no SMBIOS)")
    cpu = _short_cpu(r.get("cpu", "brand", "?"))
    fam = r.get("cpu", "microarchitecture").split(" (")[0]
    if fam and fam != "unknown":
        cpu += f" ({fam})"
    fw = " ".join(x for x in (r.get("system", "bios.vendor"), r.get("system", "bios.version")) if x)
    date = r.get("system", "bios.date")
    uefi = r.get("firmware", "firmware.uefi")
    firmware = (fw or "?") + (f" {date}" if date else "") + (f", UEFI {uefi}" if uefi else "")
    gop = re.match(r"(\d+x\d+)", r.get("firmware", "gop.in_use"))
    display = f"{gop.group(1)} (GOP)" if gop else "none"
    kb_usb = re.match(r"(\d+) usb, (\d+) ps/2", r.get("input", "keyboards"))
    mouse = re.match(r"(\d+) usb, (\d+) ps/2", r.get("input", "mice"))

    def kinds(m):
        if not m:
            return "?"
        parts = [n for n, c in (("usb", m.group(1)), ("ps/2", m.group(2))) if c != "0"]
        return "+".join(parts) if parts else "none"

    pci = pci_functions(r)
    stor = []
    for k, v in r.section("storage"):
        if k.startswith("controller."):
            m = re.match(r"\S+ \S+ (.*?) driver=(\S+)$", v)
            if m:
                stor.append(f"{m.group(1).split(' (')[0].split(':')[0]}" + ("" if m.group(2) != "none" else " (no driver)"))
    ndisks = r.get("storage", "disks", "0")
    storage = (", ".join(stor) if stor else "none") + f", {ndisks} disk" + ("" if ndisks == "1" else "s")
    if any("raid_hint" == k for k, _ in r.section("storage")):
        storage += ", RAID mode hides drives"
    nets = [v for k, v in r.section("network") if k.startswith("interface.")]
    if nets:
        parts = []
        for v in nets:
            m = re.match(r"\S+ driver=(\S+) .* link=(\w+(?: \w+)?) speed=(.*?) mtu", v)
            if m:
                parts.append(f"{m.group(1)} {m.group(2)}")
        network = ", ".join(parts) if parts else "?"
    else:
        nic = [f for f in pci if f.cls == 0x02]
        network = "no driver for " + ", ".join(f.id for f in nic) if nic else "none"
    audio_pci = [f for f in pci if f.cls == 0x04 and f.sub in (1, 3)]
    codec = r.get("audio", "codec_id")
    if audio_pci:
        audio = ", ".join(f"{f.driver} ({codec})" if f.driver != "none" and codec else (f.driver if f.driver != "none" else f"no driver for {f.id}") for f in audio_pci)
    else:
        audio = "none"
    return OrderedDict([
        ("Machine", machine), ("CPU", cpu), ("Firmware", firmware), ("Boot", "ok"), ("Display", display), ("Keyboard", kinds(kb_usb)), ("Mouse", kinds(mouse)),
        ("Storage", storage), ("Network", network), ("Audio", audio), ("Shutdown", "?"), ("Notes", ""), ("Build", _build(r)), ("Date", r.head.get("date", "?")[:10]),
    ])


def _build(r):
    b = r.head.get("build", "")
    m = re.search(r"commit (\w{7,40})", b)
    if m:
        return m.group(1)[:10]
    m = re.search(r"compiled (\w{3} +\d+ \d{4})", b)
    return ("built " + re.sub(r"\s+", " ", m.group(1))) if m else "?"


COLUMNS = ["Machine", "CPU", "Firmware", "Boot", "Display", "Keyboard", "Mouse", "Storage", "Network", "Audio", "Shutdown", "Notes", "Build", "Date"]


def table_row(s):
    return "| " + " | ".join(str(s[c]).replace("|", "/") for c in COLUMNS) + " |"


def table_header():
    return "| " + " | ".join(COLUMNS) + " |\n|" + "---|" * len(COLUMNS)


# ---------------------------------------------------------------------------------------------------- checking

MAC = re.compile(r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])")
UUID = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}(?![0-9A-Fa-f])")


def check(r, text=None):
    """Problems found in a report, as a list of strings (empty: fine). A whole report has every section and an end line; one that says its
    redaction is on must hold no full MAC address, no UUID and no value after a key that names a serial number or an asset tag."""
    problems = []
    if r.format != FORMAT:
        problems.append(f"format {r.format}, this tool reads format {FORMAT}")
    if not r.ended:
        problems.append("no 'end: hwreport' line: the file is cut short")
    for s in SECTIONS:
        if s not in r.sections:
            problems.append(f"section [{s}] is missing")
    for need in ("build", "date", "redaction"):
        if need not in r.head:
            problems.append(f"no '{need}:' line in the header")
    if r.redacted:
        for sec, pairs in r.sections.items():
            for k, v in pairs:
                if re.search(r"(serial|asset)$", k) and v and v not in ("<redacted>",) and not re.fullmatch(r"[0xX]+|n/?a|none|unknown|default string|to be filled by o\.e\.m\.|not specified", v, re.I):
                    problems.append(f"[{sec}] {k}: {v!r} is not redacted")
                for mac in MAC.findall(v):
                    if not re.search(r"xx:xx:xx$", mac, re.I) and not mac.lower().endswith(":xx:xx:xx"):
                        problems.append(f"[{sec}] {k}: full MAC address {mac}")
                if UUID.search(v):
                    problems.append(f"[{sec}] {k}: a UUID")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="report files, or folders holding *.txt reports")
    ap.add_argument("--drivers", action="store_true", help="also print the table of PCI devices without a driver (automatic with two or more reports)")
    ap.add_argument("--all", action="store_true", help="in that table, also bridges and other devices that never need a driver")
    ap.add_argument("--check", action="store_true", help="only check that each report is whole and holds nothing private; exit status 1 if not")
    ap.add_argument("--header", action="store_true", help="print the table header before the rows")
    a = ap.parse_args(argv)
    files = expand(a.paths)
    if not files:
        print("no report files found", file=sys.stderr)
        return 2
    reports, bad = [], 0
    for f in files:
        try:
            r = load(f)
        except (ReportError, OSError) as e:
            print(f"{f}: {e}", file=sys.stderr)
            bad += 1
            continue
        reports.append(r)
        if a.check:
            for p in check(r):
                print(f"{f}: {p}")
                bad += 1
    if a.check:
        if not bad:
            print(f"{len(reports)} report(s) checked: fine")
        return 1 if bad else 0
    if a.header:
        print(table_header())
    for r in reports:
        print(table_row(summary(r)))
    if len(reports) > 1 or a.drivers:
        print()
        print(format_driver_table(driver_table(reports, a.all)))
    return 1 if bad else 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    sys.exit(main())
