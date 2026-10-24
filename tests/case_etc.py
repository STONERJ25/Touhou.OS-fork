"""Keine's records: /etc kept on the HUMANVILLAGE volume (docs/PERSISTENT_ETC.md).

Each test builds a disk image with tools/mkdisk.py (see case_storage.py), changes something in the guest, powers off, boots the
same image again and looks whether the change is still there; or it puts files into /.system/etc on the image from the host and
checks what init makes of them. After the guest is gone the image is checked with mkdisk's independent FAT checker.

Run just these with:  python tests/run_all.py etc_,accounts_persist
"""
import contextlib
import re

import mkdisk
from case_storage import Image, Guest, hv_part, MIB
from harness import test, Qemu

KEPT = "/HumanVillage/.system/etc"
KLOG_LINE = re.compile(r"^\[[A-Z][A-Za-z]+\] .*(\n|$)", re.M)


@contextlib.contextmanager
def boot(img, user="root", host="gensokyo", cmdline="", name="t-etc", login=False):
    """Boots the image in text mode: a root shell (user=root), reimu's shell, or the login prompt (login=True)."""
    cl = "text bootlog=off " + cmdline
    if login:
        cl += " login"
    elif user == "root":
        cl += " user=root"
    with Qemu(cmdline=cl.strip(), disk=img.path if img else None, name=name, mem="512M") as vm:
        g = Guest(vm, user == "root")
        if not login:
            vm.expect(rf"{user}@{host} \S+ [#$] ", timeout=90)
        g.prompt = rf"{user}@\S+ \S+ [#$] "                 # the host name may change during the test
        yield g, vm


def say(g, cmd, timeout=60):
    """What a command printed, without kernel log lines the console may interleave with it."""
    return KLOG_LINE.sub("", g.ok(cmd, timeout)).strip()


def read_kept(img, name):
    vol = img.volume(1)
    try:
        return vol.read_file(f"/.system/etc/{name}")
    except mkdisk.MkdiskError:
        return None
    finally:
        vol.close()


def hv_image(name, files=None):
    return Image(name, 64 * MIB, [hv_part(files)])


@test
def accounts_persist_password():
    """reimu sets a password with passwd; after a restart the login prompt wants exactly that password."""
    with hv_image("etc-password") as img:
        with boot(img, user="reimu", name="t-etc-pw1") as (g, vm):
            vm.send("passwd\n")
            vm.expect(r"New password: ", timeout=40)
            vm.send("kagura\n")
            vm.expect(r"Retype new password: ", timeout=30)
            vm.send("kagura\n")
            out = vm.expect(r"password updated for reimu(.*?)" + g.prompt, timeout=60).group(1)
            assert "lasts only until" not in out, out
            g.poweroff()                                   # reimu may not power off: sync, then pull the plug
        img.check()
        shadow = read_kept(img, "shadow")
        assert shadow and re.search(rb"^reimu:\$gap\$[^:]+:", shadow, re.M), shadow
        assert b"kagura" not in shadow

        with boot(img, login=True, name="t-etc-pw2") as (g, vm):
            vm.expect(r"/etc restored from the Human Village's records \(/HumanVillage/\.system/etc\): shadow", timeout=90)
            vm.expect(r"gensokyo login: ", timeout=90)
            vm.send("reimu\n")
            vm.expect(r"Password: ", timeout=40)
            vm.send("\n")                                  # the old empty password no longer works
            vm.expect(r"Login incorrect", timeout=40)
            vm.expect(r"gensokyo login: ", timeout=40)
            vm.send("reimu\n")
            vm.expect(r"Password: ", timeout=40)
            vm.send("kagura\n")
            g.prompt = r"reimu@gensokyo \S+ \$ "
            vm.expect(g.prompt, timeout=40)
            assert say(g, "whoami") == "reimu"
            out = say(g, "ls -l /etc/shadow")
            assert out.startswith("-rw-------") and " root " in out, out


