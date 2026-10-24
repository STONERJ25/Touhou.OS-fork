"""Power cuts in the middle of writing: does Keine's ledger survive? (docs/STORAGE.md, "Write ordering" and "Crash test")

Each run builds a fresh disk with a HUMANVILLAGE volume, boots, starts a write-heavy random workload in the guest
(`fstest -churn SEED DIR`: create, append, overwrite, truncate, rename - also over an existing file and whole directories -,
delete, mkdir/rmdir, bursts of small files, one big file), pulls the plug at a random moment (QMP `quit`: no sync, no
poweroff, the kernel gets no warning) and then looks at the image with the host checker of tools/mkdisk.py, which shares
no code with the kernel. Allowed afterwards is only what a power cut may leave behind on a FAT volume without a journal:
lost clusters, a chain longer than its file's size, a FAT copy 2 that lags behind copy 1, a stale FSInfo free count and
the dirty flags. Anything else - a cross-link, a chain that runs into a free cluster, a size the chain cannot hold, a broken
directory, a long name without its short entry - fails the run, and so does a file that shows a byte it never wrote (every
byte of file k<id> is either 0 or churn_byte(id, offset), see user/bin/fstest.c). The kernel's own consistency check
(shared/fatck.c, run on the host through tools/fatck_host.py) must find no damage either: then the next boot would mount
the volume read-write.

  TOUHOU_CRASH_RUNS=50 python tests/run_all.py storage_crash      the full run (the default is 3 runs)
  TOUHOU_CRASH_SEED=N                                              the seed of the first run (default: from the clock)

A failing run names its seed; TOUHOU_CRASH_SEED=<seed> TOUHOU_CRASH_RUNS=1 repeats its workload, geometry and kill point
(the exact moment differs a little, the guest's speed is not under our control). The kill moment is a random number of
operations plus a random host-side delay of up to a second: the delay is the point of the test, not a wait for the guest.
"""
import os
import random
import re
import time

import numpy as np

from harness import test, scaled
import fatck_host
import mkdisk
from case_storage import Image, MIB, H, boot, hv_part, serious_errors

CHURN_DIR = "/reimu/churn"          # on the volume (H is /HumanVillage/reimu in the guest)


def churn_bytes(ident, n):
    """numpy twin of churn_byte() in user/bin/fstest.c for offsets 0..n-1."""
    off = np.arange(n, dtype=np.uint32)
    with np.errstate(over="ignore"):
        x = off * np.uint32(2654435761) + np.uint32((ident * 0x9E3779B9) & 0xFFFFFFFF)
        x ^= x >> np.uint32(15)
        x *= np.uint32(0x85EBCA6B)
        x ^= x >> np.uint32(13)
    return (x & np.uint32(0xFF)).astype(np.uint8)


def stale_bytes(vol, top=CHURN_DIR):
    """Files below `top` that hold a byte that is neither 0 nor theirs: [(path, offset of the first such byte)]."""
    bad, tree = [], vol._tree_cache()
    for ents in tree.values():
        for e in ents:
            if e.is_dir or not e.path.lower().startswith(top.lower() + "/"):
                continue
            m = re.match(r"[kK](\d+)", e.name)
            if not m:
                bad.append((e.path, "a name the workload never makes"))
                continue
            try:
                data = np.frombuffer(vol.read_file(e.path), dtype=np.uint8)
            except mkdisk.MkdiskError as ex:
                bad.append((e.path, f"unreadable: {ex}"))
                continue
            if not len(data):
                continue
            wrong = (data != 0) & (data != churn_bytes(int(m.group(1)), len(data)))
            if wrong.any():
                bad.append((e.path, int(np.argmax(wrong))))
    return bad


def crash_selftest_images():
    """Sanity check of serious_errors()/stale_bytes() on images damaged by hand: (what, damage function, expect serious?)."""
    def lost(v, files):
        v.set_fat(v.nclusters + 1, 0x0FFFFFFF)

    def chain_longer(v, files):
        e = v.find(files[0])
        chain = v.chain(e.first)
        v.set_fat(chain[-1], v.nclusters)                 # one more cluster after the end ...
        v.set_fat(v.nclusters, 0x0FFFFFFF)                # ... which ends the chain

    def cross(v, files):
        a, b = v.find(files[0]), v.find(files[1])
        v.set_entry(files[1], first=a.first, size=min(a.size, b.size))

    def into_free(v, files):
        chain = v.chain(v.find(files[0]).first)
        v.set_fat(chain[0], v.nclusters)                  # points at a free cluster

    def too_big(v, files):
        e = v.find(files[0])
        v.set_entry(files[0], size=e.size + 3 * v.cluster_bytes)

    def stale(v, files):
        a, b = v.find(files[0]), v.find(files[1])           # k2's entry now points at k3's clusters (k3 itself goes)
        off = v.entry_offset(files[1])
        v.win.write(off, b"\xe5")
        v.set_entry(files[0], first=b.first, size=min(a.size, b.size))

    return [("lost cluster", lost, False), ("chain longer than size", chain_longer, False), ("cross-link", cross, True),
            ("chain into a free cluster", into_free, True), ("size beyond the chain", too_big, True),
            ("another file's bytes", stale, True)]


