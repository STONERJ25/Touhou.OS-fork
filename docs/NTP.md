# Network time: the right date after boot (`kernel/net/ntp.c`)

The Dell's clock chip said 2025 in 2026. File times, the `TOUHOU.LOG` stamps, TLS certificate checks and ssh all want a believable date, so the kernel now asks for it itself: a kernel thread
called `ntp` waits until a network card has an address, asks NTP servers (RFC 4330, the simple client), and steps the clock. **The boot never waits for it**: with no network, or no
answer, the clock stays as the chip left it and the thread tries again later.

> **This is the one thing the kernel sends on its own besides DHCP.** With nothing configured it asks public servers (the built-in list below). [NETWORK.md](NETWORK.md) used to say "nothing phones home";
> that is now "nothing phones home except the clock". `ntp=off` on the kernel command line removes all of it; naming a server of your own (`ntp=` or the router's DHCP option 42) keeps the
> traffic on your side of the fence. The decision to leave the built-in list on by default is the owner's (see the report of the branch).

## Where it asks, in order

Three groups, tried in this order, stopping at the first group from which at least one server answers:

| group | source | note |
|-------|--------|------|
| 1 `dhcp` | the NTP servers the lease names (DHCP option 42; the client asks for it in its parameter request list, `kernel/net/dhcp.c`) | up to four |
| 2 `cmdline` | `ntp=host[:port][,host[:port]...]` on the kernel command line | names go through `/etc/hosts` and then the name servers of `/etc/resolv.conf` (`nameserver A.B.C.D#PORT` is understood, as in the C library) |
| 3 `builtin` | `time.cloudflare.com, time.google.com, pool.ntp.org, time.nist.gov` | one operator each; `ntp.builtin=LIST` replaces them (the tests do) |

Within a group the servers are asked in turn until two have answered. Two answers must agree to within **a second**; if they do not, **neither is believed**, the log says so and the clock stays
(the groups after it are not consulted: a disagreement is not "no answer"). One answer alone is accepted (the log says `only X of N servers answered` when more were asked). The answer with the shorter round trip sets the clock.

Names are resolved by a small DNS client in the kernel (`ntp_proto.c` builds and reads the messages): A records only, CNAME chains are the server's business, two tries of 1.5 s per name server.

## What is refused (`ntp_proto.c`)

A reply is thrown away if it: is shorter than 48 bytes, is not a server's reply (mode 4), is not version 3 or 4, does **not repeat our transmit timestamp** (so a late answer to an earlier request is never taken for the
answer to this one: every attempt carries a fresh 16-bit random nonce in the low bits of its timestamp, RFC 4330 section 3), or comes from another address or port than the one asked. Then, by what it says:
stratum 0 (kiss-o'-death: `RATE`, `DENY` ...: the server wants us gone), stratum 16 or more, leap indicator 3 (its clock is not synchronised), a zero or backwards timestamp, a time before 2020-01-01, a root distance
(half the root delay plus the dispersion) over 3 s. And on our side: a round trip (the on-the-wire time, less the server's think time) over 3 s or negative by more than 10 ms (the clock moved under the exchange).
NTP's 32-bit seconds are read with the era rule (top bit set: 1968 to 2036, clear: 2036 to 2104), so 2036 is not a cliff. Each request is tried twice, two seconds each.

## Setting the clock

`time_set_ms()`: the whole offset at once, however small (no slewing). One log line per sync:

```
[Sakuya] clock set from 192.168.1.1: +3.141 s (round trip 4 ms, stratum 2)
[Sakuya] clock set from time.cloudflare.com (162.159.200.1): +31536012.482 s (round trip 21 ms, stratum 3; time.google.com (216.239.35.0) agrees within 3 ms)
```

A failed round is logged and repeated after 10 s, 20 s, 40 s ... up to half an hour (never longer than `ntp.interval`); a good one is repeated after `ntp.interval` seconds (default 14400, the minimum 16). When the network comes back after being gone (a cable pulled and plugged in, a lease that ran out) the next round is brought forward to five seconds after the new lease, whatever the interval: the clock has not been checked in the meantime and this may be another network.
Other lines: `network time: 192.168.1.1: it asks us to stay away (kiss-o'-death): RATE`, `... its clock is not synchronised (leap indicator 3)`, `... an answer to another request`, `... impossible timestamps`, `... too far from a real clock (root distance)`,
`... a round trip of 4021 ms`, `... no answer`, `network time: nosuch.example: the name could not be looked up`, `network time: the servers disagree (A says +0.002 s, B says +7200.000 s): neither is believed, the clock stays as it is`, `network time: no server answered`.

## The clock chip, Windows and `rtc=local`

**The chip is not touched by default**: the Dell dual-boots Windows, which keeps *local* time in the chip and corrects it itself. The system clock is UTC always; the zone (`tz=`, `/etc/timezone`) only decides how it is shown.

`ntp.rtc=1` writes the chip after every sync, as `hwclock -w` would: UTC, or - with `rtc=local` on the command line - the local time of the zone at that instant (the kernel evaluates the POSIX zone rule itself for this,
`tz=` first, then the first line of `/etc/timezone`; `ntp_proto.c` agrees with the C library and with the tz database for every transition between 2025 and 2036, tested on the host). The log says `clock chip written too: local time, -5 h 00 min from UTC`.

**`rtc=local` and `init`.** `init` corrects a local-time chip once, early (`setup_clock`: the system clock was read as UTC and is shifted by the zone's offset). The lease usually arrives *before* `init` starts, so the first answer can set the clock
right and `init` then "corrects" a clock that is already right. The client therefore watches the clock (the wall clock minus the uptime changes only when somebody sets it): **if anyone sets the clock during the first five minutes of uptime it asks again a second later**:

```
[Sakuya] clock set from 10.0.2.2:55683: +7200.830 s (round trip 22 ms, stratum 2)
[Sakuya] the clock was set by 'init'                      <- the kernel's own line (SYS_settime)
[Sakuya] the clock was set by somebody else (-32400.549 s): asking the servers again
[Sakuya] clock set from 10.0.2.2:55683: +32401.746 s (round trip 2 ms, stratum 2)
```

That also means a `date -s` in the first five minutes does not stick while the network works (later it stays until the next scheduled sync). `ntp=off` for a machine where that is wrong.

## `/proc/ntp` and `sntp`

`cat /proc/ntp` (or `sntp -S`): `key: value` lines.

```
ntp: synced                       off | waiting for the network | syncing | synced | failing
network: up                       up | down: as the client last saw it (a card with a carrier and an address)
server: 10.0.2.2:55683            where the last good answer came from ("-" before the first)
source: cmdline                   dhcp | cmdline | builtin
stratum: 2
offset_ms: 7201696                how far off the clock was, before it was stepped
offset: +7201.696 s
delay_ms: 4
last_sync: 2026-11-03 06:16:42 UTC
last_sync_age_s: 12
cycles: 1                         rounds finished; syncs: good ones; failures: the others; fail_streak: failures in a row
syncs: 1
failures: 0
fail_streak: 0
external_steps: 0                 times somebody else set the clock while we watched
next_sync_in_s: 14388
interval_s: 14400
rtc: untouched (ntp.rtc=0)        or "written at 1793... (UTC)" / "(local time)"
rtc_local: no
dhcp_servers: 0                   how many servers the lease named
cmdline_servers: 10.0.2.2:55683
builtin_servers: time.cloudflare.com,time.google.com,pool.ntp.org,time.nist.gov
last_error: -
```

Root can write: `sync` (ask now), `server LIST` (replace the command line's servers until the next start; no list puts the command line's back), `builtin LIST`. `sudo sntp -n` writes `sync`, waits for the round to end and prints the file.
With `ntp=off` the file says `ntp: off` and writes fail with "No such device". The old `sntp`/`ntpdate SERVER` still work as before (`sntp -s`, `-w`).

## Options

| word | meaning |
|------|---------|
| `ntp=off` | nothing at all (no thread; the harness gives this to every test guest that does not name a server) |
| `ntp=HOST[:PORT][,...]` | the servers of group 2 |
| `ntp.interval=SECONDS` | between good syncs; default 14400, at least 16 |
| `ntp.rtc=1` | also write the clock chip (UTC, or local time with `rtc=local`) |
| `ntp.builtin=LIST` | replace group 3 (a test hook; `host:port` entries work) |

## What was tested

- **Host** (`python tools/hosttest.py discover -s tests/host -p "test_ntp.py" -v`, 35 tests, the C file built with gcc and UBSan and driven through ctypes against expectations computed in Python): timestamps and eras, the request bytes, every refusal above, the offset/delay formulas, `host:port` lists, DNS queries and answers (compression, CNAME, truncation and 2000 corrupted messages), POSIX zones against the tz database for six zones across every transition 2025 to 2036.
- **QEMU** (`python tools/qemu-slot.py -- python tests/run_all.py ntp_`, nine tests, a Python NTP/DNS responder on the host's loopback = 10.0.2.2 from the guest): `ntp_boot_sync` (the step, `/proc/ntp`, the packet on the wire, the chip untouched, `sntp -S`, a clock set by hand is put right), `ntp_liars_are_not_believed` (stratum 0, leap 3, stratum 16, wrong origin, 2019, root distance, two servers 2 h apart, two that agree), `ntp_off_does_nothing`, `ntp_dead_server_falls_back_and_never_blocks`, `ntp_rtc_local_and_chip` (init's correction and ours meet in the right answer), `ntp_names_hosts_and_dns` (`/etc/hosts`, DNS with `#PORT`, a CNAME, NXDOMAIN), `ntp_resyncs_by_itself` (`ntp.interval=16`, a server whose clock jumps, `ntp.rtc=1` as UTC), `ntp_asks_again_when_the_cable_comes_back` (QMP `set_link` down and up).
- `ntp_dhcp_option_42` is the ninth: user-mode networking's DHCP server never sends option 42, so the test builds a network of its own - a Python "router" at the other end of a virtio card wired to QEMU's `stream` netdev, answering ARP, DHCP (with option 42 naming two servers; it also checks that the guest *asked* for the option) and NTP on port 123 - and the lease's servers are used, cross-checked and the command line's are never asked.
- Not covered: anything about real networks (hostile or lossy paths, IPv6, NTS) and the Dell itself.

## The plan for the Dell (the lead's, after the next push)

0. **Push with `--no-cmdline` (or `--cmdline` with the Dell's real line).** Every test run rewrites `build/esp/cmdline.txt`, and the harness now puts `ntp=off` into it (as it has always put `nosshd`): a push that carries it would turn network time off on the Dell
   ([UPDATE.md](UPDATE.md), "cmdline.txt"). The Dell's own line needs no `ntp=` word at all.
1. `dmesg | grep Sakuya` - expect (the router is 192.168.1.1, no option 42 unless the router offers it): `clock set from time.cloudflare.com (...): +<about a year> s (round trip 15 to 40 ms, stratum 3; time.google.com (...) agrees within a few ms)`. If it says `no server answered`, `cat /proc/ntp` has the `last_error`; `the name could not be looked up` means the router's DNS (203.0.113.53) did not answer from the kernel, `ping` and `nslookup pool.ntp.org` from user space say whether it is the name or UDP 123 that is blocked.
2. `date` shows the right day and local time (`tz=EST5EDT,...` is on the installed command line); `cat /proc/ntp` shows `ntp: synced`, `source: builtin`.
3. `sudo sntp -n` asks again; `sntp -S` shows the new numbers. A second `dmesg` line `clock set ... +0.0xx s` shows how far the Dell's oscillator drifted since boot.
4. The chip stays wrong (it is Windows' to correct); to fix it from here put `ntp.rtc=1 rtc=local` into `cmdline.txt` (the Dell's `tz=` is already there) and expect `clock chip written too: local time, -5 h 00 min from UTC` (or `-4 h` in summer). Boot Windows once afterwards and check that its clock is right too. This was **not** tried on the Dell.
5. To keep the traffic on the LAN: `ntp=192.168.1.1` (if the router answers NTP: `sntp 192.168.1.1` says), or turn it all off with `ntp=off`.

## Files

`kernel/net/ntp.c` (thread, sockets, names, `/proc/ntp`), `kernel/net/ntp_proto.c` and `kernel/include/ntp.h` (the part that needs no machine), `kernel/net/dhcp.c` (option 42: +19 lines), `user/bin/kappabox/net_sntp.c` (`-S`, `-n`),
`tests/case_ntp.py`, `tests/host/test_ntp.py`, `tools/qemu.py` (the harness gives test guests `ntp=off`) and `tests/case_install.py` (its one regular expression that spells the harness's words).

## State when paused / how to resume (2026-11-03, the owner's budget ran out)

**Done, committed and tested in QEMU on `agent/clock-sensors`:** everything in Part 1 of `the project's internal briefs`: the SNTP client as a kernel thread (the lease's option 42, then `ntp=`, then the built-in public list; names through `/etc/hosts` and DNS;
two answers must agree; every refusal above), the 4-hour resync with back-off, `/proc/ntp` and the commands written to it, `sntp -S` / `-n`, the chip untouched unless `ntp.rtc=1` (local time with `rtc=local`), the watch that keeps `init`'s `rtc=local`
correction from spoiling the first answer, and the quick resync after a cable is plugged back in. Tests: 35 host tests and nine QEMU tests (`ntp_*`) pass, and so do the neighbours that touch the same code (`net_nic_*`, `net_sntp_host`, `net_link_virtio`, `time_*`, `manual`).
**Nothing has run on the Dell** or on any real network: the steps above ("The plan for the Dell") are the first real test.

**Not done / open:**
- *The owner's decision:* with nothing configured the kernel now asks public servers (see the box at the top). If that is not wanted, make group 3 empty by default (one line: `BUILTIN_SERVERS` in `ntp.c`) and say `ntp=` in the Dell's `cmdline.txt`.
- No slewing (`adjtime`): every answer is stepped, so the clock can jump back by a few milliseconds at a resync; a gentle correction for small offsets would be the next refinement. Leap seconds are not announced to anything (the clock is Unix time).
- The five-minute watch also undoes a `date -s` typed in the first five minutes while the network works. Narrowing it to machines with `rtc=local` is a three-line change in `watch_clock()` if that surprises anyone.
- `tools/suite.py` puts `ntp_*` and `thermal_*` into its catch-all group (`rest`); a named group would balance the runs. `ntp_liars_are_not_believed` and `ntp_resyncs_by_itself` take a minute or more on a loaded PC (a sync every 16 s of *guest* time); `TOUHOU_TIMEOUT_SCALE=3` is the cure.
- `power_restart_ways` failed once in a regression run with its log cut in the middle of a line (the serial reader had not caught up when QEMU exited: a race in that test under load, nothing to do with the clock); it passes alone.

**To resume:** `git switch agent/clock-sensors`; `python tools/build.py`; host tests `python tools/hosttest.py discover -s tests/host -p "test_ntp.py" -v`; QEMU `python tools/qemu-slot.py -- python tests/run_all.py ntp_`
(one run at a time per checkout: they share `build/esp`). Then the Dell plan above, and push with `--no-cmdline`.

## Seen on the Dell (2026-11-03, build 3fdee2b, the first start of that build)

The clock chip said 10 May 2025; 8 seconds after the network came up `/proc/ntp` showed `ntp: synced`, `server: time.cloudflare.com (162.159.200.123)`, `source: builtin`, stratum 3, delay 9 ms, a step of +46771597 s, `syncs: 1`, `failures: 0`, next sync in 14391 s, and `date` read the right day. The router's DHCP offered no NTP server (`dhcp_servers: 0`).
The hardware clock was left alone (`rtc: untouched (ntp.rtc=0)`), so after the next restart the chip still starts stale and the sync repeats; `hwclock -w` once, or `ntp.rtc=1` on the command line, would write the chip. That is the owner's choice.
Certificates now check out without a manual `sntp -s`: `https_get` has the right date from the first minute (it still needs `/etc/ssl/cacert.pem` copied in after each start).
