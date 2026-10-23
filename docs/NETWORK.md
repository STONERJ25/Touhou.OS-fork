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
  gratuitous announce, static entries) -- written, but only the loopback has been run so far (no NIC driver yet).
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

## What is not done yet

Honestly: everything that touches real hardware or user-visible configuration.

* **No NIC drivers yet** (virtio-net, e1000, e1000e + PCH, RTL8139, RTL8168). The `NetDev` interface they plug into exists and
  is exercised by the loopback. Note for the drivers: `pci_init()` on main clears Bus Master Enable of network devices, so a
  driver must `pci_enable(dev, PCI_CMD_MEM | PCI_CMD_MASTER)` (plus `PCI_CMD_IO` for I/O-BAR devices) first.
* **No DHCP client** (`kernel/net/dhcp.c` is a placeholder), no `/etc/resolv.conf` / `/etc/hosts` handling.
* **No DNS resolver** (`getaddrinfo`, `gethostbyname`, `getservbyname`) in libgap.
* **No tools**: `ifconfig`, `ip`, `ping`, `route`, `netstat`, `nslookup`/`host`, `nc`, `wget`, `ntpdate`, `httpd` and their manual pages.
  The kernel side they need (`SYS_netctl`: interfaces, routes, ARP; ICMP sockets) is in place.
* **Tests missing**: per-NIC DHCP + ping, TCP to a host-side server through 10.0.2.2, hostfwd inbound, wget.
  Present: loopback TCP/UDP/ICMP checks (`net_loopback`).
* Not implemented in TCP: SACK, timestamps, PAWS, keepalive probes (the option is stored), `SO_LINGER`, urgent data, MSS
  clamping by path MTU discovery. IPv6 is out of scope.

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
