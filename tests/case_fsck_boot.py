"""Mending at boot: the opt-in kernel word hvfsck=safe (docs/STORAGE.md, "Mending at boot").

A HUMANVILLAGE volume that was not shut down cleanly is gone through before it is mounted (tests/case_fsck.py, fsck_kernel_*). With `hvfsck=safe` on
the kernel command line, a check that finds damage is followed by the repairs of `fsck -a` on the still unmounted volume (the engine of /bin/fsck,
shared/fatfix.c, in the kernel) and by a second check that reads the volume from the device again; the volume is mounted read-write only if that comes
out clean (or with harmless leftovers), else read-only as before. These tests boot the kernel on damaged images and look at what it did with them. The
judges are the host's own run of the same engine (tools/fatck_host.py: the oracle for what a volume should look like afterwards), the independent Python
checker of tools/mkdisk.py, and the file lists of tests/case_fsck.py (AIMED: which files a damage is aimed at, what must survive of them).

  fsck_boot_menu_512 / _4096   every entry of the damage menu of case_fsck.py on FAT32 with 512- and with 4096-byte clusters, booted with the word
  fsck_boot_without_the_word   the same kinds of damage without the word: the behaviour of before (read-only, the advice, the image untouched)
  fsck_boot_needs_y            damage that needs `fsck -y` (alone, and mixed with damage that does not): stays read-only, says why
  fsck_boot_readonly_device    a device that cannot be written (virtio read-only, NVMe under blk.ro with and without blk.rw=): the word says it cannot act
  fsck_boot_4kn, _fat16        a disk with 4096-byte sectors; a FAT16 volume
  fsck_boot_stick              a damaged Human Village on a USB stick (found by the automount thread, not by the boot call)
  fsck_boot_write_error        the disk refuses a write in the middle of the repair: it stops, says why, stays read-only; the next start goes on
  fsck_boot_words              the word itself: unknown values, hvcheck=off, hvforce, a check that cannot finish
  fsck_boot_log                the repair lines reach TOUHOU.LOG on the volume; the recovered files carry the volume's own time
  fsck_boot_watchdog           a repair that outlasts the watchdog the loader armed (wdt=10) does not end in a restart
  fsck_boot_power_cuts         QEMU killed at the Nth write request of a repair, then the same word again
  fsck_boot_cuts_host          the same question sector by sector on the host (no VM), for two big repairs; _menu: for every entry of the damage menu

  Run them with `python tools/qemu-slot.py -- python tests/run_all.py fsck_boot_` (fsck_boot_cuts_host needs no VM).
  TOUHOU_FSCKBOOT_ONLY=name,name   only these entries of the damage menu (fsck_boot_menu_*)
  TOUHOU_FSCKBOOT_CUTS=N|all       cut points per scenario of fsck_boot_power_cuts (default 6)     TOUHOU_FSCKBOOT_IOPS=N   writes per second during the cut runs
  TOUHOU_FSCKBOOT_HOSTCUTS=N|all   random cut points per scenario of fsck_boot_cuts_host (default 60)
"""
import contextlib
import os
import random
import re
import shutil
import time

import mkdisk
import fatck_host
from case_storage import Image, boot, klog, hv_part, MIB, WORK, H
import case_fsck as cf
from harness import test, scaled, Qemu

SCRATCH = WORK / "fsck-boot"
FIX_SAFE = fatck_host.FIX_SAFE
ONLY = [n for n in os.environ.get("TOUHOU_FSCKBOOT_ONLY", "").split(",") if n]

GEOMETRIES = {512: 64 * MIB, 4096: 300 * MIB}          # cluster bytes -> size of the disk (FAT32 needs 65525 clusters: 4 KiB ones need 256 MiB)
PROBE = "/reimu/after.txt"                              # what the guest writes to see whether the volume is writable
UNMOUNTABLE = {"root chain broken"}                     # damage that keeps the FAT driver from reading the root directory: never mounted


def scratch(name):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return SCRATCH / name


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


# ---------------------------------------------------------------------------------------------------------------------
# images and oracles

_pristine = {}


def pristine_files(cluster):
    """(files, dirs) of an undamaged volume of this geometry, as a reader of the volume sees them."""
    if cluster not in _pristine:
        p = cf.build_image(scratch(f"pristine-{cluster}.img"), "fat32", size=GEOMETRIES[cluster], cluster=cluster)
        _pristine[cluster] = cf.volume_files(p)
        p.unlink()
    return _pristine[cluster]


def menu_names():
    p = cf.build_image(scratch("names.img"), "fat32")
    r = mkdisk.open_volume(p, 1)
    try:
        names = list(cf.damage_menu(r))
    finally:
        r.close()
        p.unlink()
    return [n for n in names if not ONLY or n in ONLY]


def entry_image(tag, entry, cluster):
    """A dirty HUMANVILLAGE volume (FAT32, this cluster size) damaged with one entry of the damage menu."""
    def mutate(r):
        cf.damage_menu(r)[entry][1](r)
    return cf.dirty_image(tag, mutate, size=GEOMETRIES[cluster], cluster=cluster)


def classify(path):
    """What the kernel's check is going to say about this image, from the host's run of the same scanner:
    refused (the boot sector is unusable), unverified (the scan cannot finish), damage, leftovers (read-write, flag stays), tidy (read-write)."""
    out = fatck_host.scan_volume(path, 1)
    r = out.result
    if not r.opened:
        return "refused"
    if r.incomplete:
        return "unverified"
    if r.errors:
        return "damage"
    if r.leaks or r.hard_error:
        return "leftovers"
    return "tidy"


def host_repair(path, tag):
    """The host's own run of the engine (FIX_SAFE) on a copy of the image: (copy, FixStats, ScanOutcome of the last scan). The copy is what the kernel's
    repair has to make of the same image (compare file trees: the time stamps of the files made from lost chains differ)."""
    twin = scratch(f"{tag}-twin.img")
    shutil.copyfile(path, twin)
    rc, st, lines, out = fatck_host.fix_volume(twin, 1, FIX_SAFE)
    assert rc == 0, f"the host's repair of {tag} stopped with {rc}: {lines[-3:]}"
    return twin, st, out


def tree(path):
    return cf.volume_files(path)


def without_probe(files_dirs):
    files, dirs = files_dirs
    return {p: b for p, b in files.items() if p != PROBE}, dirs


def mounted(g):
    """'rw', 'ro' or None: how /HumanVillage is mounted."""
    m = re.search(r"/dev/\S+ /HumanVillage fat32 (rw|ro) ", g.ok("cat /proc/mounts"))
    return m.group(1) if m else None


# ---------------------------------------------------------------------------------------------------------------------
# one damaged image, one boot

