# Updating from far away

The machine this system is installed on sits behind a home router, is switched off most of the time, and the PC that builds the system travels. A push over ssh ([UPDATE.md](UPDATE.md)) needs
the two to be on the same network. This page is the way around that: **the dev PC reaches the machine through a jump host, and the jump host switches the machine on.** Nothing has to change on the
machine: it works with the system that is installed now, because the machine still just sees an ssh login from a PC that happens to arrive through a forward.

```
                 internet / VPN                         the machine's LAN
  dev PC  ----------------------------->  jump host  ------------------->  machine (sshd)
  (builds, holds the keys)                 always on, sshd + python3         (off most of the time)
        python tools/remote-update.py push        |  sends the Wake-on-LAN packet
```

## What it needs

* **A jump host**: a PC that is always on, on the machine's LAN, that the dev PC can ssh to from outside (a VPN such as Tailscale or WireGuard, or a forwarded port), with TCP forwarding allowed for
  the account and `python3` installed. It is a plain ssh login: nothing is installed on it, not even the wake script (that is piped over ssh each time).
* **The dev PC's key** accepted by that account, and the jump host's own host key already known to the dev PC (`ssh USER@JUMPHOST` once). A jump host whose key is not known is refused, not trusted.
* **The machine's host-key fingerprint**, to pin it: `ssh-keyscan -t ed25519 ADDRESS | ssh-keygen -lf -` while the dev PC is on the machine's LAN (or the fingerprint the installer showed).
* On the machine: **Wake-on-LAN works** (the system arms the network card when it powers off, [POWER_AND_TIME.md](POWER_AND_TIME.md); the firmware needs "Wake on LAN" set to LAN and, on some
  boards, "Deep Sleep Control" off), and its **address is stable** (a DHCP reservation in the router; the tool can also find it again by its MAC address, below).

## Setting it up, once

```
python tools/remote-update.py setup --via USER@JUMPHOST --via-key ~/.ssh/KEY --host MACHINE_ADDRESS --mac MACHINE_MAC \
        --wake-bind JUMPHOST_ADDRESS_ON_THE_LAN --broadcast LAN_BROADCAST --fingerprint SHA256:...
```

`--host` is the machine's address *as the jump host sees it* (its LAN address). `--wake-bind` is the jump host's own address on that LAN, which the magic packet is sent from (a jump host with a VPN
or docker bridges must not pick its source by itself); `--broadcast` is the LAN's broadcast address (the limited broadcast 255.255.255.255 is not used: on such a host it may leave by any interface).
The command writes `~/.touhou-os/remote.json` (paths and addresses, no secret; it stays out of the repository), logs in to the jump host, checks that it has python3 and that it reaches the machine from the
address given as `--wake-bind`, and, if the machine is on, fetches its host key and keeps it when the fingerprint matches (when it is off, that happens the first time it is on). Every setting can also be
given on the command line of any later command, and the command line wins.

## Using it

```
python tools/build.py                                     # in the main checkout: the image carries the keys (docs/UPDATE.md, "The image must let the pushing key in")
python tools/remote-update.py status                      # is the machine on? what does it run?
python tools/remote-update.py push                        # wakes it if it is off, pushes, restarts it on trial, confirms
python tools/remote-update.py push --off-after            # ... and switches it off again once the update is confirmed
python tools/remote-update.py push --cmdline "..." -- --replace-trial      # options after `push` go to tools/push-update.py
python tools/remote-update.py wake                        # only wake it and wait until it answers a login
python tools/remote-update.py off                         # sudo poweroff
```

What `push` does, in order: it checks what needs no network first (`push-update.py --dry-run`: the files, the compressed initrd, the key guard), so that the machine is not woken for a push that would be
refused; asks the jump host to log in and to prove it has python3; looks for the machine with a login; if it does not answer, has the jump host send the magic packet (a burst for 20 seconds, once more
after 100 seconds without an answer) and waits for a login for up to five minutes (`boot_wait`); after two minutes it asks the jump host where the machine has gone, by MAC address (the ARP table, after a
datagram to each address of the /24), and goes on with the address it finds. Then it is `push-update.py`, unchanged: the files, the trial restart, the proof of the restart by a token in `/tmp`, the confirmation.
The exit status is that of `push-update.py` (0 done, 3 the machine came back on the PREVIOUS files, 4 it did not come back in time); 5 means the jump host could not be used, 6 that the machine did not come up.

