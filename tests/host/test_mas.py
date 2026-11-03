"""mas against NASM (host test, Linux or WSL: needs gcc, NASM and binutils).

  python tools/hosttest.py discover -s tests/host -p "test_mas.py" -v

The nine .asm files of the kernel and the C library, and the small files of tests/mas/ that cover the instruction forms and directives
the encoder knows, are assembled by NASM (`nasm -f elf64`) and by mas; the two object files must be the same bytes.  When they are not, the
failure shows the difference in readable form (readelf for the headers, symbols and relocations, objdump for the contents).
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC_DIR = ROOT / "user" / "bin" / "mas"
OUT = ROOT / "build" / "host-mas"
MAS = OUT / "mas"
CC = shutil.which("gcc") or shutil.which("cc")


def find_nasm():
    env = os.environ.get("TOUHOU_NASM")
    if env and Path(env).exists():
        return env
    return shutil.which("nasm")


NASM = find_nasm()

PROJECT_FILES = [
    "user/libc/crt0.asm", "user/libc/src/setjmp.asm", "kernel/arch/x86_64/ksyms.asm", "kernel/arch/x86_64/efi_call.asm", "kernel/arch/x86_64/entry.asm",
    "kernel/arch/x86_64/uaccess.asm", "kernel/arch/x86_64/switch.asm", "kernel/arch/x86_64/syscall.asm", "kernel/arch/x86_64/isr.asm",
]


def build_mas():
    OUT.mkdir(parents=True, exist_ok=True)
    srcs = list(SRC_DIR.glob("*.c")) + list(SRC_DIR.glob("*.inc"))
    if MAS.exists() and all(MAS.stat().st_mtime >= s.stat().st_mtime for s in srcs):
        return
    r = subprocess.run([CC, "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-Wno-unused-function", "-o", str(MAS), str(SRC_DIR / "mas.c")], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("building mas failed:\n" + r.stdout + r.stderr)


def tool_text(args):
    r = subprocess.run(args, capture_output=True, text=True)
    return r.stdout + r.stderr


def describe(a, b):
    """A readable difference of two object files."""
    import difflib
    parts = []
    for title, cmd in (("readelf -S -s -r", ["readelf", "-W", "-S", "-s", "-r"]), ("objdump -s", ["objdump", "-s"]), ("objdump -d", ["objdump", "-d", "-r"])):
        ta, tb = tool_text(cmd + [str(a)]), tool_text(cmd + [str(b)])
        ta, tb = ta.replace(str(a), "OBJ"), tb.replace(str(b), "OBJ")
        d = list(difflib.unified_diff(ta.splitlines(), tb.splitlines(), "nasm", "mas", lineterm="", n=2))
        if d:
            parts.append(f"=== {title}\n" + "\n".join(d[:60]))
    return "\n".join(parts) or "(the readelf and objdump views are equal; the bytes differ in padding or ordering)"


def assemble(tool_cmd, src, out, incdirs, cwd):
    cmd = [*tool_cmd, "-f", "elf64", *sum((["-I", d] for d in incdirs), []), str(src), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)


@unittest.skipUnless(NASM and CC and shutil.which("readelf"), "needs gcc, nasm and binutils")
class MasVersusNasm(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_mas()
        cls.tmp = Path(tempfile.mkdtemp(prefix="mas-test-"))
        (cls.tmp / "gen").mkdir()
        (cls.tmp / "gen" / "ksyms.bin").write_bytes(bytes(range(256)) * 3 + b"\x01\x02\x03")
        (cls.tmp / "gen" / "mas-blob.bin").write_bytes(b"blob bytes\x00\x01\x02 for incbin")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def same(self, rel, incdirs=()):
        src = str(rel)
        incs = [str(self.tmp / "gen") + "/", str(ROOT / "kernel" / "include") + "/", str(ROOT / "tests" / "mas" / "inc") + "/", *incdirs]
        n = assemble([NASM], src, self.tmp / "nasm.o", incs, ROOT)
        self.assertEqual(n.returncode, 0, f"nasm failed on {src}:\n{n.stdout}{n.stderr}")
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        cmd = [str(MAS), "-f", "elf64", *sum((["-I", d] for d in incs), []), src, "-o", str(self.tmp / "mas.o")]
        m = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, env=env)
        self.assertEqual(m.returncode, 0, f"mas failed on {src}:\n{m.stdout}{m.stderr}")
        a, b = (self.tmp / "nasm.o").read_bytes(), (self.tmp / "mas.o").read_bytes()
        if a != b:
            self.fail(f"{src}: the object files differ ({len(a)} bytes from nasm, {len(b)} from mas)\n" + describe(self.tmp / "nasm.o", self.tmp / "mas.o"))

    def test_project_files(self):
        for f in PROJECT_FILES:
            with self.subTest(f):
                self.same(f)

    def test_corpus(self):
        files = sorted((ROOT / "tests" / "mas").glob("*.asm"))
        self.assertTrue(files, "tests/mas has no corpus")
        for f in files:
            with self.subTest(f.name):
                self.same(f.relative_to(ROOT))

    def test_errors_are_reported_with_the_line(self):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        cases = [
            ("bits 64\nsection .text\n    frobnicate rax\n", r"3: error"),
            ("bits 64\nsection .text\n    mov rax, [rbx + undefined_thing]\n", r"undefined_thing"),
            ("bits 64\nsection .text\nfoo: nop\nfoo: nop\n", r"defined twice|redefin"),
            ("bits 64\nsection .text\n    mov eax, rbx\n", r"mismatch"),
            ("bits 64\n%macro M 1\n nop\n%endmacro\nsection .text\n M\n", r"expects 1"),
            ("bits 64\nsection .text\n    jmp short far_away\n    times 200 nop\nfar_away: nop\n", r"out of range"),
        ]
        for text, pattern in cases:
            with self.subTest(text.splitlines()[-1]):
                p = self.tmp / "bad.asm"
                p.write_text(text)
                r = subprocess.run([str(MAS), "-f", "elf64", str(p), "-o", str(self.tmp / "bad.o")], capture_output=True, text=True, env=env)
                self.assertNotEqual(r.returncode, 0, "mas accepted: " + text)
                self.assertRegex(r.stderr, pattern)
                self.assertNotIn("Sanitizer", r.stderr)

    def test_dependency_file(self):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        r = subprocess.run([str(MAS), "-f", "elf64", "-I", str(self.tmp / "gen") + "/", "-MD", str(self.tmp / "k.d"), "kernel/arch/x86_64/ksyms.asm", "-o",
                            str(self.tmp / "k.o")], capture_output=True, text=True, cwd=ROOT, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = (self.tmp / "k.d").read_text()
        self.assertIn("ksyms.bin", text)
        self.assertTrue(text.startswith(str(self.tmp / "k.o") + ": kernel/arch/x86_64/ksyms.asm"), text)


if __name__ == "__main__":
    unittest.main()