def run_entry(entry, cluster, word, bootlog=False, second_boot=False):
    """Boots a dirty volume damaged with `entry` (with `hvfsck=safe` when `word`) and returns a list of complaints (empty = all well)."""
    bad = []
    aimed, leaves, keep = cf.AIMED.get(entry, ([], False, None))
    tag = f"{entry} [{cluster} B clusters{', hvfsck=safe' if word else ', no word'}]"
    name = f"fsckboot-{cluster}-{slug(entry)}"
    with entry_image(name, entry, cluster) as img:
        kind = classify(img.path)
        h0 = img.sha()
        twin = st = out = None
        if kind == "damage":
            twin, st, out = host_repair(img.path, name)
        cmd = "hvfsck=safe" if word else ""
        mends = kind == "damage" and out.result.errors == 0 and not out.result.incomplete
        # (a root directory whose chain is broken cannot be read by the FAT driver at all: with or without the word the volume is not mounted, not even read-only)
        want_mode = None if entry in UNMOUNTABLE else "ro"
        try:
            with boot(img, root=True, name="t-" + name, cmdline=cmd, bootlog=bootlog) as (g, vm):
                log = klog(vm)
                mode = mounted(g)
                fc = g.ok("cat /proc/fatcheck")
                tried = "hvfsck=safe is set: Keine mends what loses nothing" in log
                if kind == "refused":
                    if mode or "cannot be mounted" not in log or tried:
                        bad.append(f"{tag}: a refused volume: mounted {mode}, tried to mend {tried}")
                elif kind == "tidy":
                    if mode != "rw" or "nothing is out of place: the Human Village may write in it again" not in log or tried:
                        bad.append(f"{tag}: nothing wrong but the flag: mounted {mode}, tried {tried}\n{log[-1500:]}")
                elif kind == "leftovers":
                    if mode != "rw" or "found only harmless leftovers" not in log or tried:
                        bad.append(f"{tag}: harmless leftovers: mounted {mode}, tried {tried}\n{log[-1500:]}")
                elif kind == "damage" and not word:
                    if mode != want_mode or "It stays read-only so that nothing gets worse" not in log or tried:
                        bad.append(f"{tag}: damage without the word must stay read-only: {mode}, tried {tried}\n{log[-1500:]}")
                    if "As root, `umount /HumanVillage` and run `fsck -a /dev/vda1`" not in log or "DAMAGED" not in fc:
                        bad.append(f"{tag}: the advice is missing\n{log[-1500:]}")
                    # the new way comes first, the manual way stays (in the log line and in /proc/fatcheck)
                    first, hand = log.find("put hvfsck=safe on the kernel command line"), log.find("As root, `umount /HumanVillage` and run `fsck -a /dev/vda1`")
                    if not 0 <= first < hand or "fix: put hvfsck=safe on the kernel command line" not in fc or fc.find("fix: put hvfsck=safe") > fc.find("fix: as root"):
                        bad.append(f"{tag}: the advice does not name hvfsck=safe before the manual way\n{log[-1500:]}\n{fc}")
                elif kind == "damage" and mends:
                    if mode != "rw" or not tried or "mended with hvfsck=safe in" not in log:
                        bad.append(f"{tag}: a repairable damage must be mended and mounted read-write: {mode}, tried {tried}\n{log[-2500:]}")
                    if "hvfsck=safe: mended in" not in fc or "afterwards" not in fc:
                        bad.append(f"{tag}: /proc/fatcheck has no repair report:\n{fc}")
                elif kind == "damage":
                    if mode != want_mode or not tried or "as far as it goes" not in log or "fsck -y /dev/vda1" not in log:
                        bad.append(f"{tag}: damage that needs fsck -y must stay read-only and say why: {mode}, tried {tried}\n{log[-2500:]}")
                    if "hvfsck=safe: mended in" not in fc or "needs a decision" not in fc:
                        bad.append(f"{tag}: /proc/fatcheck does not say what is left:\n{fc}")
                if mode == "rw":
                    g.ok(f"echo written after the check > {H}/after.txt; sync")
                elif mode == "ro":
                    g.fails(f"echo no > {H}/new.txt", r"[Rr]ead-only")
                g.poweroff()
        except Exception as ex:                                              # noqa: BLE001 -- a hang or a crash of the guest is a finding, not a reason to stop the loop
            bad.append(f"{tag}: the guest failed: {type(ex).__name__}: {str(ex)[:600]}")
            _keep(img, name)
            return bad
        after = tree(img.path) if kind != "refused" else None            # (the host's reader cannot open a volume whose boot sector is gone)
        rep = fatck_host.scan_volume(img.path, 1)
        if kind in ("refused",) or (kind == "damage" and not word):
            if img.sha() != h0:
                bad.append(f"{tag}: the image changed although the volume was not to be written")
        elif kind == "damage":
            ref = tree(twin)
            body = without_probe(after) if mends else after
            if body != ref:
                bad.append(f"{tag}: the kernel's repair is not the host's: only on the host {sorted(set(ref[0]) - set(body[0]))[:4]}, "
                           f"only in the guest {sorted(set(body[0]) - set(ref[0]))[:4]}, different {[p for p in ref[0] if p in body[0] and ref[0][p] != body[0][p]][:4]}")
            if mends:
                if not cf.volume_flagged_clean(img.path) or cf.py_class(img.path)[0] not in ("ok", "warn") or cf.c_class(img.path)[0] not in ("ok", "warn"):
                    bad.append(f"{tag}: not clean afterwards: python {cf.py_class(img.path)}, C {rep.text()[:300]}")
                if (after[0].get(PROBE) or b"") != b"written after the check\n":
                    bad.append(f"{tag}: the file written after the mount is not on the volume")
            elif rep.result.errors == 0:
                bad.append(f"{tag}: the damage was to stay: the checker finds nothing wrong afterwards")
            if (leaves and mends) or (not leaves and not mends):
                bad.append(f"{tag}: the host says {'mended' if mends else 'partial'}, case_fsck.AIMED says leaves={leaves}")
            bad += [f"{tag}: {m}" for m in cf.compare_trees(pristine_files(cluster), without_probe(after), aimed)]
            if entry == "long name without its entry" and mends:                  # a lost file: its exact bytes, padded with zeros to a whole cluster, in FOUND.000
                orig = pristine_files(cluster)[0][cf.P_LONG]
                chk = {p: b for p, b in after[0].items() if p.startswith("/FOUND.000/")}
                hit = [b for b in chk.values() if b[:len(orig)] == orig and not any(b[len(orig):]) and len(b) - len(orig) < cluster]
                if len(hit) != 1 or cf.P_LONG in after[0]:
                    bad.append(f"{tag}: the lost file ({len(orig)} bytes) is not in FOUND.000 once, with its exact bytes: {[(p, len(b)) for p, b in chk.items()]}")
            if keep and mends:
                for p, data in sorted(pristine_files(cluster)[0].items()):
                    if cf.under(p, aimed) and keep == "all" and not cf.recovered(data, p, after[0]):
                        bad.append(f"{tag}: the {len(data)} bytes of {p} are nowhere on the volume any more")
                    elif cf.under(p, aimed) and keep == "prefix" and p in after[0] and not data.startswith(after[0][p]):
                        bad.append(f"{tag}: {p} is not the start of what it was")
        elif kind in ("tidy", "leftovers"):
            before_ok = cf.compare_trees(pristine_files(cluster), without_probe(after), aimed or [], strict_new=True)
            bad += [f"{tag}: {m}" for m in before_ok if not m.endswith("appeared") or "FOUND.000" in m]
            if kind == "tidy" and not cf.volume_flagged_clean(img.path):
                bad.append(f"{tag}: the flag was not cleared again after a clean power-off")
            if kind == "leftovers" and cf.volume_flagged_clean(img.path):
                bad.append(f"{tag}: the leftovers are still there: the flag must stay until fsck has tidied them")
        if second_boot and not bad and kind == "damage" and mends:
            try:
                with boot(img, root=True, name="t-" + name + "-2") as (g, vm):
                    log2 = klog(vm)
                    if "was not closed properly" in log2 or "goes through the whole ledger" in log2 or "hvfsck" in log2 or mounted(g) != "rw":
                        bad.append(f"{tag}: the second boot (without the word) is not clean:\n{log2[-1500:]}")
                    g.poweroff()
            except Exception as ex:                                          # noqa: BLE001
                bad.append(f"{tag}: the second boot failed: {type(ex).__name__}: {str(ex)[:400]}")
        if bad:
            _keep(img, name)
        if twin is not None:
            twin.unlink(missing_ok=True)
    return bad