`tools/push-update.py` and `tools/wol.py` take the same route on their own: `--via [USER@]HOST[:PORT]`, `--via-key FILE`, `--via-option OPT` (an ssh option for the jump host only). `wol.py --via` pipes itself to `python3 -`
on the jump host (`--remote-command CMD` runs an installed helper instead, for a login that may run nothing else).

## What protects what

* **The machine's own identity is checked on the dev PC, end to end.** Its ssh host key is fetched through the same route without authenticating, compared with the fingerprint in the config, and only then
  written to `~/.touhou-os/known_hosts` under an alias (so a new address does not matter); every later connection uses that file. A key with another fingerprint is refused and nothing is sent. The jump host
  sees only ciphertext: it cannot read the update, change it (every file also carries a SHA-256 that the machine checks) or answer for the machine.
* **The key that may push stays on the dev PC.** The jump host only forwards a TCP connection and sends a packet. If it is wanted to do nothing else, give it a key that is restricted in `authorized_keys` to
  `restrict,port-forwarding,permitopen="MACHINE_ADDRESS:22"`, and a second key with a forced command that only sends the packet (use `--remote-command`).
* **The jump host's own host key must be known** (`StrictHostKeyChecking yes` for it): on an unknown network nothing is trusted at first sight.
* **The machine's safety net is the same as for a push on the LAN** ([UPDATE.md](UPDATE.md), "What each failure leaves"): the new files run on trial, the machine restarts itself after five minutes
  without a confirmation and, after the second start, the loader brings the previous files back. What differs is only who is there to see it.

## What can go wrong

| Symptom | Cause and what to do |
|---|---|
| `the jump host ... did not answer as needed` (exit 5) | the jump host is off or unreachable, does not take the key (`Permission denied`), or does not know its key on this PC (`Host key verification failed`: `ssh USER@JUMPHOST` once), or has no python3. The message is ssh's own. |
| `the machine did not answer a login within 300 s after the wake-up` (exit 6), power light off | the packet did not reach the card: the firmware needs Wake on LAN = LAN (and Deep Sleep Control off), the system must have been switched off by its own `poweroff` (that arms the card), and `--wake-bind` and `--broadcast` must name the machine's LAN. A real test needs someone to watch the machine once. |
| the same, power light on | the firmware started another system (a Windows update can put its own boot entry first), or the new system's network did not come up. Only someone at the machine can tell which. |
| `the machine is at ADDRESS now` | its DHCP lease changed while it was off; the run goes on with the new address (`setup --host ADDRESS` keeps it). A reservation in the router makes this go away. |
| `the machine's host key is not the pinned one` | another machine answers at that address, or the machine was reinstalled (it has a new key): nothing is sent. Check, then `setup --fingerprint` with the new one. |
| exit 3, `came back on the PREVIOUS files` | the new files did not come up; the machine is on the old system and reachable. `push-update.py`'s message and `status` say more. |
| the dev PC loses its connection during the five minutes after the restart | the update is not confirmed: the machine restarts after five minutes (a counted start), and again; the third start is the previous system. Run `push` again when the connection is back (it confirms a start on trial that carries its version). |

## Limits

The jump host has to be up (there is no way to switch *it* on from far away), and so does the LAN. The dev PC has to stay connected from the restart until the confirmation (about two minutes; the machine
waits five). A new system whose network does not work cannot be confirmed, so the machine falls back after about ten minutes. Pulling updates (the machine fetches a signed manifest from a server by itself,
the parked design in [UPDATE.md](UPDATE.md)) would take the dev PC out of that window altogether: not built.

## Tests

`python tools/hosttest.py discover -s tests/host -p "test_remote_update.py" -v`: the jump-host config, the piped wake script (run standalone, as it runs on the jump host), the discovery by MAC against a
fake ARP table, and the whole flow of `remote-update.py` against a fake ssh: a machine that is on is not woken, one that is off is woken and waited for, one that never answers gets a second packet and a
report, a wrong host key is refused before any login, a refused push does not wake the machine. `python tests/run_all.py update_push_through_a_jump_host` (QEMU, the real OpenSSH client): the machine's own
sshd is the jump host (forwarding to itself), the host key is pinned and a wrong fingerprint refused, `push` runs the whole update through the jump host and confirms it, `off` powers the machine off.
The wake-up itself can only be tried on the real machine; it was, on 2026-11-18: wake, pin, push through a jump host, confirm, power off, wake again, each as described above.
