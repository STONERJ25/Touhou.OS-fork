# Keine's records: /etc that survives a restart

`/etc` (really `/Hakurei/etc`) lives in the in-memory root filesystem and is rebuilt from the initrd at every boot. Since the
`cloud/persistent-etc` work the files that people change are also kept on the `HUMANVILLAGE` volume, in
`/HumanVillage/.system/etc`, and `init` puts them back over the initrd defaults before anyone logs in. Keine keeps the Human
Village's history, so the kept copies are "Keine's records" and her name is on the log lines.

| File | Mode in `/etc` | Who changes it |
|------|----------------|----------------|
| `passwd` | 0644 | `adduser`, `deluser` |
| `shadow` | **0600, root** | `passwd` (and the Control Panel's *Change password...*, which runs `passwd`), `adduser`, `deluser` |
| `group` | 0644 | `adduser`, `deluser` |
| `hostname` | 0644 | `hostname NAME` (root) |
| `timezone` | 0644 | `timezone ZONE` (root) |
| `network.conf` | 0644 | by hand today; the network tool of task 12 later (format below) |
| `update.conf` | 0644 | by hand today; the update client of task 5 later (format below) |

`su` writes nothing. `sudoers`, `profile`, `hosts` and the rest are not kept: they come from the initrd as before. Any kept file
edited by hand (with `vi`) is kept with `sudo etckeep save FILE`.

## Where the files live, and how they get into /etc

```
/HumanVillage/.system/        root's, 0700, on every FAT mount (see "How shadow stays root's")
/HumanVillage/.system/etc/    passwd shadow group hostname timezone network.conf update.conf
```

The overlay is a **copy**, not a mount: `/etc` stays on tmpfs, and `init` copies each kept file over the initrd default after
checking it. That keeps the modes of `/etc` exactly as before (shadow 0600 on a filesystem that has modes), lets every file fall
back on its own, and means "no volume" changes nothing at all.

**Boot order** (`kernel/main.c`, `user/sbin/init.c`):

1. The kernel mounts the root tmpfs and, once the internal disks are known, the `HUMANVILLAGE` volume on `/HumanVillage`
   (`storage_mount_boot_volumes()`). A USB stick is mounted by the hot-plug thread a moment later.
2. The initrd is unpacked: `/etc` holds the defaults.
3. `usb_wait_settled(8000)`: a stick's volume is normally mounted by now (bounded at 8 s).
4. `init` starts. Before the clock, the door or any session: `etc_snapshot()` notes size and time of every `/etc` file, then
   `etc_restore()` copies each kept file that passes its check; `etc_reload_homes()` tells the volume who owns which home.
   Only then `setup_clock()` reads the (possibly restored) `/etc/timezone`.
5. **A stick that comes later** (slower than the 8 s wait): before each new session, `init` looks again. It restores only files
   whose `/etc` copy is still what the initrd put there (the snapshot), so a change made in the meantime is not undone.
   Sessions that already run keep what they read; new processes see the restored files.

The log says what happened, one line per decision:

```
[Keine] /etc restored from the Human Village's records (/HumanVillage/.system/etc): passwd shadow group timezone
[Keine] no records kept in /HumanVillage/.system/etc: /etc keeps the initrd defaults
[Keine] /HumanVillage/.system/etc/passwd is damaged (line 2: 3 fields, a passwd line has 7): Keine falls back to the initrd default
[Keine] /HumanVillage/.system/etc/group is damaged (...): the complete group.new written just before is used
```

The lines go into the kernel log (so `dmesg` and the ledger `TOUHOU.LOG` on the volume have them); `init` borrows Keine's name
for them with `gap_setname()`.

## Writing: atomic, and on both sides

Every tool goes through `etc_commit(name, text, len)` in libgap (`user/libc/src/etcstore.c`, `<etcstore.h>`):

1. **Check** the new text with the same rules the boot uses (below). A text that would not survive the next boot is refused, and
   nothing changes.
2. **`/etc`**: write `/etc/NAME.new`, born with its final mode (0600 for shadow, so it is never readable for a moment), close,
   rename over `/etc/NAME`.
3. **The volume**, when one is mounted on `/HumanVillage`: make `.system/etc` if needed, write `NAME.new`, close, `sync()`,
   rename over `NAME`, `sync()`.

Results: 0 done; 1 "`/etc` changed, but the volume could not keep it" (read-only volume, full volume, ...): the tools then say
`passwd: the new password lasts only until the next restart: the HUMANVILLAGE volume is mounted read-only (...)`; -1 nothing
changed. With no volume at all (`nohumanvillage`, a machine without one) a change is silently RAM-only, as it always was.

**What a power cut leaves.** Before the rename the volume holds the complete old `NAME` and maybe a partial `NAME.new`; after the
second `sync()` it holds the complete new `NAME`. In between it depends on the FAT driver's write order:

* With the order of `cloud/fat-ordering` (task 7: frees after entries, and a rename over a name spelled exactly alike rewrites the
  entry in place), the name holds either the old or the new file.
* With the order on `main` today (data, then the whole FAT, then the entries), the old file's clusters may be marked free before
  the entry that pointed at them is rewritten. A cut there leaves `NAME` pointing at free clusters, which reads as damaged, while
  `NAME.new` is complete (it was synced before the rename). `etc_restore()` therefore uses `NAME.new` when `NAME` is missing or
  damaged and `NAME.new` passes the check, and says so in the log. Either way the result is the old or the new text.

After any cut the volume is flagged dirty and mounts read-only (STORAGE.md, "Safety design"): the records are still read at boot,
but changes last until the next restart until the volume has been checked (`mkdisk.py --check` or `fsck`) or mounted with
`-o force`.

Several files are not one transaction: `adduser` writes `shadow`, then `group`, then `passwd`, so a cut in between leaves at most
a line nobody refers to; `deluser` removes the `passwd` line first. A user in `passwd` without a `shadow` line cannot log in (the
lookup fails closed).

## The checks (what "damaged" means)

Common to all: at most 64 KiB, no NUL or control characters (tab is allowed), and the last line ends with a newline (a file cut
short in the middle of a line is the usual damage). Blank lines are allowed.

| File | Each line | The whole file |
|------|-----------|----------------|
| `passwd` | 7 fields; name of letters, digits, `_ - .`; numeric uid and gid; home and shell absolute | a `root` line with uid 0; no name twice |
| `group` | 4 fields; numeric gid; members are names | at least one group; no group twice |
| `shadow` | 2 to 9 fields; hash empty, `!...`, `*`, or `$gap$SALT$` and 64 hex digits | a `root` line; no name twice |
| `hostname` | letters, digits, `-`; at most 31 (the size of `uname`'s field) | exactly one line |
| `timezone` | starts with a letter or `<`, no spaces, at most 63 | exactly one line |
| `network.conf`, `update.conf` | see the formats below; `#` comments | may be empty |

A damaged file falls back to the initrd default **on its own**: a damaged `shadow` next to a good `passwd` gives the default
passwords (empty for the residents, `!` for root). That is what the brief asks for and it keeps the machine usable; it also means
that a damaged `shadow` opens the accounts that had passwords. Someone who can damage the file on purpose holds the stick in
their hands and could as well write a `shadow` of their choice from another computer, so this adds no new way in.

## How shadow stays root's on a filesystem without permissions

FAT stores no owners and no modes. The FAT driver gives them from mount options and, with `userdirs`, per home
(STORAGE.md). One more rule, in `fat_top_owner()` (`kernel/fs/fat_dir.c`): **a directory named `.system` at the top of a FAT
volume, and everything below it, belongs to root, directories 0700, files 0600, on every FAT mount** - whatever `uid=`, `gid=`,
`fmask=` or `dmask=` say, with or without `userdirs`, and also when a directory is renamed to `.system`. So:

* `ls -ld /HumanVillage/.system` shows `drwx------ root root`; a resident cannot list it, nor open `.system/etc/shadow`.
* The raw partition is no way around it: `/dev/vda2`, `/dev/sdb2` and the other block devices are 0600 root.
* The copy in `/etc` is 0600 root, as before.

What this does **not** cover: someone who takes the stick (or the disk) to another computer reads every file on it, `shadow`
included. The hashes are a single salted SHA-256 (ACCOUNTS.md): they keep residents out of each other's accounts, not an attacker
with a copy and time. Encrypting the volume would be the answer to that and is not attempted.

## Homes on the volume belong to the kept residents

With `userdirs` a top-level directory named like a user is that user's (0700/0600). The kernel's boot-time mount only knows reimu,
marisa and sakuya; a resident added by `adduser` used to get a root-owned home on the volume. Now root can hand a mounted
`userdirs` volume the residents of `/etc/passwd` (uid 1000 to 59999) with `ioctl(fd, FATIOC_USERS_RELOAD)` on any directory of the
volume (`shared/abi.h`; `fat_dir_ioctl()` in `kernel/fs/fat32.c`; up to 32 residents, `FAT_MAX_USERS`, with names of at most 23
characters: a longer name, which `adduser` allows up to 31, keeps a root-owned home on the volume). `init` does it after the
restore, `adduser` and `deluser` after every change (`etc_reload_homes()`), so a new home is its resident's at once and after
every restart.

## Formats for the network and update tasks

Both are line-based text, `#` starts a comment line, blank lines are ignored. The checks are in `check_network()` and
`check_update()` in `etcstore.c`; the tools of tasks 12 and 5 should write through `etc_commit()` so the boot never finds a file
they would not accept.

**`/etc/network.conf`** (task 12; the shape settled on the `parked/network-https` branch, plus `mac`):

```
IFACE dhcp [mac XX:XX:XX:XX:XX:XX]
IFACE static ADDR/PREFIX [gateway GW] [dns D1 [D2 [D3]]] [mac XX:XX:XX:XX:XX:XX]
```

* One line per card; a card that is not listed uses DHCP (today's behaviour). `IFACE` is a card name such as `eth0`.
* `ADDR` is not `0.x`, `127.x` or multicast; `PREFIX` is 1 to 30; `GW` lies inside `ADDR/PREFIX` and is not `ADDR`; at most three
  name servers.
* `mac` binds the line to the card with that hardware address: the line is used only when `IFACE` has that MAC. Several lines for
  one `IFACE` are allowed when their `mac` differ (one stick, several machines); the same `IFACE` and `mac` twice is damage.
  A line without `mac` applies to whatever card has that name.

**`/etc/update.conf`** (task 5):

```
url=http://192.168.1.10:8080/
key=0123456789abcdef
```

* `url=` the update server (`http://` or `https://`, no spaces, at most 200 characters); empty means "not configured".
* `key=` the key id the manifest must carry in its `key` line (16 lower-case hex digits: the first 8 bytes of SHA-256 over the
  65-byte public key, UPDATE.md); empty means "the key built into the system".
* Other `name=value` keys (lower-case letters, digits, `_`) are accepted and left alone, so later versions can add some; a key set
  twice is damage. The last installed `issued` number is state, not configuration: it belongs next to the installed files, not here.

The initrd ships both files with comments only (`initrd/Hakurei/etc/`), so "no file" and "the default" mean the same.

## A stick moved between machines

The records travel with the volume: the same passwords, residents, name and time zone on every machine the stick boots. Things to
know:

* **First found wins** (STORAGE.md): a machine with an internal `HUMANVILLAGE` volume (an installed system) uses its own records,
  and the stick's are left alone, as its homes are.
* **The clock chip**: `rtc=local` and `tz=` are kernel command line words and live on the stick's ESP (`cmdline.txt`), so they move
  too. A machine whose chip keeps local time needs `rtc=local` whichever records are kept. `tz=` wins over the kept
  `timezone` (it sets `TZ` for every session).
* **Network**: a `static` line for `eth0` meant for one machine would be used on the next one's `eth0` too; give it a `mac` so
  that other machines fall back to DHCP.
* **Homes**: uids come from the kept `passwd`, so a home stays its resident's on every machine.
* The FAT timestamps of the volume follow `tz=` only (the boot-time mount knows no other zone); a kept `timezone` changes what
  programs show, not the offset the volume is mounted with.

## Tools

| Command | What it does |
|---------|--------------|
| `etckeep` | which files are kept, and whether `/etc` and the kept copy agree (root sees everything; others are told that only root can look) |
| `etckeep check [FILE...]` | runs the boot's checks over `/etc/FILE` and the kept copy, with the reason for anything damaged |
| `sudo etckeep save FILE...` | keeps the current `/etc/FILE` (after a hand edit) |
| `sudo etckeep forget FILE...` | removes the kept copy: the initrd default returns at the next boot |
| `timezone [ZONE]` | shows the zone in effect; root sets it (`timezone JST-9`). A zone the C library cannot read is refused |
| `hostname [NAME]` | shows the name; root sets it. `gethostname()` and `uname()` in libgap read `/etc/hostname` (the kernel's `gensokyo` is the fallback) |
| `passwd`, `adduser`, `deluser` | as before; their writes now go through `etc_commit()` |

Manual pages: etckeep(8), timezone(1), hostname(1), network.conf(5), update.conf(5), and the updated passwd(1), shadow(5),
adduser(8), deluser(8), timezone(5), accounts(7).

## Tests

`tests/case_etc.py` (`python tests/run_all.py etc_,accounts_persist`), each on its own disk image, two boots where it matters:

| Test | What it shows |
|------|---------------|
| `accounts_persist_password` | reimu sets a password with `passwd`; after a restart the text login refuses the empty one and accepts the new one; `/etc/shadow` is 0600 root |
| `accounts_persist_adduser` | `adduser` gives a home owned by the new resident at once; after a restart the account, group, seat and home owner are there; `deluser` is kept too |
| `etc_timezone_hostname` | `timezone JST-9` and `hostname hakugyokurou` survive a restart (prompt, `uname -n`, `date +%Z`); a non-root `timezone ZONE` and an unreadable zone are refused |
| `etc_corrupt_fallback` | damaged passwd, shadow, hostname and network.conf each give one Keine line and the default; good timezone and update.conf are used; a damaged group with a complete group.new uses the .new; the tools refuse damaged text |
| `etc_shadow_permissions` | a resident cannot list `.system`, read the kept shadow, read `/etc/shadow` or the raw partition |
| `etc_nohumanvillage` | `nohumanvillage`: kept files ignored, changes stay in RAM, the disk image is byte-identical afterwards |
| `etc_usb_stick` | the records on a USB stick are restored before the first session |

## State when paused

Done and passing in QEMU (Ubuntu 24.04, QEMU 8.2 TCG): everything above. Not verified: a real power cut during a write (the
ordering argument above is by reading `fat32.c`/`fat_cache.c`, not by a kill-the-VM test), and anything on the owner's PC.

## How to resume

- **Kill-the-VM test for the records**: write `passwd` in a loop and pull the plug at random moments (the harness of
  `tests/case_storagecrash.py` on `cloud/fat-ordering` does this for files); after each cut the next boot must restore the old or
  the new text, never the default. Best after task 7 is merged.
- **The volume's FAT time zone**: the boot-time mount takes `tzoffset=` from `tz=` only. `init` could hand the volume the offset of
  the kept zone after the restore (a second FAT ioctl next to `FATIOC_USERS_RELOAD`); nodes already loaded would keep their times.
- **More files**: `sudoers` (with a check that root stays able to get in) and `hosts` would fit the same list (`etc_kept_names`).
- **Control Panel**: a time zone and a computer name field on the *System* page could run `timezone` and `hostname` through
  `eiki`, like *Change password...* runs `passwd`.
- **Kernel `uname`**: the kernel still answers `gensokyo`; only libgap reads `/etc/hostname`. A `sethostname` call would need a
  syscall number (the next free one is listed in `shared/syscall.h`).