def _keep(img, name):
    """Keeps the image of a run that failed (the Image removes its file when the context ends)."""
    dst = SCRATCH / f"FAILED-{name}.img"
    SCRATCH.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copyfile(img.path, dst)
        print(f"     (the image of {name} is kept as {dst})", flush=True)
    except OSError:
        pass


def run_menu(cluster, word=True, names=None):
    names = names or menu_names()
    bad = []
    for i, entry in enumerate(names):
        if entry not in cf.AIMED and entry not in cf.REFUSED:
            bad.append(f"{entry}: the damage menu has a new entry that tests/case_fsck.py does not know (AIMED / REFUSED)")
            continue
        t0 = time.time()
        got = run_entry(entry, cluster, word, second_boot=word)             # (a repaired volume is started again without the word: nothing to check)
        print(f"     {'FAILED' if got else 'ok    '} {entry} ({cluster} B clusters, {time.time() - t0:.0f} s)", flush=True)
        bad += got
    return bad


@test
def fsck_boot_menu_512():
    """Every entry of the damage menu on FAT32 with 512-byte clusters, the volume flagged dirty, booted with hvfsck=safe: a damage that `fsck -a` mends is
    mended before the mount (the log says so, the volume is read-write and clean afterwards, every file that was not aimed at is byte for byte what it was,
    a lost file is in FOUND.000 with its exact bytes, the volume is what the host's run of the same engine makes of the image), a damage that needs -y stays
    read-only and says why, harmless leftovers and a bare dirty flag take the path they always took, a damaged boot sector is refused untouched."""
    bad = run_menu(512)
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_menu_4096():
    """The same with 4096-byte clusters (a 300 MiB volume)."""
    bad = run_menu(4096)
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


# ---------------------------------------------------------------------------------------------------------------------
# power cuts during the repair: QEMU is killed after the Nth write request of a boot-time repair, then the kernel starts again with the same word

def x_forty_lose_entries(r):
    """The forty files of /many lose their directory entries (their long names stay behind as orphans): forty lost chains, FOUND.000 has to grow."""
    for e in r.listdir("/many"):
        r.win.write(r.entry_offset("/many/" + e.name), b"\xe5")
    cf.make_dirty(r)


def check_forty(before, after, mode=None):
    saved = [b for p, b in after[0].items() if p.startswith("/FOUND.000/")]
    return [f"{p}: {len(b)} bytes are not among the recovered files" for p, b in before[0].items()
            if p.startswith("/many/") and not any(s.startswith(b) for s in saved)]


def x_five_at_once(r):
    cf.menu_steps("long name without its entry", "size larger than the chain", "cross-link", "FAT copy 2 differs (clean flags)",
                  "lost chain (dirty)")(r)


# name -> (damage, aimed files, what survives of them, extra check)
CUT_SCENARIOS = {
    "forty files lose their entries": (x_forty_lose_entries, ["/many"], None, check_forty),
    "five kinds of damage at once": (x_five_at_once, [cf.P_LONG, cf.P_SPELL, cf.P_HELLO], None, None),
}


def canonical(files_dirs):
    """A tree made comparable: the files outside FOUND.000 by path, the recovered files as a sorted list of contents (their numbers may differ
    after a repair that was cut and resumed), the directories without FOUND.000."""
    files, dirs = files_dirs
    plain = {p: b for p, b in files.items() if not p.startswith("/FOUND.000/") and p != PROBE}
    found = sorted(b for p, b in files.items() if p.startswith("/FOUND.000/"))
    return plain, found, {d for d in dirs if d != "/FOUND.000"}


def benign_surplus(b, others, cb):
    """May this recovered file be there although the uninterrupted repair does not make it? A repair that was cut and went on can leave clusters of its own
    making behind, which the next repair saves like any lost chain: a zeroed cluster that was meant for FOUND.000, a directory cluster of FOUND.000 that was
    written but not yet linked (its entries, FILEnnnn.CHK, or a new FOUND.000 with its `.` and `..`), or the copy that a cross-linked file was to get (made
    in part or in full: every cluster of it is in another file of the volume as it is). Anything else would be data that nobody had. Looks at the cluster
    size `cb` at a time."""
    def entries(blk):
        return all(not any(blk[i:i + 32]) or (blk[i:i + 4] == b"FILE" and blk[i + 8:i + 11] == b"CHK") or blk[i:i + 11] in (b".          ", b"..         ")
                   for i in range(0, len(blk), 32))
    for i in range(0, len(b), cb):
        blk = b[i:i + cb]
        if not any(blk) or entries(blk) or any(blk in o for o in others):
            continue
        return False
    return True


def same_as_reference(got, ref, final_files, cb=512):
    """Is the canonical tree `got` (a repair that was cut and went on) what the uninterrupted repair `ref` makes? The files outside FOUND.000 and the
    directories must be equal; every recovered file of the reference must be among the recovered files of `got`; and `got` may have more of them only if
    benign_surplus() says so. `final_files` is {path: bytes} of the volume. Returns a list of complaints."""
    from collections import Counter
    bad = []
    if got[0] != ref[0]:
        bad.append(f"files outside FOUND.000 differ: only after the cut {sorted(set(got[0]) - set(ref[0]))[:3]}, only in the reference {sorted(set(ref[0]) - set(got[0]))[:3]}, "
                   f"different {[p for p in ref[0] if p in got[0] and got[0][p] != ref[0][p]][:3]}")
    if got[2] != ref[2]:
        bad.append(f"directories differ: {sorted(got[2] ^ ref[2])[:3]}")
    have, want = Counter(got[1]), Counter(ref[1])
    lost = want - have
    if lost:
        bad.append(f"{sum(lost.values())} recovered file(s) of the reference are missing, sizes {sorted(len(b) for b in lost.elements())[:5]}")
    for b in (have - want).elements():
        rest = list(got[1])
        rest.remove(b)                                                  # (not the file itself)
        others = [o for p, o in final_files.items() if not p.startswith("/FOUND.000/")] + rest
        if not benign_surplus(b, others, cb):
            bad.append(f"a recovered file of {len(b)} bytes that the reference does not have, and that is neither zeros, FOUND.000's own entries nor a copy of other files")
    return bad


