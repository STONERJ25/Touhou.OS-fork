"""Host tests of the read-only ext2/ext3/ext4 driver (kernel/fs/ext4_core.c), without QEMU.

  python tools/hosttest.py discover -s tests/host -p "test_ext4.py"             (on Windows this runs in WSL's Ubuntu: it needs gcc, mke2fs, debugfs, e2fsck)
  python tools/hosttest.py discover -s tests/host -p "test_ext4.py" -k Fuzz      (one group; TOUHOU_EXT4_FUZZ=N sets the cases per image, 1500 by default)

The parser is built for the host by tests/host/ext4tool.c with AddressSanitizer and UndefinedBehaviorSanitizer (build/host-ext4/ext4tool) and checked against images that
tests/host/ext4img.py makes with `mke2fs -d` from generated source trees. The oracle is the host: the manifest (type, mode, owner, size, crc32 of the content, link target, hard links)
is computed from the *source tree* that was handed to mke2fs, and e2fsprogs' own programs say what they see (`dumpe2fs` for the geometry and every group, `debugfs stat` and `ex` for
the inodes and their extents). The generator and the parser are written to the same reading of the on-disk document; docs/EXT4.md lists where that could hide an error that none of
this finds.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import ext4img  # noqa: E402
from ext4img import MIB, GIB  # noqa: E402

TOOL = ROOT / "build" / "host-ext4" / "ext4tool"
IMGDIR = Path(os.environ.get("TOUHOU_EXT4_IMAGES", Path(tempfile.gettempdir()) / "touhou-ext4-host"))
WORK = IMGDIR / "damaged"
SOURCES = [ROOT / "tests/host/ext4tool.c", ROOT / "kernel/fs/ext4_core.c"]
HEADERS = [ROOT / "kernel/fs/ext4_priv.h"]
HAVE_TOOLS = shutil.which("gcc") is not None and ext4img.have_tools()

ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=77", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")


def build_tool():
    """Compiles the parser for the host with the sanitizers (once per run, again when a source is newer than the binary)."""
    newest = max(p.stat().st_mtime for p in SOURCES + HEADERS)
    if TOOL.exists() and TOOL.stat().st_mtime >= newest:
        return
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-DEXT4_HOST", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
           "-Wno-sign-compare", "-iquote", str(ROOT / "kernel/fs"), "-iquote", str(ROOT / "kernel/include"), *map(str, SOURCES), "-o", str(TOOL)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("cannot build tests/host/ext4tool.c:\n" + r.stdout + r.stderr)


_MANIFESTS = {}


def images(*profiles):
    """The manifests of the named profiles; the images are made on first use (and again when ext4img.py changes)."""
    need = [p for p in profiles if p not in _MANIFESTS]
    if need:
        _MANIFESTS.update(ext4img.ensure(IMGDIR, need))
    return {p: _MANIFESTS[p] for p in profiles}


def image_path(profile):
    return IMGDIR / f"{profile}.img"


def tool(image, *args, chunk=None, timeout=600):
    """Runs ext4tool on an image. Returns (exit status, stdout bytes, stderr text). A sanitizer report, a hang or a signal is a test failure on its own."""
    argv = [str(TOOL), str(image)]
    if chunk:
        argv += ["--chunk", str(chunk)]
    argv += map(str, args)
    r = subprocess.run(argv, capture_output=True, timeout=timeout, env=ENV)
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode < 0 or r.returncode >= 77 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
        raise AssertionError(f"ext4tool {' '.join(map(str, args))}: status {r.returncode}\n{err[-3000:]}")
    return r.returncode, r.stdout, err


def parse_walk(out):
    rows = {}
    for line in out.decode("utf-8", "replace").splitlines():
        parts = line.split("\t")
        if len(parts) >= 12:
            rows[parts[1]] = dict(kind=parts[0], size=int(parts[2]), mode=int(parts[3], 8), uid=int(parts[4]), gid=int(parts[5]), nlink=int(parts[6]), ino=int(parts[7]),
                                  mtime=int(parts[8]), mtime_ns=int(parts[9]), rdev=int(parts[10]), tail="\t".join(parts[11:]))
        elif parts[0] == "E":
            rows[parts[1]] = dict(kind="E", err=parts[2])
    return rows


def info(image):
    rc, out, err = tool(image, "info")
    assert rc == 0, out
    return dict(line.split("=", 1) for line in out.decode().splitlines() if "=" in line)


def assert_matches(tc, manifest, rows, ignore=("/lost+found",)):
    """The C walk of a volume against what mke2fs was given."""
    files = manifest["files"]
    seen_group = {}
    for path, exp in files.items():
        row = rows.get(path)
        tc.assertIsNotNone(row, f"{path} is missing from the walk")
        tc.assertNotEqual(row["kind"], "E", f"{path}: {row}")
        tc.assertEqual(row["kind"], exp["kind"], path)
        tc.assertEqual(row["mode"], exp["mode"], f"{path}: mode {row['mode']:o} against {exp['mode']:o}")
        tc.assertEqual(row["uid"], exp["uid"], path)
        tc.assertEqual(row["gid"], exp["gid"], path)
        if exp.get("mtime") is not None:
            tc.assertEqual(row["mtime"], exp["mtime"], path)
        if "mtime_ns" in exp:
            tc.assertEqual(row["mtime_ns"], exp["mtime_ns"], path)
        if exp["kind"] == "f":
            tc.assertEqual(row["size"], exp["size"], path)
            tc.assertEqual(row["tail"], "%08x" % exp["crc"], f"{path}: the content differs")
            tc.assertEqual(row["nlink"], exp["nlink"], f"{path}: link count")
        elif exp["kind"] == "l":
            tc.assertEqual(row["size"], exp["size"], path)
            tc.assertEqual(row["tail"], exp["target"], path)
        elif exp["kind"] in "cb":
            tc.assertEqual(row["rdev"], exp["rdev"], path)
        if exp["kind"] != "d" and exp.get("group") is not None:         # hard links: the same source inode is the same image inode, and no other one is
            g = exp["group"]
            if g in seen_group:
                tc.assertEqual(row["ino"], seen_group[g], f"{path}: not the same inode as its hard link")
            else:
                seen_group[g] = row["ino"]
    inos = list(seen_group.values())
    tc.assertEqual(len(inos), len(set(inos)), "two different source files share an inode")
    extra = {p for p in rows if p not in files and not any(p == i or p.startswith(i + "/") for i in ignore)}
    tc.assertEqual(extra, set(), "paths the source tree did not have")


def debugfs_batch(img, cmds):
    """One debugfs run for many commands: {command: output}."""
    with tempfile.NamedTemporaryFile("w", suffix=".cmd", delete=False) as t:
        t.write("\n".join(cmds) + "\n")
    try:
        r = subprocess.run(["debugfs", "-f", t.name, str(img)], capture_output=True, text=True, errors="replace", env=dict(os.environ, PAGER="cat", DEBUGFS_PAGER="cat"))
    finally:
        os.unlink(t.name)
    out = {}
    cur = None
    for line in r.stdout.splitlines(keepends=True):
        if line.startswith("debugfs: "):
            cur = line[len("debugfs: "):].strip()
            out[cur] = ""
        elif cur is not None:
            out[cur] += line
    return out


@unittest.skipUnless(HAVE_TOOLS, "needs gcc, mke2fs, debugfs and e2fsck (WSL's Ubuntu on Windows)")
class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()
        WORK.mkdir(parents=True, exist_ok=True)


# The profiles the functional tests walk (the small fz_ images and the big ones are used by other groups)
FUNCTIONAL = ["ext2_1k", "ext3", "ext4", "ext4_1k", "ext4_64k", "ext4_nojournal", "ext4_inline", "ext4_inline_128", "ext4_names", "ext4_metabg", "ext4_metabg64",
              "ext4_bigalloc", "ext4_flex_off", "ext2_nofiletype", "ext2_nosparse", "ext4_sparse2", "ext4_isize1024", "ext4_featuremix", "ext2_bigdir"]


class Generator(Base):
    def test_every_image_is_clean_under_e2fsck(self):
        """The generator's own check: e2fsck -fn (a reference implementation) finds nothing wrong with any image, so a difference below is the driver's."""
        for name, m in images(*FUNCTIONAL, "ext4_bigdir", "ext4_big", "ext2_big", "fz_ext2", "fz_ext3", "fz_ext4", "fz_ext4_1k", "fz_inline", "fz_metabg").items():
            self.assertEqual(m["fsck_status"], 0, f"{name}:\n{m['fsck_output'][-1500:]}")

    def test_the_crc_of_holes(self):
        """crc32_zeros, which makes the expected crc of a 5 GiB file with holes, against zlib on small cases."""
        for first in (b"", b"abc", os.urandom(1000)):
            for n in (0, 1, 7, 4096, 65536, 100000):
                self.assertEqual(ext4img.crc32_zeros(zlib.crc32(first), n), zlib.crc32(first + bytes(n)), (len(first), n))


class Walk(Base):
    def test_the_walk_equals_the_source_tree(self):
        """Every kind of image: type, mode, uid, gid, size, link count, content crc and link target of every file against the tree mke2fs was given; hard links share an inode."""
        ms = images(*FUNCTIONAL)
        for name in FUNCTIONAL:
            for chunk in (65536, 4096):              # whole-file reads and the small-read window
                with self.subTest(image=name, chunk=chunk):
                    rc, out, err = tool(image_path(name), "walk", chunk=chunk)
                    self.assertEqual(rc, 0, err)
                    assert_matches(self, ms[name], parse_walk(out))

    def test_a_directory_of_twenty_thousand_files(self):
        """One hashed directory read block by block, and every name looked up again (the walk does that for each entry)."""
        m = images("ext4_bigdir")["ext4_bigdir"]
        self.assertGreater(len(m["files"]), 20000)
        import time
        t0 = time.time()
        rc, out, err = tool(image_path("ext4_bigdir"), "walk")
        dt = time.time() - t0
        self.assertEqual(rc, 0, err)
        assert_matches(self, m, parse_walk(out))
        print(f"\n  ext4_bigdir: {len(m['files'])} entries walked, looked up and read in {dt:.1f} s under the sanitizers", file=sys.stderr)
        rc, out, err = tool(image_path("ext4_bigdir"), "ls", "/many")
        self.assertEqual(len(out.decode().splitlines()), 20000)

    def test_the_name_index_of_a_big_directory(self):
        """A directory of 128 KiB or more gets a name index at its first lookup: every name is found in any order (here: the walk, which looks each one up, and a random order), and a name that
        is not there (a prefix, a suffix, one letter different, a longer one, the empty string) is not found. 1 KiB, 4 KiB and 64 KiB blocks."""
        for name, d in (("fz_bigdir", "/many"), ("fz_bigdir_64k", "/many"), ("ext2_bigdir", "/many"), ("ext4_bigdir", "/many")):
            with self.subTest(image=name):
                m = images(name)[name]
                rc, out, err = tool(image_path(name), "walk")
                self.assertEqual(rc, 0, err)
                assert_matches(self, m, parse_walk(out))
                rc, out, err = tool(image_path(name), "lookups", d, 7)
                self.assertEqual(rc, 0, err)
                mm = re.search(r"names=(\d+) found=(\d+)", out.decode())
                self.assertEqual(mm.group(1), mm.group(2), out)
                self.assertGreater(int(mm.group(1)), 5000)
                present = sorted(p for p in m["files"] if p.startswith(d + "/"))[0]
                for bad in (present + "x", present[:-1], present[:-5] + "9.dat", d + "/f", present[:-4] + "DAT", d + "/" + "n" * 255):
                    rc, out, err = tool(image_path(name), "stat", bad)
                    self.assertIn("error", out.decode(), bad)

    def test_a_file_of_five_gib_with_holes(self):
        """Sizes beyond 4 GiB (the high word), extents with gaps, ext2's triple indirect range with 4 KiB blocks; the content crc (zeros are cheap) and a few reads at the edges."""
        for name in ("ext4_big", "ext2_big"):
            with self.subTest(image=name):
                m = images(name)[name]
                rc, out, err = tool(image_path(name), "walk")
                self.assertEqual(rc, 0, err)
                rows = parse_walk(out)
                assert_matches(self, m, rows)
                self.assertEqual(rows["/big5g.bin"]["size"], 5 * GIB + 9000)
                self.assertEqual(rows["/just-over-4g.bin"]["size"], 4 * GIB + 1)
                for off, want in ((0, b"START"), (2 * GIB + 12345, b"MIDDLE"), (5 * GIB, b"END"), (3 * GIB, bytes(8)), (5 * GIB - 8, bytes(8))):
                    rc, out, err = tool(image_path(name), "cat", "/big5g.bin", off, 6 if want != bytes(8) else 8)
                    self.assertEqual(out[:len(want)], want, (name, off))

    def test_sizes_and_times_match_debugfs(self):
        """What debugfs prints for every inode (mode, owner, size, links, blocks, flags and the time stamps with their extra bits) against what the parser decoded."""
        for name in ("ext4", "ext2_1k", "ext4_inline", "ext4_inline_128"):
            with self.subTest(image=name):
                m = images(name)[name]
                paths = [p for p in m["files"]]
                paths = paths[:400]
                dbg = debugfs_batch(image_path(name), [f"stat \"{p}\"" for p in paths])
                for p in paths:
                    text = dbg[f"stat \"{p}\""]
                    rc, out, err = tool(image_path(name), "stat", p)
                    st = dict(line.split("=", 1) for line in out.decode().splitlines())
                    mm = re.search(r"Mode:\s+(\d+)\s+Flags: (0x[0-9a-f]+)", text)
                    self.assertIsNotNone(mm, text)
                    self.assertEqual(int(mm.group(1), 8), int(st["mode"], 8), p)
                    self.assertEqual(int(mm.group(2), 16), int(st["flags"], 16), p)
                    mm = re.search(r"User:\s+(\d+)\s+Group:\s+(\d+).*?Size: (\d+)", text)
                    self.assertEqual((int(mm.group(1)), int(mm.group(2))), (int(st["uid"]), int(st["gid"])), p)
                    if st["kind"] != "d":
                        self.assertEqual(int(mm.group(3)), int(st["size"]), p)
                    mm = re.search(r"Links: (\d+)\s+Blockcount: (\d+)", text)
                    self.assertEqual(int(mm.group(1)), int(st["nlink"]), p)
                    self.assertEqual(int(mm.group(2)), int(st["blocks512"]), p)
                    for key in ("ctime", "atime", "mtime", "crtime"):
                        mm = re.search(rf"{key}: 0x([0-9a-f]+)(?::([0-9a-f]+))?", text)
                        if not mm:
                            continue
                        lo = int(mm.group(1), 16)
                        extra = int(mm.group(2), 16) if mm.group(2) else 0
                        sec = (lo - (1 << 32) if lo >= 1 << 31 else lo) + ((extra & 3) << 32)
                        self.assertEqual(sec, int(st[key]), f"{p} {key}")
                        self.assertEqual(extra >> 2, int(st[key + "_ns"]), f"{p} {key} ns")


