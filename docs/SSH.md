# SSH: coming in over the network

TouhouOS has an SSH server, `sshd` (`user/sbin/sshd/`). From another computer on the network you log in with an ordinary SSH
client and get a shell, or run one command; a pipe carries files in both directions. It is the way to look at, and to change, a
machine that has no keyboard within reach, and it is the transport of the remote update ([UPDATE.md](UPDATE.md)).
The other direction, going from this system to other machines, is the client (`ssh`, `ssh-keygen`, `ssh-keyscan`, `scp`, `sftp`): [SSH_CLIENT.md](SSH_CLIENT.md).

It is not OpenSSH's code. It is a small SSH-2 server written for this system, with one set of algorithms: exactly what OpenSSH
itself picks first, so every current client works and nothing old is kept alive.

| | |
|---|---|
| Key exchange | `curve25519-sha256` (and its old name `curve25519-sha256@libssh.org`), with OpenSSH's *strict KEX* against the Terrapin attack |
| Host key | `ssh-ed25519` |
| Cipher | `chacha20-poly1305@openssh.com` in both directions (it carries its own integrity check, no separate MAC) |
| Login | public key only, `ssh-ed25519` keys. No passwords, no `root`. |
| Channels | `session`: a shell, a command, with or without a terminal; window changes; signals; exit status; stderr kept apart |
| Subsystem | `sftp` (SFTP version 3, `user/bin/sftp-server.c`, run as the resident): the stock `sftp`, `scp` (OpenSSH 9 and later speak SFTP) and graphical clients work |
| Port forwarding | `direct-tcpip`: `ssh -L`, `-D` (SOCKS), `-W`, `-J`, to this machine only unless `sshd.forward=any`; `ssh -R` (`tcpip-forward`, `forwarded-tcpip`) only with `sshd.remoteforward=local`. See [Port forwarding](#port-forwarding) |
| Not there | agent and X11 forwarding, Unix-socket forwarding (`direct-streamlocal`), IPv6 targets, other ciphers or key types |

## Quick start

The development PC has a key pair for it, `~/.ssh/touhou_ed25519` (made without a passphrase, so scripts and Claude can use it;
`ssh-keygen -p -f ~/.ssh/touhou_ed25519` adds one). Its public half is `assets/local/ssh/marisa.pub`, which the image builder puts
into the boot image as `/etc/ssh/authorized_keys/marisa` (`assets/local/` is never committed). So after flashing, or on an installed
system:

```
ssh -i ~/.ssh/touhou_ed25519 marisa@ADDRESS                    # a shell
ssh -i ~/.ssh/touhou_ed25519 marisa@ADDRESS 'sudo poweroff'     # one command
```

`ADDRESS` is what `ifconfig` on the machine shows for `eth0` (or look at the router's list of clients). The very first connection
asks whether to trust the host key; compare its fingerprint with the one in the machine's log:

```
dmesg | grep 'sshd.*host key'
[sshd] host key ssh-ed25519 SHA256:EJnOOVux1FHDytrkOkUiQdf9fk9M3rWXGl9svi+O3B8 (new, kept on the HUMANVILLAGE volume)
```

`-o StrictHostKeyChecking=accept-new` accepts a new host and still refuses a changed one. Files travel with the usual tools:

```
scp -i ~/.ssh/touhou_ed25519 marisa@ADDRESS:/HumanVillage/TOUHOU.LOG .
scp -i ~/.ssh/touhou_ed25519 -r somedir marisa@ADDRESS:/tmp/
sftp -i ~/.ssh/touhou_ed25519 marisa@ADDRESS
```

or through a pipe (in PowerShell 5 use `cmd /c` for the redirection: its own `>` writes text, not bytes):

```
cat build/esp/kernel.elf | ssh marisa@ADDRESS 'cat > /tmp/kernel.elf'
ssh marisa@ADDRESS 'cat /HumanVillage/notes.txt' > notes.txt
```

`marisa` is the account that may use `sudo` (without a password), so a login as `marisa` is a login with the power of root; guard the
private key like a root password. `reimu`, the default desktop account, may log in too once a key is listed for her, but cannot use
`sudo`.

## Who may come in

A key logs in as NAME when its line is in `/etc/ssh/authorized_keys/NAME` or in `~NAME/.ssh/authorized_keys`:

| File | Kept across restarts | Must belong to | Written by |
|------|----------------------|----------------|------------|
| `/etc/ssh/authorized_keys/NAME` | no, it comes from the boot image | root | the image builder, from `assets/local/ssh/NAME.pub` |
| `~NAME/.ssh/authorized_keys` | yes, on the HUMANVILLAGE volume | NAME or root | the resident, or `marisa` over ssh |

A file that others can write, or that belongs to someone else, is ignored with a line in the log. Only `ssh-ed25519` lines are
read; a line with options (`command=...`) in front is skipped, because the options could not be honoured. The files are read at
every login. Adding a second PC is therefore:

```
ssh marisa@ADDRESS 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys' < other_key.pub
```

**Taking access away:** remove the line from the volume's `~/.ssh/authorized_keys`; to take away the key in the boot image, delete
`assets/local/ssh/marisa.pub` and build a new image. (The image copy is the one that comes back after every reflash, so a key that
must be gone for good has to leave the image too.)

`root` is refused whatever the files say, and so is a resident whose shell does not exist. An unknown name and a key that is not
listed look the same from outside (a refused key), so the server does not tell which residents exist.

## The host key

Made on first start (32 random bytes from the kernel's generator, see below) and kept as 64 hex digits in
`/HumanVillage/.system/ssh_host_ed25519_key`, which belongs to root (0600) like everything under `.system`. With the key on the volume
the fingerprint stays the same across restarts, updates and reinstalls of the system files. Without a HUMANVILLAGE volume, or on a
read-only one, it lives in RAM and is new at every boot (the log says so); the client then warns that the host key has changed.
Delete the file to make a new key. A key file that exists but cannot be read as a key (damaged) is NOT replaced: the server runs on a temporary key
kept in RAM and says so in the log, because a silently new key would make every client's `known_hosts` cry wolf.

## Limits and behaviour

- At most 16 connections at once (a 17th is closed at once) and at most **4 from one address**. A connection must finish its login within
  **30 seconds** and gets six failed attempts and 20 requests of any kind (a client that only asks about keys does not go on for ever; the
  method `none` counts once). A packet may take 30 seconds from its first byte to its last, and a peer that stops *reading* is given 60
  seconds per packet (the socket is non-blocking, so a stalled client cannot park the server's send in the kernel).
- **Throttle**: five connections from one address that end without a login within a minute and the address is not served for the next
  minute (one line in the log, not one per attempt); a login wipes the slate. This keeps a scanner on the LAN from using up places and from
  filling the log (which is also written to the stick). `sshd.throttle=N` and `sshd.peraddr=N` on the kernel command line change the two
  numbers (0 switches the limit off; the tests need that, because QEMU shows every connection as coming from one address).
  **A connection that only reads the banner and goes away counts as a failure**, so a script that watches the port ("is ssh answering yet?") locks itself out
  after five looks, and then reports a machine that is running as down (2026-10-25: this got a push confirmed on the start that had only received it).
  Watch a machine with ICMP ping, or with a login that looks for a token left in `/tmp` (RAM: a restart is proved by the token being gone); see [UPDATE.md](UPDATE.md).
- Every timeout of the server runs on the **monotonic clock** (uptime), not the wall clock: `sntp` or `date -s` can step the wall clock by years
  (the Dell's says 2025) and a deadline measured on it would fire at once or never.
- The packet counter, which is also the cipher's nonce, is never allowed to come round: a connection that has not re-keyed by 2^32-256 packets is
  dropped (every OpenSSH re-keys long before).
- `authorized_keys` files are opened without following links and checked on the descriptor that is read (no swap between check and use).
- One process per connection; a session's programs are children of it. When the connection ends, the programs are sent `SIGHUP`
  and, after a second, `SIGKILL`.
- A client that stays silent is asked whether it is still there every 60 seconds (`keepalive@openssh.com`); after three unanswered rounds the connection and its programs are dropped, so a laptop that fell asleep does
  not hold a place among the 16 for ever. `sshd.keepalive=SECONDS` on the kernel command line changes the interval (0: never).
- At most four session channels per connection (and `sshd.forwardmax` forwarded ones, below); `ControlMaster` multiplexing works within that.
  Re-keying (the client does it after 1 GB, or `-o RekeyLimit=`) is done in the middle of a transfer.
- Flow control follows the protocol's windows (64 KiB toward the program); a program that does not read its input slows the sender
  down instead of losing data. A terminal session loses input only the way a terminal does.
- The login shell is the resident's (`/bin/gapsh -l`); a command runs as `SHELL -c COMMAND`. The environment is rebuilt from
  scratch (`HOME USER LOGNAME SHELL PATH HOSTNAME TERM SSH_CONNECTION SSH_CLIENT SSH_TTY`).
- When the **console** session of a resident ends, every process of that resident is sent home (init does that, so that nothing
  of a previous user lingers): that includes their ssh sessions. Using `marisa` over ssh while someone else holds the console is fine.

## Port forwarding

An ssh connection can carry other connections inside it. With `ssh -L`, the client listens on a port of its own machine and every connection made
there travels through the encrypted ssh connection to the server, which connects onward to a target it is told (a `direct-tcpip` channel, RFC 4254
section 7.2). This is how a service that listens only on this machine is used from another one without being opened to the network: the remote
desktop (VNC) is the first of them.

```
ssh -i ~/.ssh/touhou_ed25519 -N -L 5900:127.0.0.1:5900 marisa@ADDRESS     # then a VNC viewer on the PC connects to localhost:5900
ssh -N -L 8080:localhost:80 marisa@ADDRESS                                   # a web page that listens on the machine's loopback
ssh -N -D 1080 marisa@ADDRESS                                                # a SOCKS5 proxy: each connection names its own target
ssh -W 127.0.0.1:22 marisa@ADDRESS                                           # standard input and output are one forwarded connection
ssh -J marisa@ADDRESS someone@other-host                                     # a jump host (needs sshd.forward=any: see below)
ssh -N -R 9000:localhost:8000 marisa@ADDRESS                                 # the other way round (needs sshd.remoteforward=local)
```

`-N` means "no shell, only the forwards". The words on the kernel command line (`cmdline.txt`) decide what is allowed:

| Word | Means |
|------|-------|
| `sshd.forward=local` | **the default**: the targets are this machine only: `127.0.0.0/8`, `0.0.0.0` (the stack takes it for "this host"), and the current addresses of its own interfaces |
| `sshd.forward=any` | anything the machine can reach (a jump host, `-J`, and `-D` for browsing need it) |
| `sshd.forward=off` | no forwarding at all; an unknown value counts as `off` (a typo must not give more reach than was meant) |
| `sshd.remoteforward=off` | **the default**: `ssh -R` is refused |
| `sshd.remoteforward=local` | `ssh -R` listens on `127.0.0.1` only, whatever address the client asks for ("every address" gets the loopback, as OpenSSH's own server does without `GatewayPorts`; any other address is refused), never on a port below 1024, at most four ports per connection; port 0 lets the server pick one, which the client prints |
| `sshd.forwardmax=N` | forwarded channels (both kinds) per connection: 16 unless said, at most 32 (`0` means the most, 32) |
| `sshd.forwardtotal=N` | forwarded channels of the whole server: 64 unless said, `0` for no such cap. The connections are processes of their own, so the count is kept in slot files in `/var/run/sshd` (root's, 0700); the listening process cleans up after a connection that died |

The policy is checked on the address that is actually connected to, after a name was looked up: a name whose address is elsewhere is refused
under `local` like the address itself, before anything is connected. A refused open reaches the client as the protocol's reason, and OpenSSH
prints it: `channel 2: open failed: administratively prohibited: only this machine may be reached (sshd.forward=local)`, `connect failed:
Connection refused`, `connect failed: the target did not answer in time`, `connect failed: no such host`, `resource shortage: too many
forwarded channels on this connection (sshd.forwardmax=16)`. IPv6 targets are refused (the stack is IPv4).

How it behaves:

- Everything runs in the connection's one poll loop, next to the sessions, and nothing waits there: a **name** is looked up by a child process
  of its own (it forgets the server's keys and takes the resident's rights before it reads an answer from the network; at most four at a time,
  20 seconds at most), a **connect** is non-blocking with a deadline of 10 seconds. A name server that never answers, or a target that never
  answers, keeps one channel waiting, not the shell next to it (`ssh_forward_any_names_socks` times an echo while both happen).
  `localhost` and the names under it are the loopback without asking anyone.
- **Flow control** follows the windows: nothing is read from the target beyond what the client's window allows (a client that stops reading
  stops the target, whose socket buffer fills up), and the window the client gets (64 KiB) is opened again only for bytes the target's socket
  has taken, so at most 64 KiB of a channel ever wait in the server.
- **EOF** travels both ways (a target that ends its side while the client still sends is fine, and so is the other way round); the channel is
  closed when both directions are done, when the target resets the connection, or when the client closes it. The bytes a client sent just
  before its close still reach a slow target (for up to 30 seconds), as OpenSSH's own server does.
- The **timeouts, keepalives and packet counter** of the connection are those of a session: a forward is one more channel. A forward to this
  machine's own sshd (`-W 127.0.0.1:22`, or `-J` with this machine as the jump host and itself as the target) is a new ssh connection from
  `127.0.0.1` for the per-address limit and the throttle above.

**Security notes.** The development key (`touhou_ed25519`) is the key of `marisa`, who may use `sudo`: it is root's key in all but name, and
forwarding does not change that. Forwarding adds **no rights**: whoever holds a key that may log in has a shell and could connect anywhere with
`nc`. It adds **reach**: a service that listens on `127.0.0.1` and trusts "connections from this machine" (no password of its own) is open to
every key that may log in, from wherever that key is; with `sshd.forward=any` the machine is also a door into the network it stands in (the
router's page, the other PCs); and a `-R` port on the loopback is open to every resident of this machine, not only to the one who asked for it.
So: keep `local` unless a jump host is needed, keep `-R` off unless it is needed, and give the services behind a forward passwords of their own.
Authentication is unchanged: public keys only.

The log has one line when a channel opens or is refused and one when it ends: who, the target (and the address a name led to), the originator
the client gives, the result and the byte counts, never the bytes themselves.

```
[sshd] port forwarding to this machine only (sshd.forward=local), at most 16 channels per connection and 64 in all
[sshd] remote forwarding (ssh -R) off (sshd.remoteforward=off)
[sshd] 10.0.2.2: marisa: forward to localhost:5900 = 127.0.0.1 (for 127.0.0.1:51234): open
[sshd] 10.0.2.2: marisa: forward to localhost:5900 = 127.0.0.1 (for 127.0.0.1:51234): closed, 1834 bytes to the target and 5242880 from it
[sshd] 10.0.2.2: marisa: forward to 10.0.2.2:80 (for 127.0.0.1:51240): refused: only this machine may be reached (sshd.forward=local)
[sshd] 10.0.2.2: marisa: remote forward 127.0.0.1:9000 (asked for localhost:9000): listening
```

A connection that opens many channels (a browser behind `-D`, or a flood) gets lines for its first 20 and then one line at its end that counts
the rest ("... 214 more forwards were opened and 3 refused without a line each"): the log is also written to the stick.

## What is in the log

Under the name `sshd`, in `dmesg` and in `TOUHOU.LOG` on the volume (nothing secret is ever written):

```
[sshd] host key ssh-ed25519 SHA256:... (read from the HUMANVILLAGE volume)
[sshd] listening on port 22
[sshd] 10.0.2.2: connected, client SSH-2.0-OpenSSH_10.3
[sshd] accepted publickey for marisa from 10.0.2.2 port 51234: ED25519 SHA256:SffVhhcb0GorAp1qsFl9k447iwILRJeNyzmI2Q4uFQ8
[sshd] 10.0.2.2: marisa: exec echo (10 bytes; sshd.logcmds shows them) opened
[sshd] marisa: exec echo (10 bytes; sshd.logcmds shows them): session ended, exit status 0
[sshd] failed publickey for nobody-here from 10.0.2.2 port 51240 (no such resident, or the key is not authorized)
```

A command line can hold things that do not belong in a log, so an `exec` request is logged as its first word and its length; `sshd.logcmds` on
the kernel command line logs the whole command.

The fingerprint on the `accepted` line is the key's (`ssh-keygen -lf touhou_ed25519.pub` prints the same), which tells the keys of
several PCs apart.

## Switching it off

`nosshd` on the kernel command line (`cmdline.txt`) keeps `init` from starting it. `kill` its process to stop it for the moment;
`init` starts it again within a moment (unless it keeps dying at once). To change the port, start `sshd -p PORT` by hand after
stopping the other one. The test harness adds `nosshd` to every virtual machine unless the test says `sshd`.

## How it is built

```
user/sbin/sshd/wire.c        the protocol's data types, base64
user/sbin/sshd/transport.c   identification, packets (chacha20-poly1305@openssh.com), key exchange, key derivation, strict KEX
user/sbin/sshd/auth.c        host key, authorized_keys, the publickey method
user/sbin/sshd/session.c     channels, ptys and pipes, flow control, the child processes
user/sbin/sshd/forward.c     port forwarding: direct-tcpip and forwarded-tcpip channels, the policy, the caps, lookups in a child
user/sbin/sshd/main.c        options, the listening socket, one child per connection
user/bin/sftp-server.c       the sftp subsystem, a program of its own that runs as the resident
user/lib/sshcrypto/          SHA-512, X25519, Ed25519, ChaCha20, Poly1305 (the hash for the key exchange is libgap's SHA-256)
kernel/lib/random.c          the kernel's generator: the keys come from here
```

**The crypto library** is plain C (TweetNaCl-style field arithmetic, poly1305-donna's limbs), checked in the OS by `sshkat` against
vectors made with an independent library (`tools/gen_sshcrypto_vectors.py` makes `user/bin/sshkat_vectors.h`): the RFC 7748, 8032
and 8439 vectors plus random ones, corrupted and malleable signatures (`S + L`), small-order points, and every padding edge of
SHA-512. It is not hardened against timing attacks beyond comparing tags in constant time; the keys it handles are the server's
own and per-connection ones, on a machine on a home network.

**The random numbers** matter, since the host key and every connection key come from them. The kernel's generator is a ChaCha20
stream with fast key erasure: after every refill the key is replaced by the generator's own output, so nothing handed out says
anything about what comes next. It starts from RDRAND (when the CPU has it), 32 timestamp-counter samples taken a few microseconds
apart, and is stirred by interrupt and input timing; every 256 KiB of output it reseeds from RDRAND again. `sshkat` runs a sanity
check on its output (bit balance, byte histogram, repeats, short reads). That is the check that catches a broken generator, not a
proof of quality.

**Strict key exchange.** The cipher in use is the one the Terrapin attack hits. The server announces
`kex-strict-s-v00@openssh.com`; when the client announces its side, the KEXINIT must be the first packet, nothing but key-exchange
messages may come until the new keys are in, and both sequence numbers restart from zero at every NEWKEYS. A client that
announces strict KEX and sends anything in front is disconnected (`ssh_garbage_and_limits` tries it).

## Tests

`python tests/run_all.py ssh` and `sshcrypto` (on the shared PC through `python tools/qemu-slot.py -- ...`). The `ssh_*` tests need the OpenSSH
client on the host (`ssh`, `ssh-keygen`, `ssh-keyscan`, and `scp` and `sftp` for one of them; Git for Windows has them all) and fail with a message
where it is missing; `sshcrypto_known_answers` needs nothing. The `ssh_forward_*` tests drive the host's `ssh` with `-L`, `-W`, `-J`, `-D`, `-R` and
`-O cancel`; their services are `nc`, `httpd` and the sshd of the guest, and Python servers on the host, which the guest reaches as 10.0.2.2. `python tests/host/test_push_update.py` and `python tests/run_all.py update_` cover the update side.

| Test | What it shows |
|------|---------------|
| `sshcrypto_known_answers` | the crypto library and the kernel generator, inside the OS |
| `ssh_login_and_exec` | the real OpenSSH client logs in with its key, runs commands, gets the exit status and stderr; the log names the key |
| `ssh_refusals` | a key nobody listed, an unknown resident, `root`, a resident without keys, a key file others can write or the resident owns in `/etc`: all refused; the right key then works |
| `ssh_pty_shell` | `ssh -tt`: prompt, environment, Ctrl+C reaching the foreground job, a second session at the same time, `exit` |
| `ssh_bulk_and_rekey` | 3 MB up and 3 MB down, intact, with the client re-keying every 256 KB |
| `ssh_host_key_and_home_keys_persist` | the host key is kept on the volume (and is what a client sees), the same after a restart; a key in `~/.ssh/authorized_keys` on the FAT volume works |
| `ssh_sftp_and_scp` | `scp` both ways (1.5 MB intact, a directory tree), `sftp` in batch mode: ls, mkdir, put, rename, chmod, symlink, get, rm, rmdir; `/etc/shadow` stays root's; the files belong to the resident |
| `ssh_silent_client_is_dropped` | the virtual cable is pulled: three unanswered probes and the connection, its shell and its place are gone; the link back brings ssh back |
| `ssh_garbage_and_limits` | not SSH, absurd lengths, bad padding, no common algorithm, the Terrapin shape, bad key-exchange values, 17 connections: refused without harm, nothing left running |
| `ssh_forward_local_transfer` | `ssh -L` to `nc` in the guest: 5 MB down and 5 MB up at once through two forwards of one connection, re-keying every MB, intact; EOF both ways; a reader that pauses for 5 s; `-W` to the guest's own sshd (loopback and own address); `-J` through the guest to itself carries 2 MB both ways through `cat`; the log lines |
| `ssh_forward_policy_local` | the default policy: an address elsewhere and a name that leads there are refused (nothing reaches the target), refused port, unknown name, IPv6, a non-name, a target that never answers (the loopback drops every frame) fails at the 10 s deadline; `-R` is off |
| `ssh_forward_off` | `sshd.forward=off`: refused as administratively prohibited; logins work as before |
| `ssh_forward_any_names_socks` | `sshd.forward=any`: 4 MB each way at once through one channel to a Python echo server on the host; a name through the resolver (a Python name server); `-D` SOCKS5 by name and by address; a silent name server and an unanswered connect wait in the background while an echo on the same connection answers at once |
| `ssh_forward_ends` | the guest's service killed in the middle of a download and of an upload, the client resetting its end, a close that comes while 34 KB still wait for a slow service (they arrive), and the next forward of the same connection works |
| `ssh_forward_caps_and_flood` | `sshd.forwardmax=4 sshd.forwardtotal=6` with two connections: four, then two, then refusals as resource shortage; places come back; 150 opens that end at once leave every place free, the server serving, and a log of bounded length with its summary |
| `ssh_forward_remote` | `sshd.remoteforward=local`: listeners on the loopback only, refusals (another address, port 80, a port in use, a fifth listener), 1 MB both ways from the guest through the client to the host, a port the server picks, a channel the client refuses, `ssh -O cancel` through a control master |