def write_ops(vm):
    """Write requests QEMU has completed for the data disk."""
    stats = vm.qmp("query-blockstats")
    for d in stats:
        if d.get("device") == "data0" or d.get("node-name") == "data0":
            return d["stats"]["wr_operations"]
    raise AssertionError(f"the data disk is not among the block devices: {[(d.get('device'), d.get('node-name'), d.get('qdev')) for d in stats]}")


@contextlib.contextmanager
def throttled_vm(img, name, iops, cmdline="text bootlog=off user=root hvfsck=safe"):
    """A VM on the image whose writes QEMU slows to `iops` per second, so that a kill lands between the write requests of the repair."""
    with Qemu(cmdline=cmdline, disk=img.path, name=name, mem="512M") as vm:
        vm.qmp("block_set_io_throttle", device="data0", bps=0, bps_rd=0, bps_wr=0, iops=0, iops_rd=0, iops_wr=iops)
        yield vm


def repair_writes(scenario, iops):
    """The number of write requests the boot-time repair of this scenario makes (an uncut run, counted by QEMU)."""
    mutate = CUT_SCENARIOS[scenario][0]
    with cf.dirty_image("fsckboot-count", mutate) as img:
        with throttled_vm(img, "t-fsckboot-count", iops) as vm:
            vm.expect(r"hvfsck=safe is set: Keine mends", timeout=120)
            base = write_ops(vm)
            vm.expect(r"mended with hvfsck=safe in", timeout=300)
            n = write_ops(vm) - base
    return n


def cut_run(scenario, k, iops, ref):
    """One power cut: the kernel boots on the damaged image with the word, QEMU is killed after the k-th write request of the repair, the kernel starts
    again with the same word. Returns a list of complaints."""
    mutate, aimed, keep, extra = CUT_SCENARIOS[scenario]
    tag = f"{scenario}, cut after write {k}"
    name = f"fsckboot-cut-{slug(scenario)}-{k}"
    bad = []
    with cf.dirty_image(name, mutate) as img:
        with throttled_vm(img, "t-" + name, iops) as vm:
            vm.expect(r"hvfsck=safe is set: Keine mends", timeout=120)
            base = write_ops(vm)
            end = time.time() + scaled(120)
            while write_ops(vm) - base < k and vm.alive() and time.time() < end:
                time.sleep(0.005)
            seen = vm.text()
            done = "mended with hvfsck=safe in" in seen
            try:
                vm.qmp("quit")                                                  # the plug: no sync, no unmount, no warning
            except Exception:                                                   # noqa: BLE001 -- QEMU may close the socket before it answers
                pass
            vm.proc.wait(timeout=scaled(30))
        cut = classify(img.path)
        keep_img = SCRATCH / f"cut-{name}.img"
        shutil.copyfile(img.path, keep_img)                                     # (kept below if this run fails)
        try:
            with boot(img, root=True, name="t-" + name + "-2", cmdline="hvfsck=safe") as (g, vm):
                log = klog(vm)
                mode = mounted(g)
                if mode != "rw":
                    bad.append(f"{tag}: after the cut ({cut}) the second start did not mount the volume read-write ({mode}):\n{log[-2000:]}")
                else:
                    g.ok(f"echo written after the check > {H}/after.txt; sync")
                g.poweroff()
        except Exception as ex:                                                 # noqa: BLE001
            bad.append(f"{tag}: the second start failed: {type(ex).__name__}: {str(ex)[:500]}")
        rep = mkdisk.check_image(img.path)
        serious = rep.serious
        if serious:
            bad.append(f"{tag}: damage after the second start: {serious[:3]}")
        after = tree(img.path)
        # everything that is left over is tidied the way `fsck -a` does, then the volume is compared with the uninterrupted repair of the same damage
        fin = scratch(f"{name}-final.img")
        shutil.copyfile(img.path, fin)
        rc, st, lines, out = fatck_host.fix_volume(fin, 1, FIX_SAFE)
        final = tree(fin)
        fin.unlink()
        bad += [f"{tag}: not what the uninterrupted repair makes: {m}" for m in same_as_reference(canonical(final), ref["canonical"], final[0])]
        bad += [f"{tag}: {m}" for m in cf.compare_trees(pristine_files(512), without_probe(final), aimed)]
        if extra:
            bad += [f"{tag}: {m}" for m in extra(pristine_files(512), final)]
        if bad:
            dst = SCRATCH / f"FAILED-{name}.img"
            shutil.copyfile(keep_img, dst)
            bad.append(f"(the image as the cut left it: {dst})")
        keep_img.unlink(missing_ok=True)
    return bad, f"{tag} (the image was '{cut}'{', repair was already done' if done else ''})"


