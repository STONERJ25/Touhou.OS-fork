"""Helpers for the mcc tests (tests/case_mcc.py): building the host tools, assembling a system root for the
compiler, putting compiled programs into a private copy of the initrd, and running them in the guest.

Host tools: mcc is plain C; the host copy is built by tools/mcc_host.sh with a Linux gcc: through WSL on Windows (the
Windows clang here has no hosted C library), directly on Linux.  Everything the host mcc produces is a TouhouOS ELF file, so it
only ever runs in QEMU.
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


WINDOWS = os.name == "nt"


def linux_cmd(args):
    """A command line for the Linux side: through WSL on Windows, as it is on Linux."""
    return ["wsl.exe", "-e", *[str(a) for a in args]] if WINDOWS else [str(a) for a in args]


def have_wsl():
    """Whether the Linux side (WSL on Windows, the host itself on Linux) has what tools/mcc_host.sh needs."""
    return shutil.which("wsl") is not None if WINDOWS else shutil.which("gcc") is not None


def llvm_tool(name):
    """An LLVM program for the host-side checks: TOUHOU_LLVM_DIR, then the PATH, then the Windows install folder (as tools/build.py)."""
    exe = name + (".exe" if WINDOWS else "")
    env = os.environ.get("TOUHOU_LLVM_DIR")
    if env and (Path(env) / exe).is_file():
        return str(Path(env) / exe)
    found = shutil.which(name)
    if found:
        return found
    return str(Path(r"C:\Program Files\LLVM\bin") / exe)


def build_host(force=False):
    """Builds build/host/{mcc,mld,mar} (through WSL gcc) unless they are newer than the sources."""
    exe = HOST / "mcc"
    srcs = list((ROOT / "user" / "bin" / "mcc").glob("*.[ch]")) + list((ROOT / "user" / "bin" / "mcc").glob("*.inc"))
    if not force and exe.exists() and exe.stat().st_mtime >= _newest(srcs):
        return exe
    if not have_wsl():
        raise RuntimeError("the host build of mcc needs gcc (through WSL on Windows): tools/mcc_host.sh")
    r = subprocess.run(linux_cmd(["bash", wsl_path(ROOT / "tools" / "mcc_host.sh")]), capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        raise RuntimeError("host build of mcc failed:\n" + r.stdout + r.stderr)
    return exe


def host(tool, args, cwd=ROOT, input=None, timeout=120):
    """Runs a host tool (mcc, mld, mar), through WSL on Windows.  Returns (returncode, stdout, stderr)."""
    env_root = wsl_path(SYSROOT)
    cmd = linux_cmd(["env", f"MCC_ROOT={env_root}", wsl_path(HOST / tool)] + [str(a) for a in args])
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
        modes = {}
        for m in tin.getmembers():
            if m.name in extra:
                modes[m.name] = m.mode      # replaced by the file we are adding, which keeps the mode (setuid bits...)
                continue
            if m.isfile():
                tout.addfile(m, tin.extractfile(m))
            else:
                tout.addfile(m)
        for name, data in extra.items():
            if isinstance(data, Path):
                data = data.read_bytes()
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = modes.get(name, 0o755)
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
