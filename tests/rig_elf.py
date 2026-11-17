"""Tiny native TouhouOS programs for the tests of tools/run-in-guest.py and the runstatus applet (docs/RUN_IN_GUEST.md).

Each program is a static ELF64 executable made here from a few hand-assembled instructions: one PT_LOAD segment (readable, writable, executable) at 0x400000 that holds the ELF
header, the program header, the code and the data, with some zeroed memory after them for a buffer. The native system calls are `syscall` with the number in rax and the
arguments in rdi, rsi, rdx (shared/syscall.h: exit 0, read 1, write 2); a native program starts with rdi = argc and rsi = argv. The encodings were checked with ndisasm.

    from rig_elf import PROGRAMS        # name -> bytes of the executable
    python tests/rig_elf.py DIR         # writes them all into DIR
"""
import struct

BASE = 0x400000
HEAD = 64 + 56                          # ELF header + one program header; the code starts here
BSS = 16384


def elf(code_for, data=b"", bss=BSS):
    """code_for(data_addr, bss_addr) -> machine code (its length must not depend on the addresses). Returns the file."""
    probe = code_for(0, 0)
    data_addr = BASE + HEAD + len(probe)
    bss_addr = data_addr + len(data)
    code = code_for(data_addr, bss_addr)
    assert len(code) == len(probe)
    body = code + data
    size = HEAD + len(body)
    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, BASE + HEAD, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 7, 0, BASE, BASE, size, size + bss, 0x1000)
    return ehdr + phdr + body


def imm(op, v):
    return bytes([op]) + struct.pack("<I", v)


def mov_eax(v): return imm(0xB8, v)
def mov_edi(v): return imm(0xBF, v)
def mov_esi(v): return imm(0xBE, v)
def mov_edx(v): return imm(0xBA, v)


def jcc(op, skipped):
    """a short conditional jump over `skipped` (the bytes between the jump and its target)"""
    assert len(skipped) < 128
    return bytes([op, len(skipped)])


def jle(skipped): return jcc(0x7E, skipped)
def jge(skipped): return jcc(0x7D, skipped)
def je(skipped): return jcc(0x74, skipped)


def jmp_back(body):
    """a short jump back to the start of `body`: the bytes from the target up to just before this jump"""
    assert len(body) + 2 < 128
    return bytes([0xEB, (-(len(body) + 2)) & 0xFF])


SYSCALL = bytes([0x0F, 0x05])
XOR_EAX = bytes([0x31, 0xC0])
XOR_EDI = bytes([0x31, 0xFF])
XOR_EDX = bytes([0x31, 0xD2])
XOR_ECX = bytes([0x31, 0xC9])
DIV_ECX = bytes([0xF7, 0xF1])
UD2 = bytes([0x0F, 0x0B])
JMP_SELF = bytes([0xEB, 0xFE])
MOV_EAX_FROM_0 = bytes([0x8B, 0x04, 0x25, 0, 0, 0, 0])             # mov eax,[0]
TEST_RAX_RAX = bytes([0x48, 0x85, 0xC0])
MOV_RDX_RAX = bytes([0x48, 0x89, 0xC2])


def write_(fd, addr, n):
    return mov_eax(2) + mov_edi(fd) + mov_esi(addr) + mov_edx(n) + SYSCALL


def exit_(code):
    return mov_edi(code) + XOR_EAX + SYSCALL


def exit_with(code):
    """exit(code)"""
    return elf(lambda d, b: exit_(code))


def exit_argc():
    """exit(argc): the status is the number of words in the argument vector, argv[0] included (rdi holds argc when the program starts)"""
    return elf(lambda d, b: XOR_EAX + SYSCALL)


def three_lines():
    """standard output, standard error, standard output again: the order in which they come back shows that both go to one place"""
    data = b"out1\nerr\nout2\n"
    return elf(lambda d, b: write_(1, d, 5) + write_(2, d + 5, 4) + write_(1, d + 9, 5) + exit_(0), data)


def fault(first, insns):
    """writes `first` and then runs the instructions that fault"""
    return elf(lambda d, b: write_(1, d, len(first)) + insns, first)


