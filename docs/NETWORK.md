# Networking: Yukari opens the gaps between machines

TouhouOS has a TCP/IP stack in the kernel, drivers for the network cards of QEMU and of common PCs, a DHCP client, a DNS
resolver in the C library, and the usual tools. Yukari Yakumo opens gaps between places; here she opens them between
machines (the kernel log tag is `[Yukari]`). Everything is IPv4 and polled: no interrupts, no IPv6. The kernel carries bytes only; TLS is a library in user space
([TLS.md](TLS.md)) that `fetch`, `wget`, `https_get` and the Browser use for `https://`.

```
 tools (ifconfig ip route ping netstat ss nslookup host fetch wget nc sntp ntpdate httpd)   user/bin/kappabox/net_*.c
   |  libgap: sockets, getaddrinfo / gethostbyname / getservbyname, inet_*, select      user/libc/src/{socket,netdb,inet}.c
   v  system calls 70-76 (socket..socketpair, both families) and 90-96 (sendto ... netctl)
 sockets: Sock, UDP, ICMP "ping" sockets, syscall glue                              kernel/net/{sock,sockcall}.c
 TCP: state machine, rings indexed by sequence number                               kernel/net/tcp.c
 IPv4 + ICMP + ARP + routing + fragments, the loopback, the Yukari thread          kernel/net/net.c
 DHCP client                                                                        kernel/net/dhcp.c
 NetDev: name, MAC, MTU, tx(frame), poll() -> net_rx(frame)
   |- lo        (loopback, always)
   |- virtio_net.c   virtio-net, legacy and modern transport
   |- e1000.c        Intel e1000, e1000e, and the PCH parts I217 / I218 / I219
   |- rtl8139.c      Realtek RTL8139
   `- r8169.c        Realtek RTL8168 / RTL8111 / RTL8169