class Geometry(Base):
    def test_geometry_and_every_group_against_dumpe2fs(self):
        """The superblock fields and the inode table, free blocks and free inodes of every block group (meta_bg, flex_bg, 32- and 64-byte descriptors, 1 KiB to 64 KiB blocks, bigalloc)."""
        ms = images(*FUNCTIONAL, "ext4_big")
        for name in FUNCTIONAL + ["ext4_big"]:
            with self.subTest(image=name):
                inf = info(image_path(name))
                h = subprocess.run(["dumpe2fs", "-h", str(image_path(name))], capture_output=True, text=True).stdout

                def field(label):
                    m = re.search(rf"^{re.escape(label)}:\s+(.*)$", h, re.M)
                    return m.group(1).strip() if m else None

                self.assertEqual(int(inf["block_size"]), int(field("Block size")))
                self.assertEqual(int(inf["blocks"]), int(field("Block count")))
                self.assertEqual(int(inf["inodes"]), int(field("Inode count")))
                self.assertEqual(int(inf["inode_size"]), int(field("Inode size")))
                self.assertEqual(int(inf["blocks_per_group"]), int(field("Blocks per group")))
                self.assertEqual(int(inf["inodes_per_group"]), int(field("Inodes per group")))
                self.assertEqual(int(inf["first_data_block"]), int(field("First block")))
                self.assertEqual(int(inf["reserved_blocks"]), int(field("Reserved block count")))
                self.assertEqual(int(inf["first_ino"]), int(field("First inode")))
                self.assertEqual(inf["label"], ms[name]["mkfs"][ms[name]["mkfs"].index("-L") + 1])
                self.assertEqual(inf["uuid"], ext4img.UUID)
                # the feature names that e2fsprogs prints are the ones the parser prints (the order differs between the three words, which both group by)
                feats = set(field("Filesystem features").split())
                mine = set(inf["features"].split())
                self.assertEqual(mine - {"uninit_bg"}, feats - {"uninit_bg"})
                self.assertEqual(inf["flavour"], {"ext2_1k": "ext2", "ext3": "ext3", "ext2_nofiletype": "ext2", "ext2_nosparse": "ext2", "ext2_bigdir": "ext2"}.get(name, "ext4"), name)
                g = subprocess.run(["dumpe2fs", str(image_path(name))], capture_output=True, text=True).stdout
                theirs = []
                for sec in re.split(r"^Group \d+:", g, flags=re.M)[1:]:
                    it = re.search(r"Inode table at (\d+)-(\d+)", sec)
                    fr = re.search(r"(\d+) free (?:blocks?|clusters?), (\d+) free inodes", sec)
                    theirs.append((int(it.group(1)), int(fr.group(1)), int(fr.group(2))))
                rc, out, err = tool(image_path(name), "groups")
                self.assertEqual(rc, 0, err)
                mine_g = [tuple(map(int, line.split()))[1:] for line in out.decode().splitlines()]
                self.assertEqual(mine_g, theirs, name)
                self.assertEqual(len(theirs), int(inf["groups"]))

    def test_statfs_is_the_sum_of_the_groups(self):
        """statfs sums the group descriptors; on a clean volume that is what the superblock says too (dumpe2fs prints the superblock's counts)."""
        for name in ("ext4_big", "ext4_nojournal", "ext4_bigalloc"):
            with self.subTest(image=name):
                images(name)
                rc, out, err = tool(image_path(name), "statfs")
                self.assertEqual(rc, 0, err)
                st = dict(kv.split("=") for kv in out.decode().split())
                inf = info(image_path(name))
                h = subprocess.run(["dumpe2fs", "-h", str(image_path(name))], capture_output=True, text=True).stdout
                self.assertEqual(int(st["files"]), int(inf["inodes"]))
                self.assertEqual(int(st["bfree"]), int(re.search(r"^Free blocks:\s+(\d+)", h, re.M).group(1)))
                self.assertEqual(int(st["ffree"]), int(re.search(r"^Free inodes:\s+(\d+)", h, re.M).group(1)))
                self.assertEqual(int(st["bavail"]), int(st["bfree"]) - int(inf["reserved_blocks"]))
                self.assertLessEqual(int(st["bfree"]), int(st["blocks"]))


