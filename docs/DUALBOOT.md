# Dual boot next to Windows on the owner's Dell

The owner's Dell OptiPlex (Core i5-7500) has one internal disk with a fresh, activated Windows 11 Education install that must be kept. This page is the owner's procedure for putting
TouhouOS beside it. The OS side is tasks 13, 19 and 20 of [CLOUD_AGENT_BRIEF.md](CLOUD_AGENT_BRIEF.md); nothing here writes to the internal disk until those are done and tested, and **parts A to D
change nothing on it except Windows' own storage-driver setting**.

Why the BIOS has to change: the SATA controller is in *Intel RAID (RST)* mode, and in that mode TouhouOS (like any OS without Intel's RST driver) cannot see the internal disk or an NVMe drive at all
(`[Kappa] ahci: ... SATA controller is in RAID (Intel RST) mode`). The setting is **SATA Operation: RAID On -> AHCI** in the BIOS (F2 at power-on, System Configuration). Windows installed in RAID mode
loads Intel's driver at boot; when the mode changes it must switch to Microsoft's AHCI/NVMe drivers first or it stops with INACCESSIBLE_BOOT_DEVICE. Part C does that.

## State on 2026-10-24 (from the owner's Disk Management)

Disk 0, the internal 238.35 GB disk: a 500 MB EFI System Partition, `C:` 55.87 GB NTFS **BitLocker encrypted** (Windows 11 Education, activated), then **181.99 GB unallocated at the end of the disk**
(the owner shrank `C:` and left the space alone). Disk 1 is the TouhouOS stick (64 MB ESP, `HUMANVILLAG` 959 MB FAT32, the rest unallocated). So Part E is done; what is left is the AHCI switch (parts A to C),
the read-only first look (part D) and the installer's free-space mode. Because `C:` is BitLocker encrypted, **the recovery key must be saved before the BIOS change** (part A, step 1), and suspending BitLocker for
several reboots is safer than one: `manage-bde -protectors -disable C: -RebootCount 6`.

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
```

and send the output (or a photo). It shows whether the disk is SATA or NVMe, its size and the Windows partitions (ESP, MSR, NTFS, recovery), and whether the drivers worked on real hardware for the first time.
This is the point to stop and look before anything is written.

## E. Make room (when the installer for free space is ready)

In Windows: right-click Start -> Disk Management -> right-click the C: volume -> Shrink Volume -> for example 40000 MB (the OS itself needs under 1.2 GB; the rest is space for files). Leave the new space
**Unallocated**: do not create a volume in it. Optionally `powercfg /h off` so that Fast Startup does not leave NTFS half-closed (TouhouOS never touches NTFS, but Windows is happier).

## F. Install

`install` from the stick, in its free-space mode (task 19): it creates its own ESP and `HUMANVILLAGE` partition in the unallocated gap, changes no existing partition, and the loader adds a boot menu
entry (task 20). It refuses a disk that holds Windows unless it is told, in an explicit typed sentence, to erase it. Afterwards: Windows must still boot (it keeps its own entry), and the TouhouOS entry
appears in the F12 menu.

## If something goes wrong

- Windows will not boot after the BIOS change: SATA Operation back to *RAID On*.
- The recovery key screen appears: type the key saved in part A.
- TouhouOS does not boot from the internal disk: boot the stick and look at `TOUHOU.LOG`; Windows is not affected.
