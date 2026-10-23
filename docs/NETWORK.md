# Networking: Yukari opens the gaps between machines

Status of this document: **work in progress**. The sections "What works" and "What is not done yet" are kept honest at every
commit; trust them over any comment in the code.

## What works

* **Packet buffers, devices, the lock.** `kernel/include/net.h`, `kernel/net/net.c`. All of the stack runs under one sleeping
  mutex (`net_lock`); system calls take it while they touch socket state and drop it while they sleep (`net_wait`), the
  `Yukari` kernel thread takes it to poll devices and run timers. Drivers are only entered with the lock held, so they need no
  locking of their own. The thread polls every 20 ms when only the loopback exists (loopback traffic wakes it at once) and
  every 1 ms once a real NIC is registered.
* **Loopback `lo`** (127.0.0.1/8, MTU 1500, Ethernet-framed with zero MACs) works without any NIC. Fault injection on the
  loopback path: drop / duplicate / reorder / corrupt N percent (see "Tunables").
* **Ethernet + ARP** (cache with aging, queued packets while resolving, requests retried three times, replies, learning,
  gratuitous announce, static entries); exercised on the QEMU NICs only through DHCP and the gateway, not yet by a ping test.
* **IPv4**: header checksums, TTL, routing table (connected routes, default gateway, longest prefix), output fragmentation,
  **fragment reassembly** (four datagrams at a time, 16 KiB each, 30 s timeout, overlap-safe), broadcast/loopback delivery.
* **ICMP**: echo reply, destination unreachable (port and protocol), error delivery to sockets, rate limited.
* **UDP** sockets: bind, connect, sendto/recvfrom, broadcast with `SO_BROADCAST`, datagrams up to 65507 bytes (fragmented),
  `ECONNREFUSED` on connected sockets via ICMP port unreachable.
* **ICMP "ping" sockets** (`socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`): send an echo request (the kernel stamps the
  identifier and checksum), receive whole IP packets (header included) of replies and errors that carry your identifier.
* **TCP** (`kernel/net/tcp.c`): the RFC 793 state machine, 3-way handshake (and simultaneous open), MSS and window-scale
  options, retransmission with RFC 6298 RTO estimation and exponential backoff, slow start / congestion avoidance /
  fast retransmit and NewReno-style fast recovery, delayed ACKs, Nagle (`TCP_NODELAY`), out-of-order data reassembled in place
  in the receive ring, zero-window probes, FIN/RST handling (RFC 5961 style exact-match resets), TIME_WAIT (a newer SYN
  reopens the tuple), listen backlog, RST for data after `close()` and for unread data at `close()`.
* **Sockets**: `AF_INET` `SOCK_STREAM` / `SOCK_DGRAM` through the existing calls (the UNIX-domain sockets keep working):
  `socket bind listen accept connect shutdown getsockname getpeername send/recv sendto/recvfrom getsockopt setsockopt`,
  options `SO_REUSEADDR SO_BROADCAST SO_ERROR SO_RCVTIMEO SO_SNDTIMEO SO_KEEPALIVE SO_TYPE TCP_NODELAY IP_TTL IP_TOS`,
  blocking and `O_NONBLOCK`, `poll()` (and `select()` in libgap on top of it), `EPIPE` + `SIGPIPE`, `ECONNREFUSED`,
  `ETIMEDOUT`, `ECONNRESET`. Ports below 1024 need root.
* **/proc/net/{dev,route,arp,tcp,udp}** in the Linux formats, **/proc/net/tune** (root-writable tunables) and
  **/proc/net/inject** (root-only: each `write()` is one raw Ethernet frame delivered to the stack as if it had arrived on `lo`).
  `procfs` gained subdirectories and writable files for this (`procfs_register("net/dev", gen)`, `procfs_register_rw`).
* **libgap**: `htons/htonl/ntohs/ntohl`, `inet_aton/inet_addr/inet_ntoa/inet_pton/inet_ntop`, `struct sockaddr_in`,
  `select()`, the new socket calls, strerror texts for the network errors.
* **`nettest`** (`user/bin/nettest.c`): the in-guest test program; `tests/case_net.py` runs it.

## Also working since the first commit