class Mapping(Base):
    def test_extents_against_debugfs(self):
        """The mapping of a file's blocks (extent leaves, holes) against `debugfs ex`, on files with one extent, with many extents (index and leaf blocks) and with holes."""
        for name, paths in (("ext4", ["/data.bin", "/holes.bin", "/islands.bin", "/text.txt"]), ("ext4_1k", ["/islands.bin", "/data.bin", "/holes.bin"])):
            with self.subTest(image=name):
                images(name)
                dbg = debugfs_batch(image_path(name), [f"ex {p}" for p in paths])
                for p in paths:
                    theirs = []
                    for line in dbg[f"ex {p}"].splitlines():
                        m = re.match(r"\s*(\d+)/\s*(\d+)\s+\d+/\s*\d+\s+(\d+)\s+-\s+(\d+)\s+(\d+)\s+-\s+(\d+)\s+(\d+)\s*(Uninit)?", line)
                        if m and m.group(1) == m.group(2):                 # leaf level
                            theirs.append((int(m.group(3)), int(m.group(5)), int(m.group(7)), "unwritten" if m.group(8) else "mapped"))
                    rc, out, err = tool(image_path(name), "map", p)
                    self.assertEqual(rc, 0, out)
                    mine = []
                    for line in out.decode().splitlines():
                        l, kind, pb, run = line.split()
                        if kind != "hole":
                            mine.append((int(l), int(pb), int(run), kind))
                    self.assertEqual(mine, theirs, f"{name} {p}")
                    self.assertGreater(len(theirs), 0)
        rc, out, err = tool(image_path("ext4"), "map", "/islands.bin")
        self.assertGreater(len(out.decode().splitlines()), 1000, "the file with 1200 islands has an extent tree with index blocks")

    def test_block_maps_against_debugfs(self):
        """ext2's direct, single, double and triple indirect blocks: every mapped block of a dense file and of a sparse one against `debugfs stat` (which prints the ranges)."""
        images("ext2_1k")
        for p in ("/ind-dense.bin", "/ind.bin", "/data.bin", "/holes.bin"):
            text = debugfs_batch(image_path("ext2_1k"), [f"stat {p}"])[f"stat {p}"]
            theirs = {}
            body = text.split("BLOCKS:", 1)[1].split("TOTAL:", 1)[0]
            for item in re.finditer(r"\((\w+|\d+(?:-\d+)?)\):(\d+)(?:-(\d+))?", body):
                lg = item.group(1)
                if not lg[0].isdigit():                                       # (IND), (DIND), (TIND): the map blocks themselves
                    continue
                a, _, b = lg.partition("-")
                a, b = int(a), int(b or a)
                ps = int(item.group(2))
                for k in range(b - a + 1):
                    theirs[a + k] = ps + k
            rc, out, err = tool(image_path("ext2_1k"), "map", p)
            self.assertEqual(rc, 0, out)
            mine = {}
            for line in out.decode().splitlines():
                l, kind, pb, run = line.split()
                if kind == "mapped":
                    for k in range(int(run)):
                        mine[int(l) + k] = int(pb) + k
            self.assertEqual(mine, theirs, p)
        rc, out, err = tool(image_path("ext2_1k"), "map", "/ind.bin")
        starts = [int(line.split()[0]) for line in out.decode().splitlines() if line.split()[1] == "mapped"]
        self.assertGreater(max(starts), 12 + 256 + 256 * 256, "the file reaches the triple indirect range")
        self.assertIn("hole", out.decode())


