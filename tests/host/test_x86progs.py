#!/usr/bin/env python3
"""Whole programs on the x86emu CPU core (user/lib/pc98): every tests/host/x86progs/*.asm is assembled with nasm (skipped
when nasm is missing), loaded at 1000:0100 with CS = DS = ES = SS = 1000h and SP = FFFEh, run until HLT, and checked
against the expectations written in its header:

    ; models: 8086 v30 286                    which CPU models to run it on
    ; expect: ax=00a8 dx=0001                 registers (any model)
    ; expect[286]: ax=0002                    registers on one model
    ; expect-mem: 1000:0400 5a 5a 54 6f       bytes at seg:off

    python3 -m unittest discover -s tests/host -p "test_x86*.py" -v
"""
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402

PROGS = Path(__file__).resolve().parent / "x86progs"
OUT = x86ref.OUT / "progs"
MODELS = {"8086": "8086", "v30": "v30", "286": "286"}


def parse(src):
    models, regs, mem = [], {}, []
    for line in src.read_text().splitlines():
        m = re.match(r";\s*models:\s*(.*)", line)
        if m:
            models = m.group(1).split()
        m = re.match(r";\s*expect(?:\[(\w+)\])?:\s*(.*)", line)
        if m:
            for kv in m.group(2).split():
                k, v = kv.split("=")
                regs.setdefault(m.group(1), {})[k] = int(v, 16)
        m = re.match(r";\s*expect-mem:\s*([0-9a-fA-F]+):([0-9a-fA-F]+)\s+(.*)", line)
        if m:
            mem.append((x86ref.lin(int(m.group(1), 16), int(m.group(2), 16)), bytes(int(b, 16) for b in m.group(3).split())))
    return models, regs, mem


def assemble(src):
    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / (src.stem + ".bin")
    subprocess.run(["nasm", "-f", "bin", "-o", str(out), str(src)], check=True)
    return out


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
@unittest.skipUnless(shutil.which("nasm"), "needs nasm")
class Programs(unittest.TestCase):
    pass


def make(src):
    def test(self):
        models, regs, mem = parse(src)
        self.assertTrue(models, "no '; models:' line")
        binary = assemble(src)
        for model in models:
            with self.subTest(model=model):
                lines = ["model " + MODELS[model],
                         "r 0 0 0 0 fffe 0 0 0 1000 1000 1000 1000 100 f002",
                         "load %x %s" % (x86ref.lin(0x1000, 0x100), binary),
                         "run 1000000"]
                lines += ["d %x %x" % (a, len(b)) for a, b in mem]
                res = x86ref.run_script(lines)[0]
                self.assertEqual(res.stop, 0, "CPU stopped (unimplemented instruction?)")
                self.assertEqual(res.halted, 1, "did not reach HLT")
                want = dict(regs.get(None, {}))
                want.update(regs.get(model, {}))
                for k, v in want.items():
                    self.assertEqual(res[k], v, "%s on %s: %s=%04x, want %04x" % (src.name, model, k, res[k], v))
                for (a, b), got in zip(mem, res.dumps):
                    self.assertEqual(got.hex(" "), b.hex(" "), "%s on %s: memory at %05x" % (src.name, model, a))
    return test


for _src in sorted(PROGS.glob("*.asm")):
    setattr(Programs, "test_" + _src.stem, make(_src))


if __name__ == "__main__":
    unittest.main()