@test
def fsck_boot_power_cuts():
    """QEMU killed at the k-th write request of a boot-time repair (the disk's writes are slowed to a few per second so that the kill lands between the
    requests), and the kernel started again with the same word: the volume is mounted read-write, has no damage worse than harmless leftovers, and
    after `fsck -a` of the host is what the uninterrupted repair of the same damage makes of it (every file that was not aimed at byte for byte, nothing
    lost that the first repair would have kept). TOUHOU_FSCKBOOT_CUTS=N cut points per scenario (default 6; `all`: every write request), TOUHOU_FSCKBOOT_IOPS=N
    (default 8) writes per second, TOUHOU_FSCKBOOT_SEED=S."""
    spec = os.environ.get("TOUHOU_FSCKBOOT_CUTS", "6")
    iops = int(os.environ.get("TOUHOU_FSCKBOOT_IOPS", "8"))
    rng = random.Random(int(os.environ.get("TOUHOU_FSCKBOOT_SEED", str(int(time.time()) & 0xFFFFFF))))
    failed, runs = [], 0
    for scenario, (mutate, aimed, keep, extra) in CUT_SCENARIOS.items():
        total = repair_writes(scenario, iops)
        print(f"     {scenario}: the repair makes {total} write requests", flush=True)
        assert total >= 3, f"{scenario}: the repair made only {total} write request(s)"
        with cf.dirty_image("fsckboot-ref", mutate) as img:                      # what the uninterrupted repair makes of this damage (the host's engine)
            twin, st, out = host_repair(img.path, "fsckboot-ref")
            ref = {"canonical": canonical(tree(twin))}
            twin.unlink()
        if spec == "all":
            points = list(range(total + 1))
        else:                                                                   # (the start, the first write, the middle, the end, and random points between)
            n = int(spec)
            points = sorted({0, 1, total // 2, total} | {rng.randrange(2, max(3, total - 1)) for _ in range(max(0, n - 4))})
        for k in points:
            bad, summary = cut_run(scenario, k, iops, ref)
            runs += 1
            print(("     FAILED " if bad else "     ok     ") + summary, flush=True)
            failed += bad
    assert not failed, f"{len(failed)} problem(s) in {runs} power cuts:\n" + "\n".join(failed[:30])


@test
def fsck_boot_cuts_host():
    """The decisions of the second start after a power cut, sector by sector and without a VM: the repair of each scenario is cut after k sectors (writes torn
    sector by sector, in the order the engine hands them over; TOUHOU_FSCKBOOT_HOSTCUTS=N random points besides the first, the middle and the last, `all` for
    every sector), and then the kernel's policy is played on the image with the same scanner and engine the kernel runs: what is left must be damage that
    `fsck -a` mends, harmless leftovers (mounted as they are) or a bare dirty flag, never a boot sector the kernel would refuse or a scan that cannot finish;
    after the second repair no damage remains; after a last `fsck -a` the volume is what the uninterrupted repair makes of the image."""
    spec = os.environ.get("TOUHOU_FSCKBOOT_HOSTCUTS", "60")
    rng = random.Random(77)
    bad, runs, seen = [], 0, {}
    for scenario, (mutate, aimed, keep, extra) in CUT_SCENARIOS.items():
        with cf.dirty_image("fsckboot-hostcut", mutate) as img:
            twin, st, out = host_repair(img.path, "fsckboot-hostcut-ref")
            ref = canonical(tree(twin))
            twin.unlink()
            dry = scratch("hostcut-dry.img")
            shutil.copyfile(img.path, dry)
            _res, total, _cut = fatck_host.fix_volume_cut(dry, 1, FIX_SAFE)
            dry.unlink()
            assert total > 5, f"{scenario}: the repair writes only {total} sector(s)"
            points = list(range(total + 1)) if spec == "all" else sorted({0, total // 2, total - 1, total} | {rng.randrange(total) for _ in range(int(spec))})
            for k in points:
                tag = f"{scenario}, cut after sector {k} of {total}"
                q = scratch("hostcut.img")
                shutil.copyfile(img.path, q)
                fatck_host.fix_volume_cut(q, 1, FIX_SAFE, write_budget=k)                 # the first start, cut
                kind = classify(q)
                seen[kind] = seen.get(kind, 0) + 1
                runs += 1
                if kind not in ("damage", "leftovers", "tidy"):
                    bad.append(f"{tag}: the kernel would find the volume '{kind}'")
                    continue
                if kind == "damage":                                                      # the second start mends it with the same word
                    rc, st2, lines, o2 = fatck_host.fix_volume(q, 1, FIX_SAFE)
                    if rc != 0 or o2.result.errors or classify(q) == "damage":
                        bad.append(f"{tag}: the second repair leaves damage: rc {rc}, {o2.errors[:2]}")
                        continue
                rc, st3, lines, o3 = fatck_host.fix_volume(q, 1, FIX_SAFE)               # what fsck -a would still tidy (leftovers keep the fsck=left path)
                final = tree(q)
                bad += [f"{tag}: not what the uninterrupted repair makes: {m}" for m in same_as_reference(canonical(final), ref, final[0])]
                bad += [f"{tag}: {m}" for m in cf.compare_trees(pristine_files(512), final, aimed)]
                if extra:
                    bad += [f"{tag}: {m}" for m in extra(pristine_files(512), final)]
                q.unlink()
    print(f"     {runs} cuts: the kernel would have found {seen}", flush=True)
    assert not bad, f"{len(bad)} problem(s) in {runs} cuts:\n" + "\n".join(bad[:30])


@test
def fsck_boot_cuts_host_menu():
    """The same sector-by-sector question for every entry of the damage menu (FAT32, 512-byte clusters; the entries that make the volume unmountable are left
    out): the repair is cut after the first sector, the middle, the end and some random ones (TOUHOU_FSCKBOOT_HOSTCUTS=N, default 3), the policy is played, and the
    volume ends where the uninterrupted repair of the same damage ends: clean, or with exactly the damage that needs `fsck -y` (an entry that is damage after
    the uninterrupted repair stays damage, and nothing else is)."""
    n = int(os.environ.get("TOUHOU_FSCKBOOT_HOSTCUTS", "3"))
    rng = random.Random(4242)
    bad, runs, seen = [], 0, {}
    for entry in menu_names():
        if entry in cf.REFUSED:
            continue
        aimed = cf.AIMED[entry][0]
        with entry_image("fsckboot-hostcut-menu", entry, 512) as img:
            if classify(img.path) != "damage":
                continue                                                          # (nothing for the word to do: the cases of the menu tests)
            twin, st, out = host_repair(img.path, "fsckboot-hostcut-menu-ref")
            ref, ref_errors = canonical(tree(twin)), out.result.errors
            twin.unlink()
            dry = scratch("hostcut-menu-dry.img")
            shutil.copyfile(img.path, dry)
            _res, total, _cut = fatck_host.fix_volume_cut(dry, 1, FIX_SAFE)
            dry.unlink()
            if total < 2:
                continue
            for k in sorted({0, 1, total // 2, total - 1, total} | {rng.randrange(total) for _ in range(n)}):
                tag = f"{entry}, cut after sector {k} of {total}"
                q = scratch("hostcut-menu.img")
                shutil.copyfile(img.path, q)
                fatck_host.fix_volume_cut(q, 1, FIX_SAFE, write_budget=k)
                kind = classify(q)
                seen[kind] = seen.get(kind, 0) + 1
                runs += 1
                if kind not in ("damage", "leftovers", "tidy"):
                    bad.append(f"{tag}: the kernel would find the volume '{kind}'")
                    continue
                if kind == "damage":                                                  # the second start
                    rc, st2, lines, o2 = fatck_host.fix_volume(q, 1, FIX_SAFE)
                    if rc != 0 or bool(o2.result.errors) != bool(ref_errors):
                        bad.append(f"{tag}: after the second repair {o2.result.errors} error(s) are left, the uninterrupted repair leaves {ref_errors}")
                        continue
                rc, st3, lines, o3 = fatck_host.fix_volume(q, 1, FIX_SAFE)
                final = tree(q)
                bad += [f"{tag}: not what the uninterrupted repair makes: {m}" for m in same_as_reference(canonical(final), ref, final[0])]
                bad += [f"{tag}: {m}" for m in cf.compare_trees(pristine_files(512), final, aimed)]
                q.unlink()
    print(f"     {runs} cuts over the menu: the kernel would have found {seen}", flush=True)
    assert not bad, f"{len(bad)} problem(s) in {runs} cuts:\n" + "\n".join(bad[:30])


WITHOUT = ["size larger than the chain", "hole in a chain", "lost chain (dirty)", "dirty flags only", "no FATs"]


@test
def fsck_boot_without_the_word():
    """The same images without the word behave as they always did: damage (one the engine could mend, one it could not) stays read-only with the advice
    and the image is untouched, leftovers and a bare dirty flag are mounted read-write, a damaged boot sector is refused."""
    bad = run_menu(512, word=False, names=[n for n in menu_names() if n in WITHOUT or ONLY])
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


# ---------------------------------------------------------------------------------------------------------------------
# damage that needs -y, a device that cannot be written, a 4096-byte-sector disk, the word itself, the log

def boot_once(img, cmd, name, check, bootlog=False, extra=(), dev=None):
    """Boots on `img` with the kernel words `cmd`, hands the guest, its log, its mount state and /proc/fatcheck to check(), powers off. Returns complaints.
    With `dev` set the disk comes from `extra` (a read-only, NVMe or 4Kn drive) instead of the harness's own virtio disk."""
    bad = []
    try:
        with boot(img if dev is None else None, root=True, name=name, cmdline=cmd, bootlog=bootlog, extra=list(extra)) as (g, vm):
            log = klog(vm)
            mode = mounted(g)
            fc = g.ok("cat /proc/fatcheck")
            got = check(g, log, mode, fc)
            if got:
                bad += got if isinstance(got, list) else [got]
            g.poweroff()
    except Exception as ex:                                                  # noqa: BLE001 -- a hang or a crash is a finding, not a reason to stop
        bad.append(f"{name}: the guest failed: {type(ex).__name__}: {str(ex)[:600]}")
    return bad


@test
def fsck_boot_needs_y():
    """Damage that needs `fsck -y` stays read-only and says why, alone and mixed with damage that `fsck -a` mends (that part is mended: the host's run of
    the same engine says what the volume looks like). Then the advice is followed: one start with nohumanvillage, `fsck -y` by hand, and the next start with
    the word finds nothing to check."""
    bad = []
    cases = [("a hole in a chain", ["hole in a chain"]),
             ("a hole and three things fsck -a mends", ["hole in a chain", "long name without its entry", "FSInfo free count (clean flags)", "lost chain (dirty)"])]
    for label, steps in cases:
        name = "fsckboot-needs-y"
        with cf.flow_image(name, "menu:" + ",".join(steps)) as img:
            twin, st, out = host_repair(img.path, name)
            assert out.result.errors, f"{label}: the host's engine mends everything, this is no case for -y"
            ref = tree(twin)
            twin.unlink()

            def check(g, log, mode, fc):
                got = []
                if mode != "ro" or "as far as it goes" not in log or "fsck -y /dev/vda1" not in log or "nohumanvillage" not in log:
                    got.append(f"{label}: expected a read-only mount and the advice for -y: {mode}\n{log[-2500:]}")
                if "needs a decision" not in fc or "hvfsck=safe: mended in" not in fc or "left alone because they need -y" not in fc:
                    got.append(f"{label}: /proc/fatcheck: {fc}")
                if mode == "ro":
                    g.fails(f"echo no > {H}/new.txt", r"[Rr]ead-only")
                return got
            bad += boot_once(img, "hvfsck=safe", "t-" + name, check)
            if tree(img.path) != ref:
                bad.append(f"{label}: the safe part of the repair is not what the host's engine makes of the image")
            # the advice: nothing is mounted with nohumanvillage, fsck -y mends the rest, the next start with the word has nothing to do
            try:
                with boot(img, root=True, name="t-" + name + "-y", cmdline="nohumanvillage") as (g, vm):
                    out, rc = g.run("fsck -y /dev/vda1", 120)
                    if rc != 1 or "the volume is marked clean" not in out:
                        bad.append(f"{label}: fsck -y by hand exited with {rc}:\n{out}")
                    g.poweroff()
                with boot(img, root=True, name="t-" + name + "-3", cmdline="hvfsck=safe") as (g, vm):
                    log = klog(vm)
                    if mounted(g) != "rw" or "was not closed properly" in log or "mended" in log:
                        bad.append(f"{label}: after fsck -y the next start is not clean:\n{log[-1500:]}")
                    g.poweroff()
            except Exception as ex:                                          # noqa: BLE001
                bad.append(f"{label}: the advice could not be followed: {type(ex).__name__}: {str(ex)[:400]}")
            if cf.py_class(img.path)[0] not in ("ok", "warn") or not cf.volume_flagged_clean(img.path):
                bad.append(f"{label}: not clean after fsck -y: {cf.py_class(img.path)}")
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


def gpt_unique(path, index):
    """The unique partition GUID (text) of GPT entry `index` (1-based): what blk.rw= names."""
    import uuid
    with open(path, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        entries_lba, entry_size = int.from_bytes(hdr[72:80], "little"), int.from_bytes(hdr[84:88], "little")
        f.seek(entries_lba * 512 + (index - 1) * entry_size)
        entry = f.read(entry_size)
    return str(uuid.UUID(bytes_le=entry[16:32]))


@test
def fsck_boot_readonly_device():
    """The word cannot act on a device that cannot be written, and says so: a write-protected virtio disk, and an NVMe drive under `blk.ro` (the Dell's
    command line without blk.rw=); nothing is written, the volume is read-only as before. With `blk.rw=<the partition's GUID>` (the Dell's real command line)
    the same drive is written and the volume is mended."""
    from case_blockdev import nvme_ctrl
    bad = []
    steps = "menu:size larger than the chain,lost chain (dirty)"

    def cannot(dev):
        def check(g, log, mode, fc):
            got = []
            if mode != "ro" or f"hvfsck=safe is set, but {dev} cannot be written" not in log or "mends what loses nothing" in log:
                got.append(f"{dev}: the word must say it cannot act and the mount stay read-only: {mode}\n{log[-2000:]}")
            if "hvfsck=safe: not done, the device is read-only" not in fc:
                got.append(f"{dev}: /proc/fatcheck: {fc}")
            return got
        return check

    with cf.flow_image("fsckboot-rodev", steps) as img:
        h0 = img.sha()
        extra = ["-drive", f"if=none,id=d0,format=raw,file={img.path},readonly=on", "-device", "virtio-blk-pci,drive=d0"]
        bad += boot_once(img, "hvfsck=safe", "t-fsckboot-rodev", cannot("vda1"), extra=extra, dev="virtio-ro")
        if img.sha() != h0:
            bad.append("virtio read-only: the image changed")
    with cf.flow_image("fsckboot-nvme", steps) as img:
        h0 = img.sha()
        guid = gpt_unique(img.path, 1)
        extra = nvme_ctrl("nt", "FSCKBOOT", [(img.path, 1)])
        bad += boot_once(img, "blk.ro hvfsck=safe", "t-fsckboot-nvme-ro", cannot("nvme0n1p1"), extra=extra, dev="nvme")
        if img.sha() != h0:
            bad.append("nvme under blk.ro: the image changed")
        twin, st, out = host_repair(img.path, "fsckboot-nvme")
        ref = tree(twin)
        twin.unlink()

        def mends(g, log, mode, fc):
            got = []
            if mode != "rw" or "mended with hvfsck=safe in" not in log:
                got.append(f"nvme under blk.ro blk.rw=: must be mended and writable: {mode}\n{log[-2500:]}")
            if mode == "rw":
                g.ok(f"echo written after the check > {H}/after.txt; sync")
            return got
        bad += boot_once(img, f"blk.ro blk.rw={guid} hvfsck=safe", "t-fsckboot-nvme-rw", mends, extra=extra, dev="nvme")
        if without_probe(tree(img.path)) != ref:
            bad.append("nvme under blk.ro blk.rw=: the volume is not what the host's engine makes of the image")
        if not cf.volume_flagged_clean(img.path):
            bad.append("nvme under blk.ro blk.rw=: not clean afterwards")
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_4kn():
    """A disk with 4096-byte sectors (the engine reads and writes whole 4 KiB sectors): four kinds of damage at once are mended before the mount and the
    volume is what the host's run of the same engine makes of the image."""
    from case_storage import drive_args
    steps = ["long name without its entry", "size larger than the chain", "lost chain (dirty)", "FAT copy 2 differs (clean flags)"]
    with Image("fsckboot-4kn", 600 * MIB, [hv_part(cf.FILES)], sector_size=4096) as img:
        r = mkdisk.open_volume(img.path, 1, writable=True)
        try:
            assert r.bps == 4096
            cf.menu_steps(*steps)(r)
            cf.make_dirty(r)
            r.win.f.flush()
        finally:
            r.close()
        assert classify(img.path) == "damage"
        twin, st, out = host_repair(img.path, "fsckboot-4kn")
        assert out.result.errors == 0
        ref = tree(twin)
        twin.unlink()
        extra = drive_args(img.path, logical_block_size=4096, physical_block_size=4096)

        def check(g, log, mode, fc):
            got = []
            if mode != "rw" or "mended with hvfsck=safe in" not in log or "4096-byte sectors" not in log:
                got.append(f"4Kn: must be mended and writable: {mode}\n{log[-2500:]}")
            if mode == "rw":
                g.ok(f"echo written after the check > {H}/after.txt; sync")
            return got
        bad = boot_once(img, "hvfsck=safe", "t-fsckboot-4kn", check, extra=extra, dev="virtio-4k")
        if without_probe(tree(img.path)) != ref:
            bad.append("4Kn: the volume is not what the host's engine makes of the image")
        if not cf.volume_flagged_clean(img.path) or cf.py_class(img.path)[0] not in ("ok", "warn"):
            bad.append(f"4Kn: not clean afterwards: {cf.py_class(img.path)}")
        assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_fat16():
    """A FAT16 Human Village (a fixed root directory instead of a cluster chain; the policy mounts it as before): the same four kinds of damage at once are
    mended before the mount and the volume is what the host's run of the same engine makes of the image."""
    steps = ["long name without its entry", "size larger than the chain", "lost chain (dirty)", "FAT copy 2 differs (clean flags)"]
    with cf.dirty_image("fsckboot-fat16", cf.menu_steps(*steps), size=40 * MIB, kind="fat16") as img:
        assert classify(img.path) == "damage"
        twin, st, out = host_repair(img.path, "fsckboot-fat16")
        assert out.result.errors == 0
        ref = tree(twin)
        twin.unlink()

        def check(g, log, mode, fc):
            got = []
            if mode != "rw" or "mended with hvfsck=safe in" not in log or "FAT16" not in fc:
                got.append(f"FAT16: must be mended and writable: {mode}\n{log[-2500:]}")
            if mode == "rw":
                g.ok(f"echo written after the check > {H}/after.txt; sync")
            return got
        bad = boot_once(img, "hvfsck=safe", "t-fsckboot-fat16", check)
        if without_probe(tree(img.path)) != ref:
            bad.append("FAT16: the volume is not what the host's engine makes of the image")
        if not cf.volume_flagged_clean(img.path) or cf.py_class(img.path)[0] not in ("ok", "warn"):
            bad.append(f"FAT16: not clean afterwards: {cf.py_class(img.path)}")
        assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_stick():
    """A damaged Human Village on a USB stick: it is found after the boot-time call, by the automount thread (a thread of its own with a 16 KiB stack, running
    when the kernel's watchdog feeder already exists), and the word mends it there too, before the mount."""
    from case_usbstorage import XHCI, stick
    steps = ["long name without its entry", "size larger than the chain", "lost chain (dirty)", "FAT copy 2 differs (clean flags)"]
    with Image("fsckboot-stick", 100 * MIB, [hv_part(cf.FILES)]) as img:
        r = mkdisk.open_volume(img.path, 1, writable=True)
        try:
            cf.menu_steps(*steps)(r)
            cf.make_dirty(r)
            r.win.f.flush()
        finally:
            r.close()
        assert classify(img.path) == "damage"
        twin, st, out = host_repair(img.path, "fsckboot-stick")
        assert out.result.errors == 0
        ref = tree(twin)
        twin.unlink()
        bad = []
        try:
            with boot(None, root=True, name="t-fsckboot-stick", cmdline="hvfsck=safe", extra=XHCI + stick(img)) as (g, vm):
                log = klog(vm)
                mode = mounted(g)
                if mode != "rw" or not re.search(r"sd\w+ mended with hvfsck=safe in", log) or "the Human Village keeps its records on the stick now" not in log:
                    bad.append(f"stick: must be mended and writable: {mode}\n{log[-2500:]}")
                if mode == "rw":
                    g.ok(f"echo written after the check > {H}/after.txt; sync")
                g.poweroff()
        except Exception as ex:                                              # noqa: BLE001
            bad.append(f"stick: the guest failed: {type(ex).__name__}: {str(ex)[:500]}")
        if without_probe(tree(img.path)) != ref:
            bad.append("stick: the volume is not what the host's engine makes of the image")
        if not cf.volume_flagged_clean(img.path):
            bad.append("stick: not clean afterwards")
        assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_write_error():
    """The disk refuses a write in the middle of the repair (QEMU's blkdebug fails, once, the first request that touches the FSInfo sector after the first write):
    the repair stops where it is, says so with the reason, the volume is mounted read-only and nothing hangs or loops. The next start, with the disk in order again,
    goes on and ends with the volume the host's engine makes of the image."""
    from case_blockdev import drive
    with cf.flow_image("fsckboot-werr", "menu:size larger than the chain,lost chain (dirty)") as img:
        _, parts = mkdisk.partitions(img.path)
        r = mkdisk.open_volume(img.path, 1)
        try:
            fsinfo = parts[0]["first"] + r.fsinfo_sector * (r.bps // 512)
        finally:
            r.close()
        twin, st, out = host_repair(img.path, "fsckboot-werr")
        assert out.result.errors == 0
        ref = tree(twin)
        twin.unlink()
        args = drive("d0", img.path, write_error=fsinfo)
        args[1] += ",file.inject-error.0.once=on"                                   # (a sector that went bad for good could not even be read when the volume is mounted)
        extra = args + ["-device", "virtio-blk-pci,drive=d0"]

        def fails(g, log, mode, fc):
            got = []
            if mode != "ro" or "hvfsck=safe stopped after" not in log or "(the device failed)" not in log or "the next start goes on from there" not in log:
                got.append(f"a refused write: must stop, say why and stay read-only: {mode}\n{log[-2500:]}")
            if "hvfsck=safe: stopped after" not in fc or "fix: as root" not in fc:
                got.append(f"a refused write: /proc/fatcheck: {fc}")
            return got
        bad = boot_once(img, "hvfsck=safe", "t-fsckboot-werr", fails, extra=extra, dev="virtio-err")
        if classify(img.path) not in ("damage", "leftovers", "tidy"):
            bad.append("a refused write: the volume is unusable afterwards")

        def finishes(g, log, mode, fc):                                           # (what is left after the refused write is a stale FSInfo count: a leftover, which is mounted as it is)
            got = []
            if mode != "rw" or not any(w in log for w in ("mended with hvfsck=safe in", "found only harmless leftovers", "nothing is out of place")):
                got.append(f"the next start must mount the volume read-write: {mode}\n{log[-2500:]}")
            if mode == "rw":
                g.ok(f"echo written after the check > {H}/after.txt; sync")
            return got
        bad += boot_once(img, "hvfsck=safe", "t-fsckboot-werr2", finishes)
        fin = scratch("werr-final.img")
        shutil.copyfile(img.path, fin)
        fatck_host.fix_volume(fin, 1, FIX_SAFE)                                   # what is left over is tidied by hand, as the advice says
        final = tree(fin)
        fin.unlink()
        bad += [f"not what the host's engine makes of the image: {m}" for m in same_as_reference(canonical(final), canonical(ref), final[0])]
        assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_words():
    """The word itself. An unknown value and the bare word are said in the log and the volume is only checked; hvcheck=off makes the word moot; hvforce mounts
    over what the repair left; a check that cannot finish (directories nested too deep) leaves nothing for the word to mend; a clean volume is not looked at."""
    bad = []
    one = "menu:size larger than the chain"

    def stays(*must):
        def check(g, log, mode, fc):
            got = []
            if mode != "ro" or "mends what loses nothing" in log or any(m not in log for m in must):
                got.append(f"must stay read-only and say {must}: {mode}\n{log[-2000:]}")
            return got
        return check

    for word, want in (("hvfsck=all", "hvfsck=all: this kernel knows only hvfsck=safe"), ("hvfsck", "hvfsck needs a value (hvfsck=safe)")):
        with cf.flow_image("fsckboot-words", one) as img:
            h0 = img.sha()
            bad += boot_once(img, word, "t-fsckboot-words", stays(want, "It stays read-only so that nothing gets worse"))
            if img.sha() != h0:
                bad.append(f"{word}: the image changed")
    with cf.flow_image("fsckboot-words", one) as img:
        h0 = img.sha()
        bad += boot_once(img, "hvcheck=off hvfsck=safe", "t-fsckboot-words", stays("hvfsck=safe is ignored: hvcheck=off"))
        if img.sha() != h0:
            bad.append("hvcheck=off hvfsck=safe: the image changed")
    # hvforce: the safe part is mended, the rest is mounted over
    with cf.flow_image("fsckboot-words", "menu:hole in a chain,lost chain (dirty)") as img:
        def forced(g, log, mode, fc):
            got = []
            if mode != "rw" or "as far as it goes" not in log or "hvforce: vda1 is mounted read-write although its ledger does not add up" not in log:
                got.append(f"hvfsck=safe hvforce: {mode}\n{log[-2000:]}")
            return got
        bad += boot_once(img, "hvfsck=safe hvforce", "t-fsckboot-words", forced)
    # a check that cannot finish
    deep = "/".join(["d"] * 300) + "/f.txt"
    with cf.dirty_image("fsckboot-deep", files={"/reimu/a.txt": b"a", deep: b"deep"}) as img:
        h0 = img.sha()

        def unverified(g, log, mode, fc):
            got = []
            if (mode != "ro" or "check of its ledger could not be finished" not in log or "mends what loses nothing" in log
                    or "hvfsck=safe does not act on a volume whose ledger could not be gone through" not in log):
                got.append(f"an unfinished check: {mode}\n{log[-2000:]}")
            if "hvfsck=safe: not done, the check could not be finished" not in fc:
                got.append(f"an unfinished check: /proc/fatcheck: {fc}")
            return got
        bad += boot_once(img, "hvfsck=safe", "t-fsckboot-words", unverified)
        if img.sha() != h0:
            bad.append("an unfinished check: the image changed")
    # a clean volume: not looked at
    with Image("fsckboot-clean", 64 * MIB, [hv_part(cf.FILES)]) as img:
        def clean(g, log, mode, fc):
            got = []
            if mode != "rw" or "ledger" in log or "no volume has needed a consistency check" not in fc:
                got.append(f"a clean volume: {mode}\n{log[-1500:]}")
            return got
        bad += boot_once(img, "hvfsck=safe", "t-fsckboot-words", clean)
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_boot_watchdog():
    """The loader arms the TCO watchdog before the kernel starts and the kernel's own feeder exists only after the storage policy has run: a repair that
    lasts longer than the timeout must feed the timer itself, or the machine restarts in the middle of it. Here the timeout is 10 s (wdt=10) and the disk's
    writes are slowed to one a second, so that the repair of forty lost files takes far longer; the machine must go on and finish (QEMU's -no-reboot turns a
    watchdog reset into an exit of QEMU, which the test would see)."""
    with cf.dirty_image("fsckboot-wdt", CUT_SCENARIOS["forty files lose their entries"][0]) as img:
        with throttled_vm(img, "t-fsckboot-wdt", 1, cmdline="text bootlog=off user=root hvfsck=safe wdt=10") as vm:
            vm.expect(r"hvfsck=safe is set: Keine mends", timeout=120)
            t_start = time.time()
            vm.expect(r"mended with hvfsck=safe in (\d+) ms", timeout=300)
            took = time.time() - t_start
            assert took > 12, f"the repair took only {took:.0f} s: too short to show the watchdog (wdt=10)"
            vm.expect(r"root@gensokyo \S+ # ", timeout=120)
            assert vm.alive(), "the machine went away"
            log = vm.text()
            assert "watchdog: the previous start ended in a watchdog reset" not in log, log[-1500:]
            assert re.search(r"the loader armed it, the kernel takes it over|the watchdog is armed: 10 s", log), log[-2500:]
        rep = fatck_host.scan_volume(img.path, 1)                               # (the VM was stopped at the end of the with: the repair is on the disk)
        assert rep.ok and not rep.errors, rep.text()[:500]
        assert not rep.leaks and cf.volume_flagged_clean(img.path), rep.text()[:500]


@test
def fsck_boot_log():
    """What the repair says reaches the volume: TOUHOU.LOG on the mended volume holds the lines (the log is written when the volume comes up), and the files
    made from lost chains carry the time of the volume's own clock (tz=EST5EDT: five hours behind UTC, which is what FAT keeps)."""
    bad = []
    with cf.flow_image("fsckboot-log", "menu:long name without its entry,lost chain (dirty)") as img:
        def check(g, log, mode, fc):
            got = []
            if mode != "rw" or "mended with hvfsck=safe in" not in log:
                got.append(f"the repair did not happen: {mode}\n{log[-2000:]}")
            g.ok("sync")
            return got
        t_before = time.time()
        bad += boot_once(img, "hvfsck=safe tz=EST5EDT,M3.2.0,M11.1.0", "t-fsckboot-log", check, bootlog=True)
        t_after = time.time()
        v = img.volume(1)
        try:
            text = v.read_file("/TOUHOU.LOG").decode("utf-8", "replace")
            for want in ("its ledger does not add up", "hvfsck=safe is set: Keine mends what loses nothing", "mended with hvfsck=safe in",
                         "nothing is out of place and the volume is marked clean"):
                if want not in text:
                    bad.append(f"TOUHOU.LOG lacks {want!r}")
            stamps = [e.mtime for e in v.listdir("/FOUND.000") if not e.name.startswith(".")]
            if not stamps:
                bad.append("FOUND.000 holds no file")
            for m in stamps:
                if not (t_before - 5 * 3600 - 600 <= m <= t_after - 5 * 3600 + 600):
                    bad.append(f"a recovered file is stamped {m}, the host's UTC time was {t_before:.0f}..{t_after:.0f}: the stamp is not UTC minus five hours")
        finally:
            v.close()
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)