class Features(Base):
    def test_inline_data(self):
        """Files of 0 to 5000 bytes, small directories, long links: the first 60 bytes in i_block and the rest in system.data; `debugfs` agrees that the inodes are inline."""
        m = images("ext4_inline")["ext4_inline"]
        rc, out, err = tool(image_path("ext4_inline"), "walk")
        self.assertEqual(rc, 0, err)
        rows = parse_walk(out)
        assert_matches(self, m, rows)
        st = {}
        for p in ("/f0059", "/f0100", "/f0150", "/f5000", "/d1", "/d5", "/ln100"):
            rc, out, err = tool(image_path("ext4_inline"), "stat", p)
            st[p] = dict(line.split("=", 1) for line in out.decode().splitlines())
        self.assertEqual(st["/f0059"]["inline"], "1")
        self.assertEqual(st["/f0100"]["inline"], "1")
        self.assertEqual(st["/f5000"]["inline"], "0", "a file that no longer fits is a file with blocks")
        self.assertEqual(st["/d1"]["inline"], "1")
        self.assertEqual(st["/ln100"]["inline"], "1")

    def test_special_files(self):
        """Device nodes (the old and the new encoding of the number), a fifo and a socket: mode, rdev, and that they show in listings with the right type."""
        m = images("ext4")["ext4"]
        rc, out, err = tool(image_path("ext4"), "walk")
        rows = parse_walk(out)
        assert_matches(self, m, rows)
        self.assertEqual(rows["/chardev"]["rdev"], os.makedev(4, 66))
        self.assertEqual(rows["/blkdev"]["rdev"], os.makedev(259, 1))
        self.assertEqual(rows["/chardev2"]["rdev"], os.makedev(300, 5000))
        self.assertEqual(rows["/fifo"]["kind"], "p")
        self.assertEqual(rows["/sock"]["kind"], "s")
        self.assertEqual(rows["/fifo"]["mode"], 0o640)

    def test_symbolic_links_of_every_length(self):
        m = images("ext4", "ext3")["ext4"]
        for name in ("ext4", "ext3", "ext4_inline"):
            rc, out, err = tool(image_path(name), "walk")
            rows = parse_walk(out)
            for n in (1, 10, 58, 59, 60, 61, 100, 255, 1000, 4000):
                if f"/link{n:04d}" in rows:
                    self.assertEqual(len(rows[f"/link{n:04d}"]["tail"]), n, (name, n))
                    self.assertEqual(rows[f"/link{n:04d}"]["size"], n)
        rc, out, err = tool(image_path("ext4"), "stat", "/link0059")
        self.assertIn("fast_link=1", out.decode())
        rc, out, err = tool(image_path("ext4"), "stat", "/link0060")
        self.assertIn("fast_link=0", out.decode(), "60 bytes is the first target that needs a block")

    def test_names_of_255_bytes(self):
        m = images("ext4_names")["ext4_names"]
        rc, out, err = tool(image_path("ext4_names"), "walk")
        rows = parse_walk(out)
        assert_matches(self, m, rows)
        self.assertIn("/names/" + "z" * 255, rows)
        self.assertIn("/names/" + "d" * 255 + "/inside", rows)

    def test_unwritten_extents_read_as_zeros(self):
        """debugfs allocates blocks as uninitialised extents; the volume's old bytes in them must not show through."""
        name = "fz_ext4"
        images(name)
        img = WORK / "unwritten.img"
        shutil.copyfile(image_path(name), img)
        r = subprocess.run(["debugfs", "-w", "-f", "/dev/stdin", str(img)], input="write /dev/null falloc.bin\nfallocate /falloc.bin 0 7\nset_inode_field /falloc.bin size 32768\n",
                           capture_output=True, text=True)
        ex = debugfs_batch(img, ["ex /falloc.bin"])["ex /falloc.bin"]
        m = re.search(r"\s+0\s+-\s+7\s+(\d+)\s+-\s+(\d+)\s+8\s+Uninit", ex)
        self.assertIsNotNone(m, ex + r.stdout + r.stderr)
        first = int(m.group(1))
        bs = int(info(img)["block_size"])
        data = bytearray(img.read_bytes())
        for b in range(first, first + 8):
            data[b * bs:(b + 1) * bs] = b"\xAA" * bs                          # stale bytes where the unwritten extent points
        img.write_bytes(data)
        rc, out, err = tool(img, "cat", "/falloc.bin")
        self.assertEqual(out, bytes(32768))
        rc, out, err = tool(img, "map", "/falloc.bin")
        self.assertIn("unwritten", out.decode())

    def test_an_extent_tree_of_depth_five(self):
        """A hand-made tree with five levels of index blocks (no volume this small has one): the data is found; six levels, a wrong depth in a node and a loop are errors."""
        name = "fz_ext4_1k"
        images(name)
        bs = 1024
        base = bytearray(image_path(name).read_bytes())
        probe = WORK / "probe-src.img"
        shutil.copyfile(image_path(name), probe)
        subprocess.run(["debugfs", "-w", "-f", "/dev/stdin", str(probe)], input="write /dev/null probe.bin\n", capture_output=True, text=True)
        content = bytes(range(256)) * 4
        # a one-block file with content: make it with debugfs from a real file
        src = WORK / "probe.bin"
        src.write_bytes(content)
        subprocess.run(["debugfs", "-w", "-f", "/dev/stdin", str(probe)], input=f"rm /probe.bin\nwrite {src} probe.bin\n", capture_output=True, text=True)
        ex = debugfs_batch(probe, ["ex /probe.bin", "imap /probe.bin"])
        m = re.search(r"\s+0\s+-\s+0\s+(\d+)\s+-\s+(\d+)\s+1", ex["ex /probe.bin"])
        self.assertIsNotNone(m, ex)
        data_blk = int(m.group(1))
        im = re.search(r"located at block (\d+), offset (0x[0-9a-f]+)", ex["imap /probe.bin"])
        self.assertIsNotNone(im, ex)
        inode_off = int(im.group(1)) * bs + int(im.group(2), 16)
        total = len(base) // bs
        spare = list(range(total - 40, total - 40 + 8))
        for b in spare:                                                       # they must be free for this to be a fair test
            self.assertIn("not in use", debugfs_batch(probe, [f"testb {b}"])[f"testb {b}"])

        def node(depth, entries, child_or_start, leaf=False, ee_len=1, max_entries=84):
            n = bytearray(bs)
            n[0:2] = (0xF30A).to_bytes(2, "little")
            n[2:4] = entries.to_bytes(2, "little")
            n[4:6] = max_entries.to_bytes(2, "little")
            n[6:8] = depth.to_bytes(2, "little")
            if entries:
                if leaf:
                    n[12:16] = (0).to_bytes(4, "little")
                    n[16:18] = ee_len.to_bytes(2, "little")
                    n[18:20] = (child_or_start >> 32).to_bytes(2, "little")
                    n[20:24] = (child_or_start & 0xFFFFFFFF).to_bytes(4, "little")
                else:
                    n[12:16] = (0).to_bytes(4, "little")
                    n[16:20] = (child_or_start & 0xFFFFFFFF).to_bytes(4, "little")
                    n[20:22] = (child_or_start >> 32).to_bytes(2, "little")
            return n

        def build(levels, tweak=None):
            """An image whose probe.bin has an extent tree with `levels` index levels above the leaf (the root in the inode counts as one)."""
            img = bytearray(probe.read_bytes())
            blocks = spare[:levels]                                          # blocks[0] holds depth levels-1, ..., blocks[levels-1] is the leaf
            root = bytearray(60)
            root[0:2] = (0xF30A).to_bytes(2, "little")
            root[2:4] = (1).to_bytes(2, "little")
            root[4:6] = (4).to_bytes(2, "little")
            root[6:8] = levels.to_bytes(2, "little")
            root[12:16] = bytes(4)
            root[16:20] = blocks[0].to_bytes(4, "little")
            for i, b in enumerate(blocks):
                depth = levels - 1 - i
                if depth == 0:
                    n = node(0, 1, data_blk, leaf=True)
                else:
                    n = node(depth, 1, blocks[i + 1])
                if tweak:
                    n = tweak(i, depth, n) or n
                img[b * bs:(b + 1) * bs] = n
            img[inode_off + 0x28:inode_off + 0x28 + 60] = root
            return img

        for levels in (1, 2, 3, 4, 5):
            p = WORK / f"depth{levels}.img"
            p.write_bytes(build(levels))
            rc, out, err = tool(p, "cat", "/probe.bin")
            self.assertEqual(out, content, f"depth {levels}")
        p = WORK / "depth6.img"
        p.write_bytes(build(6))
        rc, out, err = tool(p, "cat", "/probe.bin")
        self.assertNotEqual(out, content, "six levels is more than the format allows")
        self.assertNotEqual(rc, 0)
        p = WORK / "depth-wrong.img"
        p.write_bytes(build(4, tweak=lambda i, depth, n: n.__setitem__(slice(6, 8), (depth + 1).to_bytes(2, "little")) if i == 2 else None))
        rc, out, err = tool(p, "cat", "/probe.bin")
        self.assertNotEqual(rc, 0, "a node must say one level less than its parent")
        p = WORK / "loop.img"
        img = build(3)
        loop_blk = spare[1]                                                    # the second node points back at the first
        img[loop_blk * bs + 16:loop_blk * bs + 20] = spare[0].to_bytes(4, "little")
        p.write_bytes(img)
        rc, out, err = tool(p, "cat", "/probe.bin")
        self.assertNotEqual(rc, 0, "a loop in the tree ends in an error")


