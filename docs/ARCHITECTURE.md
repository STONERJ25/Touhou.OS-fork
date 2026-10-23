# Architecture

## Boot flow

1. OVMF (UEFI) starts `\EFI\BOOT\BOOTX64.EFI` from the FAT volume (`build/esp`).
2. The loader (`boot/`, PE/COFF, MS ABI) reads `kernel.elf`, `initrd.img` and `cmdline.txt`, selects a GOP mode
   (default 1024x768; `video=WxH`, `video=native` (keep the firmware's mode) or `video=max` on the command line), copies the ELF's PT_LOAD segments into one
   physically contiguous block, exits boot services, converts the UEFI memory map to `BootMemRegion[]`, builds boot page
   tables and jumps to the kernel with a `BootInfo*` (see `shared/bootinfo.h`).
3. The kernel (`kernel/`, ELF64, SysV ABI, `-mcmodel=kernel`, linked at `0xFFFFFFFF80000000`) takes over.

## Where things are documented

| Topic | Page |
|-------|------|
| Window system, toolkit, desktop, writing a GUI app | [DESKTOP.md](DESKTOP.md) |
| Users, groups, login screen, screen lock, sudo | [ACCOUNTS.md](ACCOUNTS.md) |
| ACPI, power button, time zones, the clock chip | [POWER_AND_TIME.md](POWER_AND_TIME.md) |
| FAT32, mount policy, disk images, `df` | [STORAGE.md](STORAGE.md) |
| SATA and NVMe | [BLOCKDEVICES.md](BLOCKDEVICES.md) |
| USB (xHCI, hubs, HID) | [USB.md](USB.md) |
| Editor, pager, manual pages, shrine applets | [TOOLS.md](TOOLS.md) |
| Booting a real PC from a stick | [REAL_HARDWARE.md](REAL_HARDWARE.md) |
| Theming rules | [THEMING.md](THEMING.md) |
| What is left, unmerged branches | [NEXT.md](NEXT.md), [ROADMAP.md](ROADMAP.md) |

## Virtual address space

| Range | Use |
|-------|-----|
| `0x0000000000000000 - 0x00007FFFFFFFFFFF` | user space |
| `0xFFFF800000000000` + phys | direct map (HHDM) of RAM |
| `0xFFFFFFFF80000000` and up | kernel image |

## Source tree

| Dir | Contents |
|-----|----------|
| `boot/` | UEFI loader |
| `shared/` | headers/sources used by loader, kernel and userland (`bootinfo.h`, `elf.h`, 8x16 font) |
| `kernel/` | the kernel; headers are all in `kernel/include/` |
| `user/` | libc (`libgap`), `crt0`, programs (`user/bin/*`) |
| `initrd/` | files copied into the initrd verbatim |
| `tools/` | build driver, QEMU harness, generators |
| `tests/` | scripted QEMU tests |

## Writing a hardware driver

Everything a PCI driver needs already exists; look at `kernel/include/pci.h`, `blk.h`, `input.h` and `mm.h`.

- **Probing:** `pci_init()` enumerates every bus once (legacy 0xCF8/0xCFC mechanism, BARs sized and decoded, bridges
  followed). Drivers search with `pci_find_class()` / `pci_find_id()`, call `pci_enable(dev, PCI_CMD_MEM | PCI_CMD_MASTER)`
  and map registers with `pci_map_bar()` (uncached). Set `dev->driver = "name"` once a device is claimed; it shows in
  `/proc/pci` (`lspci`).
- **Entry points:** `kmain()` calls the weak functions `virtio_blk_init`, `ahci_init`, `nvme_init`, `storage_mount_boot_volumes`
  and `usb_init` (defaults in `kernel/drivers/driver_stubs.c`). A driver just defines the strong function in its own file,
  so adding a driver never edits `main.c`. New `.c` files anywhere under `kernel/` are picked up by `tools/build.py`.
  Devices that appear later than that (USB sticks: the bus is enumerated by the `usbd` thread seconds after `kmain` got here) announce
  their disks with `storage_disk_added()`, and `kmain` calls `usb_wait_settled(8000)` right before the first session starts so that a stick's
  HUMANVILLAGE volume is mounted before login (bounded; returns at once without USB).
- **No interrupts, poll instead.** Interrupt routing (IOAPIC, `_PRT`, MSI) is the most fragile part of real PCs and
  QEMU hides most of it, so drivers poll from a kernel thread (`task_create_detached` + `task_sleep_ms(1)`), or spin with
  a timeout for short synchronous operations. At 1 kHz this is invisible in CPU usage and behaves the same everywhere.
  Every wait must have a timeout; a missing or misbehaving device must never hang boot.
- **DMA:** `dma_alloc(size, &phys)` returns zeroed, physically contiguous, page-aligned memory in the direct map.
  `virt_to_phys()` converts any heap or direct-map pointer (never a kernel stack or an ioremap alias). x86 DMA is
  cache-coherent, so no flushing is needed; use `barrier()` and ordered MMIO accesses.
- **Block devices:** fill in a `BlockDev` (heap allocated), set `read`/`write`/`flush`, then `blk_register()`. The block layer
  creates `/dev/<name>`, scans GPT/MBR into partition devices (`vda1`, `nvme0n1p2`), and lists everything in
  `/proc/partitions` (`lsblk`). Filesystems take a `BlockDev` by name (`blk_find("sda1")`, `blk_find_label("HUMANVILLAGE")`).
  The virtio-blk driver, the FAT32/FAT16 filesystem, the HUMANVILLAGE mount policy, `df`/`statfs` and `tools/mkdisk.py`
  (image builder and fsck) are described in [STORAGE.md](STORAGE.md).
- **Network cards:** fill in a `NetDev` (`kernel/include/net.h`: MAC, MTU, `tx(frame)`, `poll()` that calls `net_rx(frame)`), set
  `NDF_LINK` when the carrier is up and `netdev_register("eth", d)`; the stack, DHCP and `/proc/net` do the rest, all under one
  network lock so the driver needs none. Call `pci_enable(dev, PCI_CMD_MEM | PCI_CMD_MASTER)` first. See [NETWORK.md](NETWORK.md)
  (design, sockets, tools, tunables, tests, current state); `kernel/net/` holds the stack, `kernel/drivers/{virtio_net,e1000}.c` the cards.
- **Input:** USB HID turns reports into `input_key(KEY_*, down)` and `input_mouse(dx, dy, wheel, buttons)`; the same path as
  the PS/2 driver feeds the console tty and `/dev/input/events`. Repeat is generated in the input layer.
- **Sound:** a sound driver (`kernel/drivers/audio/hda.c`, `ac97.c`) fills in an `AudioBackend` (`kernel/include/audiodev.h`: a
  looping 48 kHz 16-bit stereo DMA ring plus "where is the hardware now?") and calls `audio_register()`; the core in
  `audio.c` owns `/dev/audio`, the mixer thread and `/proc/audio`. See [AUDIO.md](AUDIO.md).
- **/proc files:** `procfs_register("name", gen)` adds a text file generated on each read.
- **Logging:** `klog("Kappa", ...)` for hardware (see THEMING.md for tags). Never `printk` raw from a driver.
- **Safety:** never write to a disk or partition on your own initiative. Only the explicit mount path and `/dev` writes by root
  may modify storage.

## Processes and threads

A `Process` (`kernel/include/process.h`) owns the address space, descriptors, cwd, credentials and signal dispositions; each
user `Task` (`kernel/include/task.h`) is one thread inside it, with its own kernel stack, FS base (TLS), signal mask and
thread id. System calls 106-115 are the thread calls and the futex that user space builds every lock on; the pthread library is
`user/libc/src/pthread.c`. Design, ABI, locking rules, limits and the list of what is missing: [THREADS.md](THREADS.md).

## Testing

`python tools/build.py` builds everything into `build/`; `python tests/run_all.py [name-filter]` runs the whole suite in QEMU
(`tools/qemu.py` is the harness: serial console over TCP, QMP input injection, `screendump`). Tests are plain functions
registered with `@test` (from `tests/harness.py`) in files named `tests/case_*.py`, which `run_all.py` discovers, so a new
feature adds its own file and edits nothing shared. `Qemu(extra=[...])` appends QEMU arguments (devices, drives);
`Qemu(disk=path)` attaches a virtio-blk disk. `tests/test_shell.py` shows how to drive the shell and check each command's
output and exit status; `tests/debug_shell.py lines.txt` prints what the console says for a file of command lines.

## Conventions

- Canonical pixel format is `0x00RRGGBB`; the framebuffer layer swaps channels when the hardware is RGBX.
- Log lines use `klog("Subsystem", ...)`; each subsystem tag gets a stable colour on screen.
- Kernel code is compiled with `-mgeneral-regs-only` (no SSE/x87 in the kernel).
