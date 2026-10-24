# Accounts, login and privilege

Gensokyo is a multi-user system: every process has an owner, files have owners and permission bits, and some programs
(`su`, `sudo`, `passwd`) borrow root's authority through the setuid bit. This page covers who lives here, how they get
in, and what stops them stepping on each other.

## Who lives here

| name       | uid  | what they are                                                        |
|------------|------|----------------------------------------------------------------------|
| `root`     | 0    | Yukari Yakumo, the boundary itself. Locked: nobody logs in as root.  |
| `reimu`    | 1000 | Reimu Hakurei, the default resident (autologin, no password)         |
| `marisa`   | 1001 | Marisa Kirisame; in `/etc/sudoers`: she borrows everything anyway    |
| `sakuya`   | 1002 | Sakuya Izayoi                                                        |
| `sumireko` | 1003 | Sumireko Usami, an outsider: no seat at the shrine (see below)       |

Each resident has a home in `/HumanVillage/<name>` (also reachable as `/home/<name>`) and a group of the same name.
Group `shrine` (gid 100) is the *seat* group: members may use the display and input devices (so they can run the
desktop) and may restart or power off the machine. Everyone except Sumireko is a member.

The default accounts have **empty passwords** so the system boots straight into a usable desktop; set real ones with
`passwd` (see below). Accounts are not persistent yet: `/etc` lives in the in-memory root filesystem and is rebuilt from the
initrd on every boot, so password changes last until the next restart. (The home folders do persist when a HUMANVILLAGE volume is
mounted, see [STORAGE.md](STORAGE.md).)

## The files

* `/etc/passwd` -- `name:x:uid:gid:full name:home:shell`
* `/etc/group` -- `name:x:gid:member,member`
* `/etc/shadow` -- `name:hash:...`, owned by root, mode 0600. The hash is `$gap$<salt>$<sha256 hex>`, computed as
  SHA-256 over salt and password. An **empty** hash means "no password needed", `!` means the account is locked.
  This is a single salted SHA-256: enough to keep the residents out of each other's accounts, not a defence against
  an attacker with a copy of the file and time. It does not pretend to be more.
* `/etc/sudoers` -- one rule per line: `name`, `name NOPASSWD`, `%group`, `%group NOPASSWD`, `ALL`; `#` starts a comment.

## Credentials in the kernel

Each process carries an effective uid/gid (what permission checks use), a real uid/gid (who really started it) and up to
16 supplementary groups. The kernel honours the setuid and setgid bits at `exec`, so a program owned by root with mode
`4755` runs with root's effective uid whoever started it. `setuid`/`setgid` follow the usual rules: root may become
anyone (real and effective ids both), everyone else may only re-assert the ids they already have.

Directories with the sticky bit (`/BambooForest`, i.e. `/tmp`, and `/Netherworld`, the trash) let everyone create
entries but only the entry's owner, the directory's owner or root remove or rename them.

## Programs

* `login` -- asks for a name and password on the console, drops privileges and becomes the resident's login shell.
* `su [-c command] [user]` -- become another user (root by default). Setuid root. No password for accounts that have
  none, otherwise three tries.
* `eiki` (and `sudo`, a link to it) -- run a command as root or another user, if `/etc/sudoers` allows it. Anyone
  else gets the traditional answer: *"reimu is not in the sudoers file. This incident will be reported to Eiki."*
  and a line in the kernel log (`[Eiki] reimu tried to borrow root's power and was refused`). Setuid root.
* `passwd [user]` -- change a password (asks for the current one first if there is one). Leaving the new password
  empty removes the password. Setuid root; only root may change someone else's.
* `adduser [-c "Full Name"] [-s shell] [-L] [--no-seat] NAME` -- root only (`sudo adduser ...`). Creates the passwd,
  group and shadow entries, the home directory and, unless `--no-seat`, membership in `shrine`. It asks for a password;
  `-L` creates the account locked instead. `deluser [--remove-home] NAME` undoes it.
* `logger [-t TAG] message...` -- one line into the kernel log, handy from scripts and from a desktop terminal.

## The door: how init lets people in

`/sbin/init` decides who is at the console and what they get:

1. **Autologin** (the default): `reimu`'s desktop opens straight away, as before.
2. **Ask at the door**, with `login` on the kernel command line (or `autologin=none`): on a machine with a display
   the **login screen** (`hakurei-greeter`) appears; in text mode (`text login`) the classic `gensokyo login:` prompt does.
3. `autologin=NAME` opens NAME's session instead of reimu's; `user=root` is the old spelling for root.

Logging out of the desktop always returns to the door, so switching user is just *Shut down... -> Log out*. When a
resident leaves, any process they left behind is sent home (killed), so nothing lingers on their account.

### The login screen

`init` starts `hakurei-session --greeter` as root: a private window server (`fairyd`) plus `hakurei-greeter`, which
draws the moonlit wallpaper, a card with an avatar for every resident who has a seat, and a password field. A wrong
password makes the card shake its head and shuts the gate for a moment; the attempt is logged. A correct one makes the
greeter write the resident's name to a pipe that `init` handed it (`$GREETER_FD`) and exit; `init` then tears the
window server down and starts that resident's own session. *Tab* and the arrow keys switch resident; accounts
without a password just need *Enter*.

Buttons in the corner: **Text console** (exit status 10: `init` opens the text `login` prompt once, and returns to
the login screen when that shell ends), **Shut down** and **Restart**. If the greeter keeps dying (three times in a
row) `init` falls back to the text prompt for good, so the machine is never left without a way in.

## The screen lock

*Start -> Lock screen*, the desktop menu, or **Super+L** runs `hakurei-lock`: a full-screen window flagged `FW_LOCK`
that the window server treats as the only thing on the machine. While it exists no other window can take the keyboard,
every shortcut (Alt+Tab, Ctrl+Alt+T, the launcher) is switched off, and nothing is drawn in front of it. It shows the
resident's avatar and a password field; the password is checked by `/sbin/pwcheck`, a tiny setuid-root helper that
answers only for the user who runs it (so the lock itself never reads `/etc/shadow`). A wrong password shakes the card
and shuts the gate for a moment; an account without a password just needs *Enter*.

The one way past it is the kernel's emergency exit, Ctrl+Alt+Backspace, which kills the window server. `init` does not
reward that with a free shell: it opens the text `login` prompt, and the login screen returns afterwards.

## What the seat group guards

* `/dev/fb0` and `/dev/input/events` are owned by group `shrine` (mode 0660): without the group, no desktop.
* `reboot` / `poweroff` (and the desktop's *Shut down* buttons) work for root and for `shrine` members. An outsider
  gets `poweroff: Operation not permitted` and the machine keeps running.

## Tests

`tests/case_accounts.py` (setuid bits, shadow permissions, sudo/su/passwd flows, adduser/deluser, the sticky bit),
`tests/case_login.py` (the greeter with a wrong and a right password, switching resident with Tab, text login,
autologin) and `tests/case_power.py` (ACPI poweroff and reset, and the seat check).
