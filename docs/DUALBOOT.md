# Dual boot next to Windows on the owner's Dell

The owner's Dell OptiPlex (Core i5-7500) has one internal disk with a fresh, activated Windows 11 Education install that must be kept. This page is the owner's procedure for putting
TouhouOS beside it. The OS side is tasks 13, 19 and 20 of the cloud agents' task list; nothing here writes to the internal disk until those are done and tested, and **parts A to D
change nothing on it except Windows' own storage-driver setting**.

Why the BIOS has to change: the SATA controller is in *Intel RAID (RST)* mode, and in that mode TouhouOS (like any OS without Intel's RST driver) cannot see the internal disk or an NVMe drive at all
(`[Kappa] ahci: ... SATA controller is in RAID (Intel RST) mode`). The setting is **SATA Operation: RAID On -> AHCI** in the BIOS (F2 at power-on, System Configuration). Windows installed in RAID mode
loads Intel's driver at boot; when the mode changes it must switch to Microsoft's AHCI/NVMe drivers first or it stops with INACCESSIBLE_BOOT_DEVICE. Part C does that.

## State on 2026-10-24 (from the owner's Disk Management)

Disk 0, the internal 238.35 GB disk: a 500 MB EFI System Partition, `C:` 55.87 GB NTFS **BitLocker encrypted** (Windows 11 Education, activated), then **181.99 GB unallocated at the end of the disk**
(the owner shrank `C:` and left the space alone). Disk 1 is the TouhouOS stick (64 MB ESP, `HUMANVILLAG` 959 MB FAT32, the rest unallocated). So Part E is done. **Later the same day the owner reported
that the BIOS is already in AHCI mode and Windows boots in it** (Windows refused to start in RAID mode), so parts A to C are done as well; what is left is the read-only first look (part D) and the installer's
free-space mode. Because `C:` is BitLocker encrypted, **the recovery key must be saved** (part A, step 1), and if protection was suspended for the change it has to be switched on again afterwards
(`manage-bde -protectors -enable C:`); suspending for several reboots is safer than one: `manage-bde -protectors -disable C: -RebootCount 6`.

## State on 2026-10-25 (late evening): parts F and G are done

`install --alongside` ran on the owner's disk from the stick (over ssh, after the owner's go): partitions 4 (`TOUHOUOS-ESP`, 64 MiB) and 5 (`HUMANVILLAGE`, about 182 GiB) were added in the free space; the installer
read everything back and an independent check of both GPT copies found Windows' partitions 1 to 3 unchanged. **The BitLocker volume had only a clear-key protector** (read-only look at its metadata: no TPM protector, no recovery
password: Windows 11's "encrypted but not yet activated" state), so there was no key to save and nothing for the partition change to trip; the owner had suspended protection anyway. Windows booted normally afterwards.
The stick's loader registered the "TouhouOS" boot-menu entry; the owner pulled the stick, booted the entry, compared the new ssh host key's fingerprint with the screen and set the BIOS boot order. The update loop (push on trial, restart,
confirm) has run twice on the installed disk without anyone at the machine. With the stick *and* the installed disk both present the installed volume is mounted as `/HumanVillage` and the ssh host key changes (INSTALL.md): keep the stick out except as a rescue, and expect a new key (compare its
fingerprint with the screen) when it is in.

## A. In Windows, before touching the BIOS (nothing destructive)