def divide_by_zero():
    return fault(b"before the fault", XOR_EDX + XOR_ECX + mov_eax(1) + DIV_ECX)     # #DE -> SIGFPE


def null_read():
    return fault(b"partial", MOV_EAX_FROM_0)                                        # #PF -> SIGSEGV


def undefined_instruction():
    return fault(b"about to fail\n", UD2)                                           # #UD -> SIGILL


def spin():
    return fault(b"started\n", JMP_SELF)


def flood():
    """writes 4096 bytes of itself for ever"""
    body = mov_eax(2) + mov_edi(1) + mov_esi(BASE) + mov_edx(4096) + SYSCALL
    return elf(lambda d, b: body + jmp_back(body))


def cat():
    """copies standard input to standard output until the end of the file, then exits 0"""
    def code(d, b):
        read = mov_eax(1) + XOR_EDI + mov_esi(b) + mov_edx(4096) + SYSCALL + TEST_RAX_RAX      # n = read(0, buf, 4096)
        write = MOV_RDX_RAX + mov_eax(2) + mov_edi(1) + SYSCALL                                # write(1, buf, n)   (esi still holds buf)
        back = jmp_back(read + jle(b"") + write)
        return read + jle(write + back) + write + back + exit_(0)
    return elf(code)


def argv_lines():
    """prints argv[1], argv[2], ... one to a line (argv[0] is left out)"""
    def code(d, b):
        setup = bytes([0x49, 0x89, 0xFC, 0x49, 0x89, 0xF5, 0x41, 0xBE, 1, 0, 0, 0])          # mov r12,rdi ; mov r13,rsi ; mov r14d,1
        head = bytes([0x4D, 0x39, 0xE6])                                                      # next: cmp r14,r12
        load = bytes([0x4B, 0x8B, 0x74, 0xF5, 0x00]) + XOR_EDX                                # mov rsi,[r13+r14*8] ; xor edx,edx
        scan = bytes([0x80, 0x3C, 0x16, 0x00])                                                # len: cmp byte [rsi+rdx],0
        inc = bytes([0x48, 0xFF, 0xC2])                                                       # inc rdx
        strlen = scan + je(inc + jmp_back(b"")) + inc + jmp_back(scan + je(inc + jmp_back(b"")) + inc)
        emit = mov_eax(2) + mov_edi(1) + SYSCALL + write_(1, d, 1) + bytes([0x49, 0xFF, 0xC6])   # w: write(1, rsi, rdx) ; write(1, "\n", 1) ; inc r14
        loop = load + strlen + emit
        back = jmp_back(head + jge(b"") + loop)
        return setup + head + jge(loop + back) + loop + back + exit_(0)
    return elf(code, b"\n")


def big_bss():
    """A .bss of 2 GiB, more than the RAM of the test guests: the loader must leave it demand-zero. The program makes the kernel read 8 bytes of a page of untouched .bss (write: they are
    zeros), touches one byte a gigabyte further on, makes the kernel read that back, and exits 0."""
    def code(d, b):
        far = b + (1 << 30)
        return (write_(1, b + 0x1000, 8) +                                           # a page that holds no file data: demand-zero
                bytes([0x48, 0xB8]) + struct.pack("<Q", far) +                       # mov rax, far
                bytes([0xC6, 0x00, 0x41]) +                                          # mov byte [rax], 'A'
                bytes([0x48, 0x89, 0xC6]) + mov_edi(1) + mov_edx(1) + mov_eax(2) + SYSCALL +   # write(1, rax, 1)
                exit_(0))
    return elf(code, bss=2 << 30)


def programs():
    return {
        "big_bss": big_bss(),
        "exit_7": exit_with(7), "exit_0": exit_with(0), "exit_200": exit_with(200),
        "exit_argc": exit_argc(), "three_lines": three_lines(), "divide_by_zero": divide_by_zero(), "null_read": null_read(),
        "undefined_instruction": undefined_instruction(), "spin": spin(), "flood": flood(), "cat": cat(), "argv_lines": argv_lines(),
    }


PROGRAMS = programs()


if __name__ == "__main__":
    import sys
    from pathlib import Path
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    for name, blob in PROGRAMS.items():
        (out / name).write_bytes(blob)
        print(f"{out / name}  {len(blob)} bytes")