@test
def accounts_persist_adduser():
    """adduser on the volume: the account, its group, its seat and its own home are all still there after a restart."""
    with hv_image("etc-adduser") as img:
        with boot(img, name="t-etc-add1") as (g, vm):
            assert "youmu joins the Human Village (uid 1004)" in say(g, "adduser -L -c 'Youmu Konpaku' youmu")
            out = say(g, "ls -ld /HumanVillage/youmu")
            assert re.search(r"^drwx------ .* youmu +youmu ", out), out    # hers at once, not root's
            g.poweroff()
        img.check()
        assert b"youmu:x:1004:1004:Youmu Konpaku:/HumanVillage/youmu:/bin/gapsh" in read_kept(img, "passwd")
        assert b"youmu:!:" in read_kept(img, "shadow")

        with boot(img, name="t-etc-add2") as (g, vm):
            assert "youmu:x:1004:1004:Youmu Konpaku" in say(g, "grep ^youmu /etc/passwd")
            assert "youmu" in say(g, "grep ^shrine /etc/group")
            assert "youmu:!:" in say(g, "grep ^youmu /etc/shadow")
            out = say(g, "ls -ld /HumanVillage/youmu")
            assert re.search(r"^drwx------ .* youmu +youmu ", out), out
            assert "uid=1004(youmu)" in say(g, "su youmu -c id")
            assert "youmu has left the Human Village" in say(g, "deluser youmu")
            g.poweroff()
        img.check()
        assert b"youmu" not in read_kept(img, "passwd") and b"youmu" not in read_kept(img, "group")


@test
def etc_timezone_hostname():
    """timezone and hostname set by root are kept: after a restart the clock shows JST and the prompt the new name."""
    with hv_image("etc-tz") as img:
        with boot(img, name="t-etc-tz1") as (g, vm):
            assert "the shrine now keeps JST-9 time" in say(g, "timezone JST-9")
            assert say(g, "hostname hakugyokurou") == ""
            assert say(g, "date +%Z") == "JST"
            assert say(g, "hostname") == "hakugyokurou"
            assert "JST-9" in say(g, "su reimu -c timezone")
            g.fails("su reimu -c 'timezone UTC'", "only root may set the time zone")
            g.poweroff()
        img.check()
        assert read_kept(img, "timezone") == b"JST-9\n"
        assert read_kept(img, "hostname") == b"hakugyokurou\n"

        with boot(img, host="hakugyokurou", name="t-etc-tz2") as (g, vm):
            assert say(g, "cat /etc/timezone") == "JST-9"
            assert say(g, "date +%Z") == "JST"
            assert say(g, "uname -n") == "hakugyokurou"
            assert "JST-9 (JST, UTC+09:00)" in say(g, "timezone")
            out = say(g, "etckeep")
            assert re.search(r"timezone +kept on the volume, same as /etc", out), out
            assert re.search(r"passwd +not kept", out), out
            # a time zone the libc cannot read is refused instead of quietly meaning UTC
            g.fails("timezone Asia/Tokyo", "cannot understand")
            g.poweroff()


