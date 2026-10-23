"""Helpers for the mcc tests (tests/case_mcc.py): building the host tools, assembling a system root for the
compiler, putting compiled programs into a private copy of the initrd, and running them in the guest.

Host tools: mcc is plain C; the host copy is built by tools/mcc_host.sh with the Linux gcc of WSL (the Windows clang here
has no hosted C library).  Everything the host mcc produces is a TouhouOS ELF file, so it only ever runs in QEMU.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
from qemu import Qemu, BUILD  # noqa: E402

HOST = BUILD / "host"
SYSROOT = BUILD / "mcc-root"
CORPUS = HERE / "mcc"
OUT = BUILD / "mcc-out"
PROMPT = r"reimu@gensokyo \S+ \$ "


def wsl_path(p):
    p = str(Path(p).resolve()).replace("\\", "/")
    m = re.match(r"^([A-Za-z]):(.*)$", p)
    return f"/mnt/{m.group(1).lower()}{m.group(2)}" if m else p


def _newest(paths):
    return max((Path(p).stat().st_mtime for p in paths), default=0)


def have_wsl():
    return shutil.which("wsl") is not None


def build_host(force=False):
    """Builds build/host/{mcc,mld,mar} (through WSL gcc) unless they are newer than the sources."""
    exe = HOST / "mcc"
    srcs = list((ROOT / "user" / "bin" / "mcc").glob("*.[ch]")) + list((ROOT / "user" / "bin" / "mcc").glob("*.inc"))
    if not force and exe.exists() and exe.stat().st_mtime >= _newest(srcs):
        return exe
    if not have_wsl():
        raise RuntimeError("the host build of mcc needs WSL with gcc (tools/mcc_host.sh)")
    r = subprocess.run(["wsl.exe", "-e", "bash", wsl_path(ROOT / "tools" / "mcc_host.sh")], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError("host build of mcc failed:\n" + r.stdout + r.stderr)
    return exe


def host(tool, args, cwd=ROOT, input=None, timeout=120):
    """Runs a host tool (mcc, mld, mar) through WSL.  Returns (returncode, stdout, stderr)."""
    env_root = wsl_path(SYSROOT)
    cmd = ["wsl.exe", "-e", "env", f"MCC_ROOT={env_root}", wsl_path(HOST / tool)] + [str(a) for a in args]
    r = subprocess.run(cmd, capture_output=True, cwd=cwd, input=input, timeout=timeout)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def make_sysroot():
    """build/mcc-root/{include,lib}: what the compiler looks for under $MCC_ROOT (the same tree goes into the initrd)."""
    sys.path.insert(0, str(ROOT / "tools"))
    import mccroot
    return mccroot.stage_sysroot(SYSROOT)


def make_initrd(extra, out):
    """Copies build/initrd.img and adds extra files: {archive name: bytes | Path}.  Executables get mode 0755."""
    src = BUILD / "initrd.img"
    with tarfile.open(src, "r") as tin, tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tout:
        names = set()
        for m in tin.getmembers():
            names.add(m.name)
            if m.isfile():
                tout.addfile(m, tin.extractfile(m))
            else:
                tout.addfile(m)
        for name, data in extra.items():
            if isinstance(data, Path):
                data = data.read_bytes()
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = 0o755
            ti.mtime = int(time.time())
            tout.addfile(ti, io.BytesIO(data))


def make_esp(name, extra):
    """A private copy of build/esp whose initrd also holds `extra`.  Returns the directory."""
    esp = BUILD / f"{name}-esp"
    (esp / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(BUILD / "esp" / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    shutil.copyfile(BUILD / "esp" / "kernel.elf", esp / "kernel.elf")
    make_initrd(extra, esp / "initrd.img")
    if not (esp / "cmdline.txt").exists():
        (esp / "cmdline.txt").write_text("\n")
    return esp


def run(vm, cmd, timeout=60, prompt=PROMPT):
    """Runs one shell command in the guest; returns (output, exit status).  See tests/test_shell.py."""
    vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
    m = vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
    vm.expect(prompt, timeout=30)
    return m.group(1).replace("\r", ""), int(m.group(2))