1. **BitLocker.** Open an administrator Command Prompt and run `manage-bde -status`. If C: says *Protection On* (Windows 11 may turn device encryption on by itself when you sign in with a Microsoft
   account): save the recovery key first (https://aka.ms/myrecoverykey while signed in, or `manage-bde -protectors -get C:`), then `manage-bde -protectors -disable C:` for the duration of the BIOS
   change, and `manage-bde -protectors -enable C:` afterwards. A changed storage mode or Secure Boot setting otherwise asks for the recovery key at the next boot.
2. **The licence.** Settings -> System -> Activation: it should say *Windows is activated with a digital license linked to your Microsoft account*. If it does not, use "Add an account". Keep the
   purchase email or the full key as well (Windows only shows the last five characters).
3. **A way back.** Optional but cheap: build Windows 11 installation media on another stick with Microsoft's Media Creation Tool.
4. **Prepare Windows for AHCI.** In the administrator Command Prompt: `bcdedit /set {current} safeboot minimal`, then shut down (do not restart into Windows).

## B. The BIOS

Power on, F2: System Configuration -> SATA Operation -> **AHCI** (Dell warns that the OS may not boot: that is what part A prepared for). Apply, Exit. Leave Secure Boot as it is (it has to stay
off to boot TouhouOS from the stick; Windows does not need it).

## C. Let Windows adopt the AHCI/NVMe drivers

The PC boots into Windows Safe Mode by itself (this is the step that installs the Microsoft storage drivers). Sign in, open the administrator Command Prompt: `bcdedit /deletevalue {current} safeboot`,
restart, and Windows starts normally. Check that it does, and re-enable BitLocker if you disabled it. **If Windows does not start after the BIOS change:** put SATA Operation back to *RAID On*; it boots as
before. Tell me what the screen said.

## D. First contact of TouhouOS with the internal disk (read only)

Boot the TouhouOS stick (F12, the UEFI entry of the stick). Nothing is written: the default command line has `blk.ro`. Open a terminal (Ctrl+Alt+T) and run

```
lsblk
cat /proc/partitions
cat /proc/ahci /proc/nvme
dmesg | grep -i -E 'ahci|nvme|blk|raid'
su marisa
sudo install
```

and send the output (or a photo). It shows whether the disk is SATA or NVMe, its size and the Windows partitions (ESP, MSR, NTFS, recovery), and whether the drivers worked on real hardware for the first time.
`install` without arguments only lists the disks and what it thinks of each (the boot stick, a disk that holds Windows and is therefore protected, a disk in use, an empty disk); it writes nothing. It needs root
because the disk nodes are root-only: `reimu` may not use `sudo`, `marisa` may (no passwords at first; just press Enter if asked).
This is the point to stop and look before anything is written.

**If the first boot with the internal disk visible hangs or misbehaves** (it is the first time the SATA/NVMe drivers meet this controller): add `ahci=off nvme=off` to `cmdline.txt` on the stick's boot
partition (the 64 MB one) and boot again; the two drivers then do not touch the controllers at all (`[Kappa] ahci: disabled on the kernel command line`). `ahci=off` and `nvme=off` work separately, which also
tells which driver it was. Send `TOUHOU.LOG` from the stick's HUMANVILLAGE volume.

## E. Make room (when the installer for free space is ready)

In Windows: right-click Start -> Disk Management -> right-click the C: volume -> Shrink Volume -> for example 40000 MB (the OS itself needs under 1.2 GB; the rest is space for files). Leave the new space
**Unallocated**: do not create a volume in it. Optionally `powercfg /h off` so that Fast Startup does not leave NTFS half-closed (TouhouOS never touches NTFS, but Windows is happier).

## F. Install

From the stick, as `marisa` (`su marisa`, then `sudo ...`): the free-space mode, `install --alongside` (see [INSTALL.md](INSTALL.md), "Beside another system"). It needs the unallocated space of part E and a sound
GPT; it creates its own ESP and `HUMANVILLAGE` partition (partitions 4 and 5 on the owner's disk) in that space, **changes no existing partition and no other byte of the disk** (the only other writes are
the two copies of the partition table, with two entries filled in), and refuses anything that is not as expected.

```
sudo install --alongside --dry-run nvme0n1       # read the plan: what stays, the free space used, the two new partitions
sudo install --alongside nvme0n1                 # type the disk's name when asked
```

Before it, in Windows: save the BitLocker recovery key and suspend protection for the restart (`manage-bde -protectors -disable C: -RebootCount 2`). After it, **restart with the stick still in**: the
loader on the stick (`findinstalled` in its `cmdline.txt`) finds the installed system and adds a "TouhouOS" entry to the firmware's boot menu, last in the order, so Windows stays the default. Then the stick can
come out and F12 offers TouhouOS. `install --force` / `ERASE WINDOWS ...` is the *other* mode, which wipes a disk: never use it on the Windows disk.

## G. After the install: the stick is not needed again

This is the plan; it needs the network card to work in the OS (see [NETWORK.md](NETWORK.md) for where that stands on the Dell). The image that is installed carries the SSH server and the update tool, so from
the first start of TouhouOS from the internal disk:

1. Find the machine's address: `ifconfig` on its console, or the router's list of clients (the machine announces the name `gensokyo` in DHCP, which many routers turn into a DNS name).
2. From the development PC: `ssh -i ~/.ssh/touhou_ed25519 marisa@ADDRESS` ([SSH.md](SSH.md); compare the host key's fingerprint with the one in `dmesg | grep sshd` on the console the first time).
3. Every later build: `python tools/build.py`, then `python tools/push-update.py --host ADDRESS` ([UPDATE.md](UPDATE.md), "Pushing over ssh"). It sends the new kernel and initrd, restarts the machine, waits for it to answer,
   and confirms the new system; if the new one does not come up, the machine goes back to the previous kernel by itself after two restarts. Only a kernel that hangs before init needs the reset button (twice).
4. The loader (`BOOTX64.EFI`) and the command line are replaced only on request (`--loader`, `--cmdline`): a broken loader needs the stick, so keep the stick somewhere.

## If something goes wrong

- Windows will not boot after the BIOS change: SATA Operation back to *RAID On*.
- The recovery key screen appears: type the key saved in part A.
- TouhouOS does not boot from the internal disk: boot the stick and look at `TOUHOU.LOG`; Windows is not affected.