class Hostile(Base):
    """Damage made by hand to a small volume. Each case must end in an error or a partial listing, never in a crash, a hang or a sanitizer report (tool() fails on those)."""

    def damaged(self, name, mutate):
        images("fz_ext4")
        data = bytearray(image_path("fz_ext4").read_bytes())
        mutate(data)
        p = WORK / f"{name}.img"
        p.write_bytes(data)
        return p

    def test_superblock_fields(self):
        sb = 1024
        cases = {
            "magic": (0x38, b"\x00\x00"),
            "block-size-7": (0x18, (7).to_bytes(4, "little")),
            "block-size-huge": (0x18, (0xFFFFFFFF).to_bytes(4, "little")),
            "blocks-huge": (0x04, (0xFFFFFFFF).to_bytes(4, "little")),
            "blocks-zero": (0x04, bytes(4)),
            "inodes-zero": (0x00, bytes(4)),
            "inodes-huge": (0x00, (0xFFFFFFFF).to_bytes(4, "little")),
            "ipg-zero": (0x28, bytes(4)),
            "ipg-huge": (0x28, (0xFFFFFFFF).to_bytes(4, "little")),
            "bpg-zero": (0x20, bytes(4)),
            "bpg-huge": (0x20, (0xFFFFFFFF).to_bytes(4, "little")),
            "inode-size-0": (0x58, bytes(2)),
            "inode-size-100": (0x58, (100).to_bytes(2, "little")),
            "inode-size-3000": (0x58, (3000).to_bytes(2, "little")),
            "inode-size-65535": (0x58, (65535).to_bytes(2, "little")),
            "first-ino-1": (0x54, (1).to_bytes(4, "little")),
            "revision-2": (0x4C, (2).to_bytes(4, "little")),
            "incompat-unknown": (0x60, (0x80000000 | 0x2 | 0x40 | 0x200 | 0x80).to_bytes(4, "little")),
            "incompat-compression": (0x60, (0x1 | 0x2 | 0x40 | 0x200 | 0x80).to_bytes(4, "little")),
            "incompat-journal-dev": (0x60, (0x8 | 0x2).to_bytes(4, "little")),
            "desc-size-0": (0xFE, bytes(2)),
            "desc-size-3": (0xFE, (3).to_bytes(2, "little")),
            "desc-size-60000": (0xFE, (60000).to_bytes(2, "little")),
            "first-data-block": (0x14, (5).to_bytes(4, "little")),
            "log-cluster": (0x1C, (30).to_bytes(4, "little")),
            "ro-bigalloc": (0x64, (0x200).to_bytes(4, "little")),
        }
        for name, (off, val) in cases.items():
            with self.subTest(case=name):
                p = self.damaged(f"sb-{name}", lambda d: d.__setitem__(slice(sb + off, sb + off + len(val)), val))
                rc, out, err = tool(p, "walk")
                self.assertIn(rc, (0, 1), (out, err))

    def test_unusable_volumes_are_refused_with_a_reason(self):
        for name, mut, text in (("magic", lambda d: d.__setitem__(slice(1024 + 0x38, 1024 + 0x3A), b"\0\0"), "0xEF53"),
                                ("unknown", lambda d: d.__setitem__(slice(1024 + 0x60, 1024 + 0x64), (0x80000000 | 0x2 | 0x40 | 0x200 | 0x80).to_bytes(4, "little")), "incompatible"),
                                ("journal-dev", lambda d: d.__setitem__(slice(1024 + 0x60, 1024 + 0x64), (0x8).to_bytes(4, "little")), "journal_dev")):
            p = self.damaged(f"refuse-{name}", mut)
            rc, out, err = tool(p, "info")
            self.assertEqual(rc, 1)
            self.assertIn("open failed", out.decode())
            self.assertIn(text, out.decode(), name)

    def test_a_volume_cut_short_is_refused(self):
        p = self.damaged("short", lambda d: None)
        with open(p, "r+b") as f:
            f.truncate(p.stat().st_size // 2)
        rc, out, err = tool(p, "info")
        self.assertEqual(rc, 1)
        self.assertIn("holds only", out.decode())

    def test_group_descriptors_and_inodes(self):
        images("fz_ext4")
        inf = info(image_path("fz_ext4"))
        bs = int(inf["block_size"])
        rc, out, err = tool(image_path("fz_ext4"), "groups")
        itable = int(out.decode().split()[1])
        for name, off, val in (("itable-zero", 0x08, bytes(4)), ("itable-huge", 0x08, (0xFFFFFFF0).to_bytes(4, "little")), ("itable-hi", 0x28, (0x7FFF).to_bytes(4, "little"))):
            with self.subTest(case=name):
                gdt = bs * (int(inf["first_data_block"]) + 1)
                p = self.damaged(f"gd-{name}", lambda d: d.__setitem__(slice(gdt + off, gdt + off + len(val)), val))
                rc, out, err = tool(p, "walk")
                self.assertIn(rc, (0, 1))
        # inode fields of one file and of the root directory
        rc, out, err = tool(image_path("fz_ext4"), "stat", "/data.bin")
        ino = int(dict(line.split("=") for line in out.decode().splitlines())["ino"])
        ipg = int(inf["inodes_per_group"])
        isz = int(inf["inode_size"])
        base = (itable * bs) + ((ino - 1) % ipg) * isz
        root = (itable * bs) + 1 * isz
        for name, off, val in (("mode-0", 0, bytes(2)), ("mode-dir", 0, (0x41ED).to_bytes(2, "little")), ("size-huge", 4, (0xFFFFFFFF).to_bytes(4, "little")),
                               ("size-hi", 0x6C, (0xFFFFFFFF).to_bytes(4, "little")), ("flags-inline", 0x20, (0x10000000).to_bytes(4, "little")),
                               ("flags-both", 0x20, (0x10080000).to_bytes(4, "little")), ("flags-blockmap", 0x20, bytes(4)), ("extra-isize", 0x80, (0xFFFF).to_bytes(2, "little")),
                               ("extra-isize-odd", 0x80, (7).to_bytes(2, "little")), ("iblock-garbage", 0x28, bytes(range(60))), ("iblock-depth", 0x2E, (9).to_bytes(2, "little")),
                               ("iblock-entries", 0x2A, (60000).to_bytes(2, "little")), ("iblock-len-0", 0x3C, bytes(2)), ("iblock-start-huge", 0x40, (0xFFFFFFFF).to_bytes(4, "little"))):
            for where, tag in ((base, "file"), (root, "root")):
                with self.subTest(case=f"{tag}-{name}"):
                    p = self.damaged(f"ino-{tag}-{name}", lambda d: d.__setitem__(slice(where + off, where + off + len(val)), val))
                    rc, out, err = tool(p, "walk")
                    self.assertIn(rc, (0, 1))
                    rc, out, err = tool(p, "cat", "/data.bin")
                    self.assertIn(rc, (0, 1))

    def test_directory_blocks(self):
        images("fz_ext4")
        inf = info(image_path("fz_ext4"))
        bs = int(inf["block_size"])
        rc, out, err = tool(image_path("fz_ext4"), "map", "/dir")
        rows = [line.split() for line in out.decode().splitlines()]
        blk = int(rows[0][2])
        for name, off, val in (("rec-len-0", 4, bytes(2)), ("rec-len-3", 4, (3).to_bytes(2, "little")), ("rec-len-huge", 4, (0xFFFF).to_bytes(2, "little")),
                               ("rec-len-small", 4, (8).to_bytes(2, "little")), ("name-len-255", 6, bytes([255])), ("name-len-0", 6, bytes([0])), ("inode-huge", 0, (0xFFFFFFFF).to_bytes(4, "little")),
                               ("inode-1", 0, (1).to_bytes(4, "little")), ("inode-8", 0, (8).to_bytes(4, "little")), ("second-rec-len", 16, (0xFFFC).to_bytes(2, "little")),
                               ("slash-in-name", 24, b"/"), ("nul-in-name", 24, b"\0")):
            with self.subTest(case=name):
                p = self.damaged(f"dir-{name}", lambda d: d.__setitem__(slice(blk * bs + off, blk * bs + off + len(val)), val))
                rc, out, err = tool(p, "walk")
                self.assertIn(rc, (0, 1))
                rc, out, err = tool(p, "ls", "/dir")
                self.assertIn(rc, (0, 1))

    def test_a_directory_block_of_zeros_and_a_cycle(self):
        images("fz_ext4")
        inf = info(image_path("fz_ext4"))
        bs = int(inf["block_size"])
        rc, out, err = tool(image_path("fz_ext4"), "map", "/dir")
        blk = int(out.decode().splitlines()[0].split()[2])
        p = self.damaged("dir-zeros", lambda d: d.__setitem__(slice(blk * bs, (blk + 1) * bs), bytes(bs)))
        rc, out, err = tool(p, "walk")
        self.assertEqual(rc, 1, "a block of zeros is a directory block that cannot be what it claims")
        rc, out, err = tool(image_path("fz_ext4"), "stat", "/dir")
        dino = int(dict(line.split("=") for line in out.decode().splitlines())["ino"])
        # an entry of /dir that names the root directory: the walk follows it down to its depth limit and stops
        p = self.damaged("dir-cycle", lambda d: d.__setitem__(slice(blk * bs, blk * bs + 4), (2).to_bytes(4, "little")))
        rc, out, err = tool(p, "walk")
        self.assertIn(rc, (0, 1))

    def test_symbolic_links(self):
        images("fz_ext4")
        inf = info(image_path("fz_ext4"))
        bs = int(inf["block_size"])
        rc, out, err = tool(image_path("fz_ext4"), "groups")
        itable = int(out.decode().split()[1])
        isz = int(inf["inode_size"])
        ipg = int(inf["inodes_per_group"])
        for path in ("/link0059", "/link4000", "/rel-link"):
            rc, out, err = tool(image_path("fz_ext4"), "stat", path)
            ino = int(dict(line.split("=") for line in out.decode().splitlines())["ino"])
            base = itable * bs + ((ino - 1) % ipg) * isz
            for name, off, val in (("size-0", 4, bytes(4)), ("size-5000", 4, (5000).to_bytes(4, "little")), ("size-huge", 4, (0xFFFFFFF0).to_bytes(4, "little")),
                                   ("nul", 0x2C, b"\0"), ("blocks", 0x1C, (1000).to_bytes(4, "little"))):
                with self.subTest(case=f"{path}-{name}"):
                    p = self.damaged(f"ln-{path[1:]}-{name}", lambda d: d.__setitem__(slice(base + off, base + off + len(val)), val))
                    rc, out, err = tool(p, "readlink", path)
                    self.assertIn(rc, (0, 1))
                    rc, out, err = tool(p, "walk")
                    self.assertIn(rc, (0, 1))


class Fuzz(Base):
    """Thousands of random mutations of whole volumes: bit flips, bytes and fields set to extremes, copies of one place over another and (rarely) truncation, three quarters of them in
    the superblock, the group descriptors and the blocks the walk of the intact volume read as inode tables, directories, extent trees and indirect blocks. Each damaged volume is
    opened and walked (every file read, every name looked up, directories read from positions that are not entry boundaries) under AddressSanitizer and UBSan, with a time limit."""

    def run_fuzz(self, name, seed, cases, mutations):
        images(name)
        rc, out, err = tool(image_path(name), "fuzz", seed, cases, mutations, timeout=3600)
        self.assertEqual(rc, 0, err)
        text = out.decode()
        m = re.search(r"fuzz: (\d+) iterations, (\d+) mounted, (\d+) refused", text)
        self.assertIsNotNone(m, text)
        self.assertEqual(int(m.group(1)), cases)
        print(f"\n  {name}: {text.strip()}", file=sys.stderr)
        return text

    def test_fuzz(self):
        n = int(os.environ.get("TOUHOU_EXT4_FUZZ", "1500"))
        for k, name in enumerate(("fz_ext4", "fz_ext4_1k", "fz_ext2", "fz_ext3", "fz_inline", "fz_metabg", "fz_bigdir", "fz_bigdir_64k", "fz_nofiletype", "ext2_bigdir")):
            with self.subTest(image=name):
                self.run_fuzz(name, 1000 + k, n, 4)
        with self.subTest(image="fz_ext4 with many mutations"):
            self.run_fuzz("fz_ext4", 99, max(n // 3, 100), 40)


class Source(unittest.TestCase):
    def test_the_source_has_no_way_to_write(self):
        """Read only for ever: the parser and the glue mention no function that writes to a device or a file."""
        banned = [r"\bblk_write\b", r"\bblk_flush\b", r"\bblk_write_as\b", r"\bfwrite\b", r"\.write\s*=", r"->write\b", r"\bpwrite\b", r"\bmemcpy\s*\(\s*v->dev"]
        for name in ("ext4_core.c", "ext4_priv.h", "ext4.c"):
            p = ROOT / "kernel" / "fs" / name
            if not p.exists():
                continue
            text = re.sub(r"/\*.*?\*/", "", p.read_text(), flags=re.S)
            text = re.sub(r"//.*", "", text)
            for pat in banned:
                for m in re.finditer(pat, text):
                    line = text[:m.start()].count("\n") + 1
                    ctx = text.splitlines()[line - 1]
                    # the glue's refusals are the only mention of a write method: they all return EROFS
                    self.assertTrue(name == "ext4.c" and "ext4_no" in ctx, f"{name}:{line}: {ctx.strip()}")


if __name__ == "__main__":
    unittest.main()
