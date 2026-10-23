"""Accounts: setuid exec, /etc/shadow, su, passwd and eiki (sudo), driven from reimu's text shell."""
import test_shell
from harness import test, Qemu

PROMPT = test_shell.PROMPT

# (command, substring that must appear, expected exit status or None)
CHECKS = [
    ("ls -l /bin/eiki /bin/su /bin/passwd", "-rwsr-xr-x", 0),
    ("ls -l /bin/sudo", "eiki", 0),
    ("ls -l /Hakurei/etc/shadow", "-rw-------", 0),
    ("cat /Hakurei/etc/shadow", "Permission denied", 1),
    ("sudo id", "reimu is not in the sudoers file. This incident will be reported to Eiki.", 1),
    ("dmesg | grep Eiki", "reimu tried to borrow root's power and was refused", 0),
    ("su marisa -c id", "uid=1001(marisa) gid=1001(marisa) groups=1001(marisa),100(shrine)", 0),
    ("su marisa -c 'sudo id'", "uid=0(root) gid=0(root)", 0),
    ("su marisa -c 'sudo cat /Hakurei/etc/shadow'", "root:!:", 0),
    ("su marisa -c 'whoami; echo $HOME; echo $SUDO_USER'", "marisa\n/HumanVillage/marisa", 0),
    ("su nobodyhome -c id", "does not exist", 1),
    ("su sumireko -c id", "uid=1003(sumireko) gid=1003(sumireko) groups=1003(sumireko)", 0),
    ("su sumireko -c 'cat /dev/fb0'", "Permission denied", None),             # no seat at the shrine: no display
    ("echo mine > /tmp/sticky.txt; su marisa -c 'rm /tmp/sticky.txt'", "not permitted", None),   # /tmp is sticky
    ("ls /tmp/sticky.txt && rm /tmp/sticky.txt", "/tmp/sticky.txt", 0),            # the owner may remove it
    ("passwd sakuya", "you may only change your own password", 1),
    ("sudo -u sakuya id", "is not in the sudoers file", 1),
]


@test
def accounts():
    """setuid binaries, shadow permissions, themed sudoers refusal, su/sudo for marisa."""
    failures = []
    with Qemu(cmdline="text", name="t-accounts") as vm:
        vm.expect(PROMPT, timeout=90)
        for cmd, expect, status in CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                if status is not None:
                    assert rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-700:]}")
    assert not failures, f"{len(failures)} account check(s) failed:\n" + "\n".join(failures)


ADMIN_CHECKS = [
    ("su marisa -c 'sudo adduser -L -c \"Patchouli Knowledge\" patchouli'", "patchouli joins the Human Village (uid 1004)", 0),
    ("grep ^patchouli /etc/passwd", "patchouli:x:1004:1004:Patchouli Knowledge:/HumanVillage/patchouli:/bin/gapsh", 0),
    ("grep ^shrine /etc/group", "patchouli", 0),                                   # a new resident gets a seat
    ("ls -ld /HumanVillage/patchouli", "patchouli patchouli", 0),
    ("su marisa -c 'sudo grep ^patchouli /etc/shadow'", "patchouli:!:", 0),         # created locked
    ("su marisa -c 'sudo -u patchouli id'", "uid=1004(patchouli) gid=1004(patchouli) groups=1004(patchouli),100(shrine)", 0),
    ("adduser nobody-here", "only root", 1),
    ("su marisa -c 'sudo adduser -L patchouli'", "already lives here", 1),
    ("su marisa -c 'sudo adduser -L Bad_Name'", "not a good name", 1),
    ("su marisa -c 'sudo deluser --remove-home patchouli'", "patchouli has left the Human Village (home removed)", 0),
    ("ls /HumanVillage", "sakuya", 0),
    ("su marisa -c 'sudo deluser root'", "part of the shrine itself", 1),
]


@test
def accounts_admin():
    """adduser creates passwd/group/shadow entries, a home and a seat; deluser takes them away again (root only)."""
    failures = []
    with Qemu(cmdline="text", name="t-accounts-admin") as vm:
        vm.expect(PROMPT, timeout=90)
        for cmd, expect, status in ADMIN_CHECKS:
            try:
                out, rc = test_shell.run(vm, cmd)
                assert expect in out, f"expected {expect!r} in output:\n{out}"
                if status is not None:
                    assert rc == status, f"exit status {rc}, expected {status}; output:\n{out}"
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-700:]}")
        out, _ = test_shell.run(vm, "su marisa -c 'sudo grep -c patchouli /etc/passwd /etc/group /etc/shadow'; true")
        assert out.count(":0") == 3, f"patchouli should be gone from all three files:\n{out}"
    assert not failures, f"{len(failures)} admin check(s) failed:\n" + "\n".join(failures)


@test
def accounts_passwords():
    """passwd hashes into /etc/shadow, getpass does not echo, su refuses a wrong password and accepts the right one."""
    with Qemu(cmdline="text", name="t-accounts-pw") as vm:
        vm.expect(PROMPT, timeout=90)

        vm.send("passwd\n")
        vm.expect(r"New password: ", timeout=40)
        vm.send("secret\n")
        vm.expect(r"Retype new password: ", timeout=30)
        vm.send("secret\n")
        vm.expect(r"password updated for reimu", timeout=40)
        vm.expect(PROMPT, timeout=30)

        # with a password set, passwd wants the current one first; a mismatch then leaves the shadow file alone
        vm.send("passwd\n")
        vm.expect(r"Current password: ", timeout=40)
        vm.send("secret\n")
        vm.expect(r"New password: ", timeout=40)
        vm.send("one\n")
        vm.expect(r"Retype new password: ", timeout=30)
        vm.send("two\n")
        vm.expect(r"do not match", timeout=30)
        vm.expect(PROMPT, timeout=30)

        # reimu now has a password, so marisa has to know it to become her
        vm.send("su marisa -c 'su reimu -c id'\n")
        vm.expect(r"Password: ", timeout=40)
        vm.send("wrong\n")
        vm.expect(r"Authentication failure", timeout=40)
        vm.expect(r"Password: ", timeout=40)
        vm.send("secret\n")
        vm.expect(r"uid=1000\(reimu\)", timeout=40)
        vm.expect(PROMPT, timeout=30)

        # the stored value is a salted hash, never the plain text
        out, _ = test_shell.run(vm, "su marisa -c 'sudo cat /Hakurei/etc/shadow' | grep ^reimu")
        assert "reimu:$gap$" in out, out
        assert "secret" not in out, out
        assert "secret" not in vm.text(), "getpass must not echo what was typed"