```

## Using it

* **Boot.** When a card reports a carrier, the kernel starts a DHCP client on it, in the background (the boot never waits).
  A lease sets the address, the default route and `/etc/resolv.conf`. Under QEMU's user-mode networking that is
  10.0.2.15/24 via 10.0.2.2 with the name server 10.0.2.3. Unplugging and re-plugging the cable restarts DHCP by itself.
  **A static card never asks.** `netconf` (run by init, after it has put the kept `network.conf` into `/etc`) gives a card its static address, but the DHCP client starts
  when the carrier comes, long before init, and a lease could arrive first (and a real DHCP server would have heard a machine that was told to be static). So the first card
  to register (`netdev_register`, after the Human Village is mounted) reads the copy of `network.conf` kept on the volume (`/HumanVillage/.system/etc/network.conf`); when
  it has a line whose second word is `static`, the automatic starts of the client (a card appears, the carrier comes, a driver recovers) only mark the card as waiting
  (`held` in `/proc/net/dhcp`) until one of these happens: init says that netconf has run (`NETCTL_NETCONF_DONE`, sent by `run_netconf` in `user/sbin/init.c` whatever came
  of netconf), the boot finds that no init will run it (`init=kshell`, or init did not start), or `netconf.wait` seconds (20; `netconf.wait=0` turns the wait off) have passed
  since init started. Then every waiting card that has no address of its own asks; a card that netconf gave an address had its wish cancelled (`dhcp_stop`). `ifconfig eth0 dhcp`
  never waits. A boot whose kept file has no static line, or that has no such file, is not delayed at all. Limit: a Human Village that is mounted only after the cards (a
  USB stick) is not seen by this, and the old race remains there. Tests: `net_static_config_at_boot`, `net_dhcp_waits_for_netconf_then_asks`,
  `net_dhcp_hold_ends_at_the_limit_without_netconf`, `net_dhcp_asks_at_once_when_there_is_no_init`, `net_dhcp_wait_can_be_turned_off`.
  The only other thing that sends a packet unasked is the clock: soon after the lease the kernel asks an NTP server for the time ([NTP.md](NTP.md); the lease's server, `ntp=` on the command line, or a built-in public one; `ntp=off` stops it). Nothing else phones home.
* **Tools** (all kappabox applets, each with a manual page; start with `man network`):

  | Tool | What |
  |------|------|
  | `ifconfig`, `ip`, `route` | show and set addresses, MTU, up/down, routes, the ARP cache; `ifconfig eth0 dhcp` restarts DHCP |
  | `ping` | ICMP echo through an ICMP datagram socket, any user |
  | `nslookup`, `host` | raw DNS queries (A, AAAA, NS, CNAME, SOA, PTR, MX, TXT) against `/etc/resolv.conf` or a named server |
  | `fetch`, `wget` | HTTP/1.1 GET with redirects and chunked bodies, to stdout (`fetch`) or a file (`wget`) |
  | `nc` | TCP and UDP, client and listener, `-z` port check |
  | `sntp`, `ntpdate` | NTP query; `-s` steps the clock through `SYS_settime`, `-w` writes the clock chip too (local time with `rtc=local`); `-S` shows what the kernel's own client did, `-n` makes it ask again ([NTP.md](NTP.md)) |
  | `httpd` | a tiny static file server, one process per connection |
  | `netstat`, `ss` | sockets, routes and counters, read from `/proc/net` |
* **Names.** `getaddrinfo`/`gethostbyname` look in `/etc/hosts`, then in a 16-entry cache, then ask the servers of
  `/etc/resolv.conf` (`nameserver ADDR[#PORT]`, `search`, `domain`, `options ndots/timeout/attempts`), following CNAME chains.
  Only A records are used. `getservbyname` knows `/etc/services` and a built-in list of common ones.
* **`/proc/net/`**: `dev`, `route`, `arp`, `tcp`, `udp` in the Linux formats; `tune` (root can write `name value` lines, see
  below); `inject` (root: one raw Ethernet frame per `write()`, delivered as if it had arrived on `lo`).
* **Tunables** (`/proc/net/tune`): `lo_drop`, `lo_dup`, `lo_reorder`, `lo_corrupt` (percent of loopback frames),
  `rto_min_ms` (200), `syn_retries` (5), `data_retries` (10), `timewait_ms` (30000). Reading the file also shows `ip_bad`,
  `tcp_bad` (malformed packets rejected) and the number of packet buffers in use.

## Design

* **One lock.** The whole stack, including the drivers, runs under a single sleeping mutex (`net_lock`). System calls take it
  while they touch socket state and drop it while they sleep (`net_wait`); the `Yukari` kernel thread takes it to poll the
  cards, run the loopback queue and the timers. Drivers are therefore only entered with the lock held and need none of their
  own. The thread polls every 20 ms while only `lo` exists (loopback traffic and socket calls wake it at once) and every 1 ms
  once a real card is registered.
* **Packet buffers** are 2 KiB (`NetBuf`) with 80 bytes of headroom for the headers; at most 16384 exist at a time.
* **Rings indexed by sequence number.** A TCP connection has two 128 KiB rings addressed by `seq & (TCP_BUF - 1)`.
  Retransmission reads straight from the send ring; a segment that arrives out of order is copied to where it belongs and
  remembered as an interval (up to 8), and `rcv_nxt` jumps over the holes when they fill.
* **TCP** (`kernel/net/tcp.c`): the RFC 793 state machine with simultaneous open; MSS and window-scale options; RFC 6298
  retransmission timer with backoff; slow start, congestion avoidance, fast retransmit and NewReno recovery (initial window
  10 segments); delayed ACKs (every second segment or 40 ms); Nagle (`TCP_NODELAY` turns it off); zero-window probes; exact-match
  resets (RFC 5961); TIME_WAIT that a newer SYN may reopen; listen backlog; RST for data after `close()` and for unread data at
  `close()`. Not done: SACK, timestamps/PAWS, keepalive probes (`SO_KEEPALIVE` is stored), `SO_LINGER`, urgent data, path-MTU
  discovery.
* **IP**: header checksums, TTL, longest-prefix routing (connected routes, default gateway, anything added by hand), output
  fragmentation, reassembly of up to four 16 KiB datagrams at a time with a 30 s timeout (overlap-safe), limited and directed
  broadcast. ARP keeps up to two packets per unresolved neighbour and asks three times, a second apart; when nobody answers, the TCP connect or connected UDP socket that was waiting fails with `EHOSTUNREACH` ("No route to host") instead of timing out after a minute. A host, not a router: packets for other addresses are dropped. ICMP: echo reply, destination unreachable, errors
  delivered to the sockets they belong to, everything rate limited (200 messages per second).
* **Addresses and ports are host byte order inside the kernel**, network order only in headers and in `sockaddr_in`.
* **Checksums** are summed as little-endian words and folded; the inverted sum is stored with a native 16-bit write (the
  ones-complement sum commutes with byte swapping). See `net_csum`.
* **Sockets**: `AF_INET` `SOCK_STREAM`/`SOCK_DGRAM` (and `SOCK_DGRAM` with `IPPROTO_ICMP` for ping) through the existing
  calls; `bind`, `connect` and `accept` take a `sockaddr*` for both families (UNIX sockets used to get a bare path).
  Options: `SO_REUSEADDR SO_BROADCAST SO_ERROR SO_RCVTIMEO SO_SNDTIMEO SO_KEEPALIVE SO_TYPE SO_ACCEPTCONN TCP_NODELAY
  TCP_MAXSEG IP_TTL IP_TOS` (and, accepted without effect or kept so that a read-back answers, the others Linux programs set: [COMPAT](COMPAT.md#sockets-poll-and-event-descriptors-2026-11-08)). Blocking and `O_NONBLOCK`, `poll()`/`select()`, `MSG_PEEK/DONTWAIT/WAITALL/NOSIGNAL`, `EPIPE`
  with `SIGPIPE`, `ECONNREFUSED`, `ETIMEDOUT`, `ECONNRESET`. Ports below 1024 need root. A connect that outlives
  `SO_SNDTIMEO` fails with `ETIMEDOUT` and abandons the attempt.
* **Waiting for sockets.** `poll()` (and the Linux `select`, `ppoll`, `epoll_*`) do not spin: a socket's `FileOps.pollwq` names the one wait queue that every `sock_wake`
  (and so every state change of a TCP, UDP or ICMP socket) wakes, and a `PollWaiter` attached to that queue (`kernel/fs/pollq.c`, `kernel/include/pollq.h`) tells the
  waiting call. AF_UNIX sockets did the same with the queue of their connection (streams) or their own (datagrams); the contract for any file type is in [COMPAT](COMPAT.md#the-readiness-interface).
  Options for UNIX sockets: `SO_PEERCRED`, `SO_PASSCRED`, `SO_TYPE`, ...; a UNIX socket can be a datagram socket, can be bound to a path or to an abstract name and can carry open files in a message.
* **`SYS_netctl`** (96) is how the tools read and change interfaces, routes and the ARP cache and restart DHCP (structures in
  `shared/netabi.h`); changing anything needs root.
* **libgap** also gained `htons/htonl/ntohs/ntohl`, `inet_aton/addr/ntoa/pton/ntop`, `select`, `sockaddr_in`, `netdb.h`,
  and `gap_dns_*` (the resolver one level down, used by `nslookup`).

## The cards

| Driver | Chips | State |
|--------|-------|-------|
| `virtio_net.c` | virtio-net, legacy (I/O port) and modern (PCI capabilities) | tested in QEMU, both transports |
| `e1000.c` | Intel 82540EM and relatives (QEMU `-device e1000`) | tested in QEMU |
| `e1000.c` | Intel 82574L (QEMU `-device e1000e`) | tested in QEMU |
| `e1000.c` | I217, I218, I219 (the Dell target's I219-LM5 `8086:15e3`, device ids 0x15B7..0x15E3 and the earlier PCH ids) | **run on the Dell once**: link and receive work, transmit hangs (see "The first contact with the Dell's I219-LM5" below); the code path is also exercised on QEMU's 82574L with `e1000.pch` on the kernel command line |
| `rtl8139.c` | Realtek RTL8139 (QEMU `-device rtl8139`) | tested in QEMU |
| `r8169.c` | Realtek RTL8168 / RTL8111 / RTL8169 (ids 0x8168, 0x8169, 0x8167, 0x8161) | **never run**: QEMU has no such device |

Every driver calls `pci_enable(dev, PCI_CMD_MEM | PCI_CMD_MASTER)` (plus I/O where it uses ports) first, because `pci_init()`
clears bus mastering of network devices. Each one registers a `NetDev` and follows the same rules: bounded waits, one log line and
hands off if anything looks wrong, link changes noticed by polling.

### What could not be verified on the real PC (Dell OptiPlex, Intel I219-LM)

The I219 driver was written from Linux's e1000e as remembered, without the hardware or its datasheet at hand, and QEMU's
82574L is not a PCH part. Specifically **unverified**:

1. The register offsets and bit values of the management-engine handshake (`H2ME` 0x5B50, `FWSM` 0x5B54, ULP bits) and of the
   software semaphore (`EXTCNF_CTRL.SWFLAG`), and MDIC/PHY addressing (PHY address 1, page select register 31). In QEMU the
   ME is absent (`FWSM` reads 0) and the PHY does not answer MDIC, so only the "nothing there" branches ran.
2. That skipping the global reset is right: the driver relies on the firmware having left the PHY and link working
   (UEFI network boot disabled or not), and programs the rings, `RXDCTL/TXDCTL`, `RFCTL`, `MRQC` explicitly instead. If the PHY
   is powered down, the driver clears `BMCR.PDOWN`; if the ME keeps it in ULP mode with no ME answer, nothing helps.
3. The SPT/KBL transmit errata bits (`IOSFPC`, `TARC0`) and the transmit-queue watchdog that restarts a stuck queue.
4. Autonegotiation time: link-up is polled every millisecond-tick and DHCP starts when `STATUS.LU` first reads 1, so a card
   that needs several seconds simply shows up later (the log says `link up, N Mb/s`); an unplugged cable costs nothing
   (no waits anywhere), and re-plugging restarts DHCP. DHCP itself retries in the background (DISCOVER at 1, 2, 4, 8, 8, 8 s, then
   a minute's pause, then again).
5. The RTL8168 driver (above) has the same status as the PCH path, only more so: nothing about it has run.

If a first boot shows `[Yukari] ... did not answer sensibly; leaving it alone`, the card is left untouched and the rest of the
system is unaffected; `cat /proc/pci` lists the device and who claimed it.

### The first contact with the Dell's I219-LM5 (2026-10-24)

Device `8086:15e3` (Sunrise Point / Kaby Lake, "SPT" in Linux's e1000e), cable plugged in after the boot. What `ifconfig` showed: `eth0` UP,RUNNING, a link,
**receive worked** (32 frames, 20 of them dropped by the stack) and **every transmitted frame failed** (6 sent, 6 transmit errors, no DHCP address): the card
never completed a transmit descriptor. Receive writes to memory, transmit reads from it and from the descriptor ring, which points at a card whose DMA read
side (or the state the firmware left it in) is what is wrong. Everything below was added so that one more boot with the cable in answers the question:

* **Always on (probe, every reset, every link-up)**, taken from Linux: the second transmit queue is set up like the first (`TXDCTL(1)`, `TARC(1)`), the
  K1-off workaround of the SPT parts (`PCIEANACFG` bit 31 copied to `FEXTNVM6` bit 31: with K1 on at 1 Gb/s the MAC can miss DMA completions, which looks
  exactly like this), and `FEXTNVM11` "disable MULR fix".
* **A ladder of eight ways to start the transmit unit**, one per hang (the watchdog moves on after two seconds without progress while the link is up), the log
  names each (`[Yukari] eth0: transmit variant N: ...`): 0 plain restart, 1 transmit unit off and idle first, 2 MAC reset, 3 no errata bits, 4 descriptor
  ring flush (Linux's `e1000_flush_desc_rings`) then MAC reset, 5 MAC and PHY reset, 6 D3hot/D0 cycle then MAC reset, 7 receive stopped and transmit started
  first. `transmit works with variant N` says which one finally moved the ring. `e1000.txvar=N` starts at a variant, `e1000.txstall=N` pretends a hang
  until variant N (tests).
* **Register dumps**: `as the firmware left it` at probe (before this driver wrote anything), `stuck` with the power, link, second-queue and descriptor
  registers at each hang (`GPTC`/`TPT` count frames that really left the card; `TDH` where the transmit unit stopped; `pciE4` bit 8 is "descriptor rings
  must be flushed"; `LNKCTL`/`PMCSR` the PCIe and power state), and `after` once a variant has been applied.

`dmesg | grep -i -E 'yukari|eth0'` shows all of it; a photograph of that is what the next step needs.

#### What the first run on the Dell showed (2026-10-24, `first-look.txt`)

All eight variants, three rounds, 24 restarts, none made the card transmit: **TDH stayed 0, GPTC and TPT stayed 0, no descriptor ever got its done bit**, while receive kept
working (GPRC counted a few frames a second, the stack saw 33 frames in 90 s). The link was 1000 Mb/s full duplex, the ME was alive (`FWSM e001c25c`, bit 15), no ring flush was
asked for (`pciE4` 0), the card has no PCIe capability (an integrated function: no ASPM to switch), and the registers read back exactly what the driver wrote. After a PHY reset or
a D3 cycle the link came back at **10 Mb/s**, which says the PHY's advertisement was reset to something narrow. Two oddities in the dump:

* `RDH 0 RDT 255` in every dump although receive worked: the tail writes of `e_poll` do not seem to land, or these registers do not read back what is real on this part.
* The firmware had **TCTL.MULR (bit 28) and bit 29 set** (`TCTL 3003f0f8`); this driver wrote `TCTL = EN|PSP|CT|COLD` and so cleared both, and cleared them again after every MAC
  reset (whose default has MULR set: `TARC1` bit 28 follows it). Linux changes only the collision threshold and the pad bit.

The second round (this is what `touhou-os-look2` and later images contain):

* Every `TCTL` access is read-modify-write now (`tctl_set`, `tctl_off`); `TIPG` and `PBA` are left as the firmware or the reset made them (they were written before); `STATUS.PHYRA` is
  cleared after resets (Linux does).
* **The PCIm-to-PCI arbiter guard** of Linux's `__ew32_prepare()`: with the ME alive every register write first waits (at most 100 ms) until `FWSM` bit 24 says the arbiter is free,
  and every tail write (TDT, RDT) is read back and repeated if it did not hold (`"RDT did not hold: wrote N, reads M (the ME interfered)"` in the log, first six times).
* Four more variants: 8 the transmit registers exactly as the firmware had them (saved at probe), 9 the Linux reset path (MAC+PHY reset, `PBA` 26 KB, `EXTCNF` PHY-config gate
  cleared, transmitter switched on 1.5 s later), 10 the LANPHYPC pin toggled (the PHY power-cycled) then a MAC reset, 11 a plain `TXDCTL`. Twelve variants, two rounds.
* Rings and buffers are taken from below 4 GiB when the allocator has such memory, and their addresses are logged.
* The dumps grew: TIPG, MANC, FEXTNVM3/4, software view of the rings, arbiter waits, and the **PHY's registers** (BMCR, BMSR, ANAR, ANLPAR, GBCR, GBSR, extended status) and the Kumeran K1 config.
* **`netdiag`** (loader, `boot/netdiag.c`): with `netdiag` on the command line the loader drives the card with the *firmware's* network driver (SNP) for a few seconds: initialises it,
  sends a DHCP DISCOVER, waits for the OFFER, dumps the card's registers and the firmware's first transmit descriptors before and after, shuts the adapter down, and hands the text to
  the kernel (`BootInfo.report_*`), which logs it with a `[Hakurei]` tag. If the firmware's driver can send, those registers are what a working card looks like. It needs the BIOS setting
  "Enable UEFI Network Stack" (Dell); without it the report says there is no SNP.

#### What the second and third runs showed (2026-10-24, `look2` and `look3`)

* **The firmware's driver works, with every frame.** `netdiag` sends a DHCP DISCOVER in three shapes (the plain one, one with the UDP checksum filled in, one exactly like the kernel's own
  client sends it: client id, host name, six parameters, a nonzero IP id) and the router (192.168.1.1) answers all three with an OFFER of 192.168.1.185 (lease 172800 s, mask /24,
  router 192.168.1.1, DNS 203.0.113.53). So the cable, the router and the contents of the frames are not the problem.
* **Transmit works in the OS driver** since the second round: five or six DISCOVERs left the card, `GPTC` matched, `TDH` followed the ring, no transmit error, variant 0 was enough.
* **Receive is the problem.** In `look2` the OS saw 45 frames (24 dropped by the stack, 4 with error bits, no OFFER processed). In `look3` it saw **none**: the card's own
  counters said otherwise (`GPRC` 9, 23, 48 after 3, 8 and 20 s, mostly broadcast and multicast), with `CRC`, `align`, `symbol`, `rxerr`, `missed` and `no-buffer` all zero. Frames were
  accepted by the MAC and nothing reached the ring, nor was any lost for lack of room. `RDH` read 0 throughout, but it did in the very first run too, when receive worked: that register
  is no evidence on this part. The same receive code worked in `look2`, so what decides is state the code does not see: the card's idea of its ring, or the caches.
* A plausible reading: the firmware's driver leaves the receive queue **enabled** (`RXDCTL` 02010000) and the OS driver wrote new ring registers into a queue that stayed enabled, whereas the
  transmit queue (whose enable bit the firmware leaves off) is switched on by the OS driver, an edge, after its ring is programmed. Igb, ixgbe and i40e switch a queue off, wait for the bit
  to read 0, program the ring and switch it on again. The other candidate is the CPU's cache: if the card writes descriptors without snooping (GCR bits 0 to 5) a line still held by the CPU hides
  the write, and which lines get evicted depends on the ring's address (it was `0xb9b28000` in `look2` and `0xacbe8000` in `look3`).

Round 4 (`look4`) does not choose between the explanations; it tries them one after the other and records the evidence:

* **The receive unit is started by `rx_start()`** in one of twelve ways (the *receive ladder*, `g_rxvar`; this is the order of round 5, the first round-4 order is in the git history): 0 the start at probe (after the MAC reset of round 5) cycles the queue (off, wait,
  ring registers, on, wait) and uses the other receive registers (`RDTR`, `RADV`, `RFCTL`, `RXCSUM`, the thresholds in `RXDCTL`) as the firmware's driver had them (saved at probe); 1 a MAC reset, then the same with this driver's own
  thresholds; 2 the descriptor rings flushed the way Linux does it, then a MAC reset; 3 the queue cycled, own thresholds; 4 the receiver switched on before the ring is programmed; 5 the queue switched on last, after the tail pointer;
  6 how the earlier images did it (queue left enabled); 7 promiscuous; 8 management traffic passed to the host as well (`MANC2H`), in case the ME is keeping the frames; 9 D3hot and back, then a MAC reset; 10 a MAC and PHY reset;
  11 the PHY's power cycled (LANPHYPC toggle), then a MAC reset. The resets are the transmit ladder's variants 2, 4, 6, 5 and 10.
* **A receive watchdog** (`rx_watchdog`): with the link up, the card counting good frames and no receive descriptor ever completed (in round 5: no frame with contents), it takes the next step (3.5 s after link-up if the card has
  counted three frames, else 7 s; 5 s per step after that). The reset steps are taken only if the card has counted frames (a quiet network is not a dead card). The log says
  `no frame has reached memory for N s although the card counted M good frames`, then `receive step N: ...`, and finally `receive works with step N (...)`, at which point DHCP is started again at once.
  Only the PCH parts are watched (and a QEMU card with the test hook `e1000.rxstall=N`, which leaves the receive ring without descriptors until step N).
* **Caches**: the receive ring is written back and dropped (`clflush`) before the card owns it, a descriptor's line is dropped before the driver reads it and again after it is released, and the packet's
  buffer lines are dropped before the copy; the GCR no-snoop bits are cleared if the firmware left any set (and the log says so).
* **Evidence**: at +3 s, and again at each step that fails, `receive side:` lines give `RXDCTL`/`RCTL`/`RDBAL`/`RDLEN`/`RDH`/`RDT`/`RDTR`/`RADV`/`RFCTL`/`RXCSUM`, the **receive FIFO** pointers and packet count
  (`RDFPC`: frames the MAC has taken in that the DMA has not moved), the manageability filters (`MANC`, `MANC2H`, `MDEF0-3`: would the ME be keeping frames?), `FWSM`, `LTRV`, the interrupt causes seen, how many
  of the 256 descriptors say done, how many of the first 32 buffers hold data (a frame DMA'd without its descriptor being written back), and whether the **firmware's old ring** (a copy was taken at probe) has
  changed (a card still writing where the firmware's driver had it).
* **A register map to diff**: `regs OFFSET: ...` lines (`g_regmap` in `kernel/drivers/e1000.c`, the same table in `boot/netdiag.c`) with some 140 registers that do not clear when read: the loader prints it before the firmware's driver starts
  and again **while the firmware's driver is receiving frames**; the kernel prints it as the firmware left it, 3 s after link-up, and when the ladder is used up. Lay the firmware's working state next to the kernel's
  and every register where they differ, other than the pointers and counters, is a candidate.

How to read the next `TOUHOU.LOG`: `receive works with step N` (and the lease after it) says which way works; if no step does, the last `receive side:` / `ring:` / `old receive ring` lines say where the frames are
(FIFO packet count above 0: stuck in the card; buffers holding data without a done descriptor: written but not announced; the old ring changed: the card never switched to the new ring; nothing at all: the ME has them).

#### What the fourth run showed (2026-10-24, `look4`): the card had never let go of the firmware's rings

With the queue cycled (step 0) the receive descriptors started to complete (418 frames in three minutes, lengths and flags right: 359 bytes is a DHCP reply, status `73` is done, end of packet and the
checksum-valid bits that the firmware's `RXCSUM` 0x300 asks for), but **every buffer held zeros** (`0 of the first 32 buffers hold data`; all 100 traced frames read `000000000000 < 000000000000`), so the stack threw every frame away
("for another station") and DHCP never got its OFFER. The card also **rewrote the address field of the descriptors it wrote back**: slot 0 read `c5cbbc80` after the first lap and `c5cbc480` after the second. The firmware's driver had
its receive ring at `c5ca1000` (64 descriptors), its transmit ring at `c5ca1400` and its 2 KiB buffers from `c5ca1480` on; `c5ca1480 + 53 * 0x800 = c5cbbc80`, and 53 was the head index it left (`RDH 53` at probe). So **the card went on fetching descriptors
from the firmware's ring and writing the frames into the firmware's buffers**; only the write-back and the head pointer followed the registers this driver wrote. The same was true of the earlier runs: in `look2` the 24 "dropped" frames were these
blank frames, in `look3` the write-backs went to a place nobody looked at. The ladder of round 4 did not notice, because it counted completed descriptors; and the frames were landing in memory the kernel had already reclaimed (nothing visible broke in
three minutes; it is not a state to leave a machine in).

Round 5 (`look5`):

* **A clean slate**: on the PCH parts the MAC is reset before the rings are programmed (`e1000.noreset` leaves it out), after the descriptor-ring flush if the card asks for one (`pciE4` bit 8), exactly what every OS driver does at load; the station address is put back if
  the reset reloaded something else, and the transmit registers the firmware had tuned (`TIPG`, `TARC0/1`, `TXDCTL1`, `TCTL` without its enable bit) are written back, because transmit is known to work with them. If the link has not come back 12 s later, the PHY is power-cycled once.
* **A frame counts only with contents** (the Ethernet header is not all zeros): `blank` frames are counted and thrown away, and the ladder's success is a frame with contents. `e1000.rxgarbage=N` pretends blank frames until step N (tests).
* **The ladder starts with the cure**: step 1 MAC reset, step 2 ring flush and MAC reset, then the cheap variations (queue order, promiscuous, `MANC2H`), and the steps that touch the PHY or the power state last.
* **The loader switches the receiver and transmitter of the Intel Ethernet controllers off just before it leaves boot services** (`nic_quiesce`, always, not only with `netdiag`), so that a firmware driver that is still initialised cannot go on writing into memory the kernel reuses.
* The log says whether the queue-enable bit really cleared when the queue was switched off, how many buffers hold data (all 256 are looked at), and the firmware's old ring is mapped for the "did the card write there" check even where the kernel's direct map does not cover it.

## Tests

`python tests/run_all.py net` (everything below). Tests that type into the serial shell use `text` on the command line.
The "internet" of the QEMU tests is a set of Python servers on the host's loopback, which user-mode networking maps to 10.0.2.2.

| Test | What it proves |
|------|----------------|
| `net_loopback` | in-guest `nettest`, no NIC: UDP incl. a 6000-byte fragmented datagram and ICMP port-unreachable refusal, TCP echo, a 5 MiB transfer with checksum between two processes, refused connections, non-blocking connect/accept with `poll`, orderly and abortive close (EPIPE, ECONNRESET), half-close, 24 simultaneous connections, socket options and timeouts, ping through an ICMP socket, a listen backlog of 2 holding back 5 connects, UDP broadcast with `SO_BROADCAST`, a 700 KB transfer to a reader that sleeps (the window closes and reopens); afterwards no packet buffer may be left over |
| `net_robust` | 3000 malformed frames through `/proc/net/inject` (truncated headers, bad checksums, absurd lengths, odd fragments, bogus TCP options and flags): the counters show they were rejected and the stack still works |
| `net_loss` | a 400 KB TCP transfer over a loopback that drops 5%, duplicates 3% and reorders 5% of the frames arrives intact |
| `net_names_local` | no NIC: `/etc/hosts` through `getaddrinfo`, `ifconfig`/`ip`/`route`/`netstat` on `lo`, the `/proc/net` listing, `nc` listening and connecting over TCP and UDP |
| `net_nic_virtio`, `_virtio_modern`, `_e1000`, `_e1000e`, `_rtl8139`, `_e1000e_pch` | each card: found, DHCP lease with gateway and DNS, the gateway answers ping, ARP and `/proc/net/dev` fill in |
| `net_e1000e_transmit_variants`, `net_e1000e_receive_ladder` | the PCH code path on QEMU's 82574L: every way of starting the transmit unit works and `e1000.txstall=N` walks the transmit ladder; `e1000.rxstall=N` leaves the receive ring empty until step N of the receive ladder, the watchdog takes the steps (with `rxstall=8` it resets the card on the way, steps 7 and 8) and the lease arrives |
| `net_nicdebug_trace` | `nicdebug`: the first frames in both directions are traced, the counter lines and the `receive side:` / register-map lines appear, a card that receives never walks the receive ladder |
| `net_tcp_host` | `nc` to a Python echo server on the host through 10.0.2.2, half-close included; a refused port; an unanswered ARP (10.0.2.77) ends in "No route to host" after about three seconds |
| `net_fetch_host` | `fetch`/`wget` against a Python HTTP server: redirects, chunked body, 200 KB with an md5 check, a 404 |
| `net_httpd_hostfwd` | the guest's `httpd` answers requests that enter through QEMU's `hostfwd` |
| `net_dns_host` | `nslookup`/`host` (A, CNAME, MX, TXT, NS, PTR, NXDOMAIN), the search list, and getaddrinfo through a CNAME for `ping` and `fetch`, against a Python DNS responder |
| `net_sntp_host` | `sntp`/`ntpdate` against a Python NTP responder two hours ahead: offset measured, clock stepped by root, refused for others |
| `net_link_e1000`, `_virtio`, `_rtl8139` | QEMU's `set_link` pulls and re-plugs the cable: the link goes down without hanging anything, plugging in again brings a new DHCP lease and a working network |
| `net_ifconfig_set` | root changes address, mask, MTU, routes and neighbours with `ifconfig`, `ip` and `route` |
| `net_static_config_at_boot`, `net_dhcp_waits_for_netconf_then_asks`, `net_dhcp_hold_ends_at_the_limit_without_netconf`, `net_dhcp_asks_at_once_when_there_is_no_init`, `net_dhcp_wait_can_be_turned_off` | a static line in the kept `network.conf` gives the card its address before the first session and no lease ever shows; the DHCP client waits for init (the log says `DHCP waits until netconf has run`, then `DHCP may ask now: init has run netconf`) and then the card without a line gets its lease; with a pid 1 that never says so (`init=/bin/cat`, `netconf.wait=4`) the wait ends at its limit; with the kernel shell instead of init it ends at once; `netconf.wait=0` turns it off |
| `linux_net_sockets_match_linux`, `linux_net_events_match_linux`, `linux_net_talks_to_the_host` | static glibc programs under the Linux personality: TCP, UDP and UNIX sockets (streams, datagrams, descriptors in messages), the errors, `poll`/`select`/`epoll`/`eventfd`/`timerfd`, ten TCP clients served by one thread, and a program that talks to Python servers on the host through 10.0.2.2 (TCP, UDP, a hand-made DNS query); the first two are compared line by line with the same programs on a real Linux (`tools/linux-oracle.py`) |

The guest clock can run several times slower than wall time, so no test sleeps and then expects an effect: they wait for
output with generous timeouts or retry.

## Limits and known problems

* No IPv6, TLS, raw sockets, multicast, VLANs, bridging or forwarding (the stack is a host, not a router).
* One address per interface; setting an address drops that interface's routes.
* A socket has fixed 128 KiB rings; `SO_SNDBUF`/`SO_RCVBUF` are accepted and ignored. At most 4096 sockets.
* UDP receive queues are capped at 128 KiB per socket; ICMP sockets deliver whole IP packets (header included).
* The TCP out-of-order table holds 8 intervals; data beyond that is dropped and resent.
* TIME_WAIT lasts 30 s, not the traditional 60 to 120.
* The drivers copy every frame once (no zero-copy); the receive path allocates a 2 KiB buffer per packet.
* `nc`, `fetch`, `wget` and `httpd` are simple tools: no IPv6, no persistent connections, no ranges, no cookies, no authentication. `fetch` and `wget`
  speak `https://` too (libhttp and libtls, the certificate always checked, see "fetch and wget" in [TLS.md](TLS.md)); `nc` and `httpd` do not speak TLS.

## Files

`kernel/net/{net,tcp,sock,sockcall,dhcp}.c`, `kernel/include/{net,netsock}.h`, `kernel/drivers/{virtio_net,e1000,rtl8139,r8169}.c`,
`shared/netabi.h` (constants, errno values, `netctl` structures), `user/libc/src/{socket,inet,netdb}.c` and the headers
`sys/socket.h`, `sys/select.h`, `netinet/{in,tcp}.h`, `arpa/inet.h`, `netdb.h`, `user/bin/kappabox/net_*.c`,
`user/bin/nettest.c`, `initrd/Hakurei/etc/hosts`, the manual pages (`network(7)`, `resolv.conf(5)`, `hosts(5)` and one per tool),
`tests/case_net.py`; the readiness interface and the event descriptors are `kernel/fs/{pollq,epoll,eventfd,timerfd}.c`, `kernel/include/{pollq,evfd,unixsock}.h`, and the Linux calls around sockets `kernel/linux/{net,epoll}.c` with the tests `tests/case_linux_net.py` (`linux_net_*`, against a real Linux). `procfs` gained subdirectories (`procfs_register("net/dev", gen)`) and writable files
(`procfs_register_rw`) for `/proc/net`.
