"""mcc's inline assembler against gcc's: the privileged and system instructions the kernel's inline assembly uses (host test, WSL or Linux).

  python tools/hosttest.py discover -s tests/host -p "test_mcc_kasm.py" -v

Every case is one statement __asm__ volatile("...") with explicit registers (so that the two compilers have nothing to choose), compiled by mcc
and by gcc -O0; after the function prologue and epilogue are dropped, the machine instructions that remain must be the same bytes.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MCC = ROOT / "build" / "host" / "mcc"
GCC = shutil.which("gcc")

CASES = [
    "wbinvd", "invd", "rdmsr", "wrmsr", "rdtsc", "cpuid", "hlt", "cli", "sti", "pause", "mfence", "lfence", "sfence", "ud2", "swapgs", "clts",
    "mov %%cr0, %%rax", "mov %%cr2, %%rax", "mov %%cr3, %%rax", "mov %%cr4, %%rdx", "mov %%cr8, %%rax", "mov %%rax, %%cr0", "mov %%rax, %%cr3", "mov %%rcx, %%cr4", "mov %%rax, %%cr8",
    "movw $0x100, %%ax", "mov %%ax, %%ds", "mov %%ax, %%es", "mov %%ax, %%ss", "mov %%ax, %%fs", "mov %%ax, %%gs", "mov %%ds, %%ax", "mov %%cs, %%dx",
    "lgdt (%%rax)", "lidt (%%rax)", "lgdt 16(%%rbx)", "lidt -8(%%rbp)", "ltr %%ax", "invlpg (%%rdi)", "clflush (%%rax)", "fxsave (%%rax)", "fxrstor (%%rax)", "ldmxcsr (%%rax)", "stmxcsr (%%rax)",
    "rdrand %%rax", "rdrand %%eax", "rdrand %%r9",
    "outb %%al, %%dx", "outw %%ax, %%dx", "outl %%eax, %%dx", "inb %%dx, %%al", "inw %%dx, %%ax", "inl %%dx, %%eax",
    "pushfq", "popfq", "popq %%rax", "pushq %%rax", "pushq $0x23", "pushq $0x1000",
    "std; rep movsb; cld", "rep stosq", "rep stosb", "rep movsq", "lretq", "fninit", "iretq", "leaq 1f(%%rip), %%rax\\n1:",
]


def objdump_lines(obj):
    out = subprocess.run(["objdump", "-d", "-w", "--no-show-raw-insn", str(obj)], capture_output=True, text=True).stdout
    raw = subprocess.run(["objdump", "-d", "-w", str(obj)], capture_output=True, text=True).stdout
    keep = []
    for line in raw.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s*(\S.*)$", line)
        if not m:
            continue
        text = m.group(2).strip()
        if re.match(r"(endbr64|push\s+%rbp|mov\s+%rsp,%rbp|pop\s+%rbp|ret|leave|nop|sub\s+\$0x[0-9a-f]+,%rsp|add\s+\$0x[0-9a-f]+,%rsp|xchg\s+%ax,%ax|data16|cs nopw)", text):
            continue
        keep.append(m.group(1).strip())
    return keep


@unittest.skipUnless(MCC.exists() and GCC, "needs the host mcc (tools/mcc_host.sh) and gcc")
class KernelAsm(unittest.TestCase):
    def test_instructions(self):
        tmp = Path(tempfile.mkdtemp(prefix="mcc-kasm-"))
        try:
            bad = []
            for i, insn in enumerate(CASES):
                src = tmp / f"c{i}.c"
                src.write_text(f'void f(void) {{ __asm__ volatile ("{insn}" ::: "memory"); }}\n', newline="\n")
                a = subprocess.run([GCC, "-O0", "-fno-asynchronous-unwind-tables", "-fcf-protection=none", "-c", str(src), "-o", str(tmp / "g.o")], capture_output=True, text=True)
                if a.returncode:
                    bad.append(f"{insn!r}: gcc says {a.stderr.strip()[:200]}")
                    continue
                import os
                env = dict(os.environ, MCC_ROOT=str(ROOT / "build" / "mcc-root"))
                m = subprocess.run([str(MCC), "-c", str(src), "-o", str(tmp / "m.o")], capture_output=True, text=True, env=env)
                if m.returncode:
                    bad.append(f"{insn!r}: mcc says {m.stderr.strip().splitlines()[0][:200]}")
                    continue
                g, mm = objdump_lines(tmp / "g.o"), objdump_lines(tmp / "m.o")
                if g != mm:
                    bad.append(f"{insn!r}: gcc {g} / mcc {mm}")
            self.assertFalse(bad, "\n".join(bad))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
