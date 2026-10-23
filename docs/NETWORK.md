# Networking: Yukari opens the gaps between machines

TouhouOS has a TCP/IP stack in the kernel, drivers for the network cards of QEMU and of common PCs, a DHCP client, a DNS
resolver in the C library, and the usual tools. Yukari Yakumo opens gaps between places; here she opens them between
machines (the kernel log tag is `[Yukari]`). Everything is IPv4, plain HTTP, and polled: no interrupts, no TLS, no IPv6.

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
  Nothing else ever sends a packet unasked, and nothing phones home.
* **Tools** (all kappabox applets, each with a manual page; start with `man network`):

  | Tool | What |
  |------|------|
  | `ifconfig`, `ip`, `route` | show and set addresses, MTU, up/down, routes, the ARP cache; `ifconfig eth0 dhcp` restarts DHCP |
  | `ping` | ICMP echo through an ICMP datagram socket, any user |
  | `nslookup`, `host` | raw DNS queries (A, AAAA, NS, CNAME, SOA, PTR, MX, TXT) against `/etc/resolv.conf` or a named server |
  | `fetch`, `wget` | HTTP/1.1 GET with redirects and chunked bodies, to stdout (`fetch`) or a file (`wget`) |
  | `nc` | TCP and UDP, client and listener, `-z` port check |
  | `sntp`, `ntpdate` | NTP query; `-s` steps the clock through `SYS_settime`, `-w` writes the clock chip too (local time with `rtc=local`) |
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
  TCP_MAXSEG IP_TTL IP_TOS`. Blocking and `O_NONBLOCK`, `poll()`/`select()`, `MSG_PEEK/DONTWAIT/WAITALL/NOSIGNAL`, `EPIPE`
  with `SIGPIPE`, `ECONNREFUSED`, `ETIMEDOUT`, `ECONNRESET`. Ports below 1024 need root. A connect that outlives
  `SO_SNDTIMEO` fails with `ETIMEDOUT` and abandons the attempt.
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
| `e1000.c` | I217, I218, I219 (the Dell target's I219-LM, device ids 0x15B7..0x15E3 and the earlier PCH ids) | **never run on hardware**; the code path is exercised on QEMU's 82574L with `e1000.pch` on the kernel command line |
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
| `net_tcp_host` | `nc` to a Python echo server on the host through 10.0.2.2, half-close included; a refused port; an unanswered ARP (10.0.2.77) ends in "No route to host" after about three seconds |
| `net_fetch_host` | `fetch`/`wget` against a Python HTTP server: redirects, chunked body, 200 KB with an md5 check, a 404 |
| `net_httpd_hostfwd` | the guest's `httpd` answers requests that enter through QEMU's `hostfwd` |
| `net_dns_host` | `nslookup`/`host` (A, CNAME, MX, TXT, NS, PTR, NXDOMAIN), the search list, and getaddrinfo through a CNAME for `ping` and `fetch`, against a Python DNS responder |
| `net_sntp_host` | `sntp`/`ntpdate` against a Python NTP responder two hours ahead: offset measured, clock stepped by root, refused for others |
| `net_link_e1000`, `_virtio`, `_rtl8139` | QEMU's `set_link` pulls and re-plugs the cable: the link goes down without hanging anything, plugging in again brings a new DHCP lease and a working network |
| `net_ifconfig_set` | root changes address, mask, MTU, routes and neighbours with `ifconfig`, `ip` and `route` |

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
* `nc`, `fetch` and `httpd` are simple tools: no IPv6, no TLS, no persistent connections, no ranges.

## Files

`kernel/net/{net,tcp,sock,sockcall,dhcp}.c`, `kernel/include/{net,netsock}.h`, `kernel/drivers/{virtio_net,e1000,rtl8139,r8169}.c`,
`shared/netabi.h` (constants, errno values, `netctl` structures), `user/libc/src/{socket,inet,netdb}.c` and the headers
`sys/socket.h`, `sys/select.h`, `netinet/{in,tcp}.h`, `arpa/inet.h`, `netdb.h`, `user/bin/kappabox/net_*.c`,
`user/bin/nettest.c`, `initrd/Hakurei/etc/hosts`, the manual pages (`network(7)`, `resolv.conf(5)`, `hosts(5)` and one per tool),
`tests/case_net.py`. `procfs` gained subdirectories (`procfs_register("net/dev", gen)`) and writable files
(`procfs_register_rw`) for `/proc/net`.