@test
def storage_crash_checker_selftest():
    """The crash test's judges (mkdisk's checker, the kernel's ledger check): power-cut leftovers pass, cross-links, broken chains and
    stale bytes are caught (no VM)."""
    files = {f"{CHURN_DIR}/k2 first.txt": bytes(churn_bytes(2, 9000)), f"{CHURN_DIR}/K3.DAT": bytes(churn_bytes(3, 5000))}
    names = list(files)
    for what, damage, expect in crash_selftest_images():
        with Image("crash-self", 40 * MIB, [hv_part(files, cluster=512)]) as img:
            v = mkdisk.open_volume(img.path, 1, writable=True)
            try:
                damage(v, names)
            finally:
                v.close()
            mkdisk.set_dirty(img.path, 1, True)                       # as a power cut leaves it
            rep = mkdisk.check_image(img.path)
            v = mkdisk.open_volume(img.path, 1)
            try:
                found = serious_errors(rep) + [f"{p}: stale byte at {o}" for p, o in stale_bytes(v)]
            finally:
                v.close()
            assert bool(found) == expect, f"{what}: expected {'a' if expect else 'no'} finding, got {found}\n" + rep.text(True)
            if what != "another file's bytes":                      # a matter of contents, which no FAT checker can see
                ledger = fatck_host.scan_volume(img.path, 1)
                assert ledger.ok != expect, f"{what}: the kernel's ledger check says {'ok' if ledger.ok else 'damaged'}\n" + ledger.text()


def crash_run(seed):
    """One power cut. Returns None, or a text that says what is wrong."""
    rng = random.Random(seed)
    kind, cluster, size = rng.choice([("fat32", None, 96), ("fat32", 512, 96), ("fat32", 4096, 300), ("fat16", None, 96)])
    ops = 20 * rng.randint(0, 60)                         # kill after this many operations (0: right at the start) ...
    jitter = rng.uniform(0.0, 1.0)                        # ... plus up to a second
    tag = f"seed {seed} ({kind}, {cluster or 'default'} B clusters, kill after {ops} ops + {jitter:.2f} s)"
    with Image("crash", size * MIB, [hv_part(cluster=cluster, kind=kind)]) as img:
        with boot(img, name="t-storage-crash") as (g, vm):
            vm.send(f"fstest -churn {seed} {H}\n")
            vm.expect(r"churn start seed", timeout=60)
            if ops:
                vm.expect(rf"churn {ops}\r?\n", timeout=60 + ops)
            time.sleep(jitter)
            try:
                vm.qmp("quit")                            # the plug: no sync, no poweroff
            except Exception:                             # noqa: BLE001 -- QEMU may close the socket before it answers
                pass
            vm.proc.wait(timeout=scaled(30))
            fails = [line for line in vm.text().splitlines() if "CHURNFAIL" in line]
        rep = mkdisk.check_image(img.path)
        problems = serious_errors(rep)
        if not problems:
            v = img.volume(1)
            try:
                problems = [f"{p}: a byte the file never held, at offset {o}" for p, o in stale_bytes(v)]
                files = sum(1 for ents in v._tree_cache().values() for e in ents if not e.is_dir)
            finally:
                v.close()
        # The kernel's own ledger check (shared/fatck.c, the code that decides at the next boot whether the volume may be written)
        # must find no damage either: then the next boot mounts it read-write (fsck=left), lost clusters and all.
        ledger = fatck_host.scan_volume(img.path, 1)
        if not ledger.ok:
            problems += ["the kernel's ledger check would keep the volume read-only: "
                         + ("; ".join(ledger.errors[:4]) or "the scan did not finish")]
        if fails:
            problems += ["the workload saw a failing call: " + "; ".join(fails[:4])]
        if problems:
            if os.environ.get("TOUHOU_KEEP_IMAGES"):
                tag += f", image kept as {img.path}"
            return f"{tag}:\n  " + "\n  ".join(problems[:12]) + "\n" + rep.text(True)[-2500:]
        leftovers = [e for e in rep.errors]
        print(f"     crash run {tag}: {files} files, ok, ledger check: {len(ledger.leaks)} leftover(s)" + (f"; leftovers: {'; '.join(leftovers)[:300]}" if leftovers else ""),
              flush=True)
        return None


@test
def storage_crash_consistency():
    """Power cut at a random moment of a write-heavy workload, N times (TOUHOU_CRASH_RUNS, default 3): at worst lost clusters."""
    runs = int(os.environ.get("TOUHOU_CRASH_RUNS", "3"))
    base = int(os.environ.get("TOUHOU_CRASH_SEED", str(int(time.time()) & 0xFFFFFF)))
    print(f"     crash test: {runs} run(s), seeds {base}..{base + runs - 1}", flush=True)
    failed = []
    for i in range(runs):
        why = crash_run(base + i)
        if why:
            failed.append(why)
            print("     FAILED " + why.splitlines()[0], flush=True)
    assert not failed, (f"{len(failed)} of {runs} power cuts left damage (replay one with TOUHOU_CRASH_SEED=<seed> "
                        f"TOUHOU_CRASH_RUNS=1):\n" + "\n".join(failed))