@test
def etc_corrupt_fallback():
    """Damaged files on the volume fall back to the initrd default, one Keine line each; good ones are still used."""
    files = {
        "/.system/etc/passwd": b"root:x:0:0:Yukari Yakumo:/root:/bin/gapsh\nreimu:x:1000\n",          # a line cut short
        "/.system/etc/shadow": b"root:!:19000:0:99999:7:::\nreimu:$gap$abc$1234",                   # no newline at the end
        "/.system/etc/hostname": b"bad host name\n",
        "/.system/etc/network.conf": b"eth0 static 10.0.2.15/24 gateway 192.168.9.1\n",            # gateway outside the network
        "/.system/etc/update.conf": b"url=http://10.0.2.2:8080/\nkey=00112233aabbccdd\n",
        "/.system/etc/timezone": b"EST5EDT,M3.2.0,M11.1.0\n",
    }
    with hv_image("etc-corrupt", files) as img:
        with boot(img, name="t-etc-corrupt") as (g, vm):
            log = vm.text()
            for name, why in (("passwd", r"line 2: 3 fields, a passwd line has 7"), ("shadow", r"the last line is cut short"),
                              ("hostname", r"line 1: a host name has only letters"), ("network.conf", r"line 1: the gateway 192\.168\.9\.1")):
                rx = rf"\[Keine\] {re.escape(KEPT)}/{re.escape(name)} is damaged \({why}.*?\): Keine falls back to the initrd default"
                assert len(re.findall(rx, log)) == 1, f"expected one fallback line for {name}:\n" + log[-3000:]
            assert re.search(r"/etc restored from the Human Village's records \(/HumanVillage/\.system/etc\): timezone update\.conf", log), log[-3000:]
            assert "reimu:x:1000:1000:Reimu Hakurei" in say(g, "grep ^reimu /etc/passwd")
            assert "reimu::" in say(g, "grep ^reimu /etc/shadow")
            assert say(g, "hostname") == "gensokyo"
            assert say(g, "cat /etc/timezone") == "EST5EDT,M3.2.0,M11.1.0"
            assert "key=00112233aabbccdd" in say(g, "cat /etc/update.conf")
            assert "dhcp" in say(g, "cat /etc/network.conf")                   # the initrd's comments
            out = say(g, "etckeep check passwd update.conf; true")
            assert re.search(r"/HumanVillage/\.system/etc/passwd: damaged: line 2", out), out
            assert "/etc/passwd: fine" in out and "/HumanVillage/.system/etc/update.conf: fine" in out, out
            # a damaged text is refused by the tools before it reaches /etc or the volume
            g.fails("hostname 'not ok'", "not a host name")
            g.fails("timezone '9JST'", "")
            g.poweroff()


@test
def etc_shadow_permissions():
    """The kept shadow file is root's: a resident can neither list .system nor read the file, nor read the raw partition."""
    files = {"/.system/etc/shadow": b"root:!:19000:0:99999:7:::\nreimu::19000:0:99999:7:::\nmarisa::19000:0:99999:7:::\n"
                                    b"sakuya::19000:0:99999:7:::\nsumireko::19000:0:99999:7:::\n"}
    with hv_image("etc-perm", files) as img:
        with boot(img, user="reimu", name="t-etc-perm") as (g, vm):
            out = say(g, "ls -ld /HumanVillage/.system")
            assert re.search(r"^drwx------ .* root +root ", out), out
            g.fails(f"cat {KEPT}/shadow", "Permission denied")
            g.fails(f"ls {KEPT}", "Permission denied")
            g.fails("head -c 512 /dev/vda1 > /dev/null", "Permission denied")
            g.fails("cat /etc/shadow", "Permission denied")
            assert "(only root can look)" in say(g, "etckeep")
            # root (through marisa's sudo) sees the file, mode 0600 and root's
            out = say(g, f"su marisa -c 'sudo ls -l {KEPT}/shadow'")
            assert out.startswith("-rw-------") and " root " in out, out
            g.poweroff()


@test
def etc_nohumanvillage():
    """nohumanvillage: the kept files are ignored and changes stay in RAM; the disk is not touched."""
    files = {"/.system/etc/hostname": b"hakugyokurou\n", "/.system/etc/timezone": b"JST-9\n"}
    with hv_image("etc-novolume", files) as img:
        before = img.sha()
        with boot(img, cmdline="nohumanvillage", name="t-etc-novol") as (g, vm):
            assert "/etc restored" not in vm.text() and "no records kept" not in vm.text()
            assert say(g, "hostname") == "gensokyo"
            assert say(g, "cat /etc/timezone") == "UTC"
            assert "No HUMANVILLAGE volume is mounted" in say(g, "etckeep")
            out = say(g, "timezone JST-9")
            assert "the shrine now keeps JST-9 time" in out and "lasts only" not in out, out
            assert say(g, "date +%Z") == "JST"
            g.poweroff()
        assert img.sha() == before, "the disk changed although nohumanvillage was given"