* **NIC drivers** (all polled from the Yukari thread, all register one `NetDev`; each calls
  `pci_enable(dev, PCI_CMD_MEM | PCI_CMD_MASTER)` first because main's `pci_init()` clears bus mastering):
  * `kernel/drivers/virtio_net.c` -- virtio-net, legacy (I/O port) and modern (PCI capabilities) transports. Tested in QEMU
    (`-device virtio-net-pci`, which is the modern transport on q35).
  * `kernel/drivers/e1000.c` -- Intel e1000 (82540EM and relatives) and e1000e (82574L), tested in QEMU (`-device e1000`,
    `-device e1000e`); plus **best-effort, untested** support for the PCH LAN parts of real desktops (I217, I218, I219 ids,
    including the Kaby Lake LM4/LM5/V4/V5 ids): no global reset and no PHY access for them, Linux's
    `initialize_hw_bits_ich8lan` and the SPT/KBL transmit-hang errata bits, MAC taken from RAL0/RAH0 as the firmware left
    it; if the registers do not answer or the MAC looks wrong it logs one line and leaves the card alone.
* **DHCP client** (`kernel/net/dhcp.c`): DISCOVER / OFFER / REQUEST / ACK, renewal at half the lease, restart on NAK or
  expiry, sets the address, the default route and `/etc/resolv.conf` (written once init is running, so it never races the
  initrd unpack). It starts by itself when a NIC reports carrier; nothing else ever transmits first. Boot is not delayed:
  everything happens in the Yukari thread. QEMU user networking gives 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3.
* **Test harness**: `Qemu(net=...)` takes a NIC model (`True` keeps the old e1000 behaviour; `"virtio-net-pci"`, `"e1000"`,
  `"e1000e"`, `"rtl8139"`, ...), `hostfwd=["tcp:127.0.0.1:18080-:80"]` and `netdev_opts="..."` for user-mode networking.

## What is not done yet

* **Drivers**: RTL8139 (QEMU `-device rtl8139`) and RTL8168/8111 (r8169 family) are not written. The e1000/e1000e/PCH paths
  have only been exercised on QEMU's emulation, never on real hardware.
* **No `/etc/hosts`**, no **DNS resolver** (`getaddrinfo`, `gethostbyname`, `getservbyname`) in libgap.
* **No tools**: `ifconfig`, `ip`, `ping`, `route`, `netstat`, `nslookup`/`host`, `nc`, `wget`, `ntpdate`, `httpd`, and no manual
  pages for them or for the stack (main now has Patchouli's library: every kappabox applet needs a `.pw` page, see
  `docs/TOOLS.md` on main). The kernel side they need (`SYS_netctl` for interfaces, routes, ARP, DHCP; ICMP sockets;
  `/proc/net/*`) is in place and the ICMP socket is exercised by `nettest`.
* **Tests missing**: ping of the gateway 10.0.2.2 through a NIC (needs the `ping` tool), a TCP session to a host-side Python
  server through 10.0.2.2, an inbound connection through `hostfwd` to the guest's `httpd`, `wget` from the host, the
  malformed-packet test (`nettest fuzz` exists and feeds `/proc/net/inject`, but it needs root and has not been run or wired
  into `case_net.py` yet) and the packet-loss test (`nettest loss` plus `echo "lo_drop 5" > /proc/net/tune`, same story).
* Not implemented in TCP: SACK, timestamps, PAWS, keepalive probes (the option is stored), `SO_LINGER`, urgent data,
  path-MTU discovery. IPv6 is out of scope.

## Design notes

* **Rings indexed by sequence number.** The send and receive buffers of a connection are 128 KiB rings addressed by
  `seq & (TCP_BUF - 1)`. Retransmission reads straight from the send ring; a segment that arrives out of order is copied to
  where it belongs and remembered as an interval (up to 8), and rcv_nxt jumps over the holes when they fill.
* **Addresses and ports are host byte order inside the kernel**, network order only in headers and in `sockaddr_in`.
* **Checksums** are summed in little-endian word order and folded; the inverted sum is stored with a native 16-bit write
  (the ones-complement sum commutes with byte swapping). See `net_csum`.
* **Syscalls**: `SYS_sendto` ... `SYS_netctl` are 90-96 (`shared/syscall.h`); the existing 70-76 serve both families. `bind`,
  `connect` and `accept` now take a `sockaddr*` for both families (they used to take a bare path for UNIX sockets).

## Tunables (`/proc/net/tune`, root)

`lo_drop`, `lo_dup`, `lo_reorder`, `lo_corrupt` (percent of loopback frames), `rto_min_ms` (200), `syn_retries` (5),
`data_retries` (10), `timewait_ms` (30000). `echo "lo_drop 5" > /proc/net/tune`.

## Tests

`python tests/run_all.py net` runs `tests/case_net.py`. `net_loopback` boots a text console and runs `nettest`: UDP including a
6000-byte fragmented datagram and ICMP refusal, TCP echo, a 5 MiB transfer with checksum between two processes, refused
connections, non-blocking connect/accept with `poll`, orderly and abortive close, half-close, 24 simultaneous connections,
socket options and timeouts, a ping through an ICMP socket.

## State when paused

Done and committed: everything under "What works" and "Also working since the first commit". Verified by running
`python tests/run_all.py net_loopback net_dhcp` (loopback checks in the guest; DHCP lease on virtio-net, e1000, e1000e) and
`utest` (AF_UNIX sockets still work after the bind/connect/accept ABI change).

Half done: `nettest fuzz` and `nettest loss` are written but never run (they need root: boot with `text autologin=root`, prompt
`root@gensokyo ...`). The e1000 code for the PCH parts is a best guess from memory of Linux's driver.

Not started: RTL8139, RTL8168, the DNS resolver in libgap, /etc/hosts, all tools and their manual pages, the remaining tests.
Not run at all: the full suite (`python tests/run_all.py`), the desktop tests (the socket calls fairyd uses changed ABI; only
`utest` covers AF_UNIX).

Known problems: none seen in what was run. Things to watch: the 20 ms idle poll of the Yukari thread when only `lo` exists;
the TCP out-of-order table holds 8 intervals; the e1000 drivers copy every frame once (no zero-copy); `ICMP` sockets deliver
whole IP packets.

How to resume: `python tools/build.py`, then `python tests/run_all.py net` (loopback + DHCP tests). Next steps in order:
(1) `ping` and `ifconfig` applets in `user/bin/kappabox/net_*.c` (`APPLET(name)` files are picked up automatically; use
`SYS_netctl` via a libgap wrapper and ICMP sockets) and a test that pings 10.0.2.2 on each NIC; (2) RTL8139, then RTL8168;
(3) `/etc/hosts` + resolver (`user/libc/src/netdb.c`, `nameserver ADDR[#PORT]` is handy for tests); (4) route/netstat/nc/wget/
httpd/ntpdate/nslookup; (5) wire `nettest fuzz` / `nettest loss` into `tests/case_net.py` (run as root); (6) `git merge main`
(Patchouli's manual pages: every applet needs a `.pw` page, `docs/TOOLS.md`), write the pages and the section-7 overview,
run `python tests/run_all.py manual` and then the whole suite.
