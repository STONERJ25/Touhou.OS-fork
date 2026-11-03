# SSH client: going from TouhouOS to other machines

`ssh`, `ssh-keygen`, `ssh-keyscan`, `scp` and `sftp` (and the manual pages `ssh(1)`, `ssh-keygen(1)`, `ssh-keyscan(1)`, `scp(1)`, `sftp(1)`,
`ssh_config(5)`, `known_hosts(5)`) let this system log in to another machine (the Forgejo server, a router, another PC), run a command
there, copy files, and carry ports. It is the counterpart of the server ([SSH.md](SSH.md)): the same SSH-2, the same one set of
algorithms, written separately on purpose (see *Tests* for why that matters).

| Program | Source | What it is |
|---|---|---|
| `ssh` | `user/bin/ssh/` (`main.c` options, `session.c` terminal and loop, `forward.c` port forwarding) | login, a command, forwarding |
| `ssh-keygen` | `user/bin/ssh-keygen/` | make keys (ed25519), show fingerprints and randomart, change passphrases, look after known_hosts |
| `ssh-keyscan` | `user/bin/ssh-keyscan/` | ask servers for their host keys |
| `scp`, `sftp` | `user/bin/scp/`, `user/bin/sftp/` | file copy and an interactive file session, over SFTP |
| the library | `user/lib/sshcrypto/src/sshc_*.c`, `include/sshc.h` | transport, authentication, channels, SFTP, known_hosts, key files, ssh_config; the same code in every program |

The library sits in `sshcrypto` next to the primitives it uses (SHA-512, X25519, Ed25519, ChaCha20-Poly1305) and shares no code with
`user/sbin/sshd`, which it must not link. No existing source file was changed (`docs/SSH.md` has one link line to this page).

## A worked example

On this system, once, to make a key and let the Forgejo server know it:

```
ssh-keygen -t ed25519                         # asks where (Enter) and for a passphrase; prints the fingerprint and a picture
cat ~/.ssh/id_ed25519.pub                     # this line goes into the server's authorized_keys (Forgejo: Settings > SSH keys)
ssh-keyscan -p 22 server.example.net > /tmp/hostkey
ssh-keygen -l -f /tmp/hostkey                 # compare this fingerprint with one you know from another source ...
cat /tmp/hostkey >> ~/.ssh/known_hosts        # ... and only then trust it
ssh git@server.example.net                    # Forgejo answers "Hi there, ..." and closes: key login works
ssh marisa@gensokyo 'uptime'                  # a command; its output and exit status come back
ssh marisa@gensokyo                           # a login shell (the terminal goes raw; ~. ends it, ~? lists the escapes)
scp -r photos marisa@gensokyo:/HumanVillage/marisa/
sftp marisa@gensokyo
ssh -L 5900:127.0.0.1:5900 marisa@gensokyo    # then a VNC viewer on 127.0.0.1:5900 reaches the other machine's VNC, encrypted
```

Without the keyscan step the first `ssh` shows the fingerprint and asks `yes/no/[fingerprint]`; typing the fingerprint you were given is
the careful answer. `-o StrictHostKeyChecking=accept-new` remembers new hosts without asking and still refuses a changed one.

## What is supported

| | |
|---|---|
| Key exchange | `curve25519-sha256` (and its older name `@libssh.org`); OpenSSH's strict key exchange (Terrapin) is on whenever the server announces it. New keys after 1 GiB of traffic (both ways together), `-o RekeyLimit=SIZE [TIME]`, `~R`, and whenever the server starts one |
| Host key | `ssh-ed25519` |
| Cipher | `chacha20-poly1305@openssh.com` both ways (no MAC: it carries its own), no compression |
| Login | public key (`ssh-ed25519`, from `~/.ssh/id_ed25519`, `-i`, `IdentityFile`; the key is offered first and the passphrase of an encrypted key is asked for only if the server would take the key), keyboard-interactive (RFC 4256), password (3 tries); `none` first, to learn what the server offers; banners are shown with control characters made harmless; `BatchMode` asks nothing |
| Key files | OpenSSH's `openssh-key-v1`, plain or with a passphrase (bcrypt_pbkdf and aes256-ctr / aes192-ctr / aes128-ctr / `chacha20-poly1305@openssh.com`); written files are read by OpenSSH and the other way round |
| Host keys | `known_hosts` (plain names, `[host]:port`, `*` `?` `!`, hashed `\|1\|` entries, `@revoked`; `@cert-authority` skipped), `/etc/ssh/ssh_known_hosts` too; `StrictHostKeyChecking` `ask` / `accept-new` / `yes` / `no`; `HashKnownHosts`, `UserKnownHostsFile`, `HostKeyAlias` |
| Session | pty or not (`-t`, `-tt`, `-T`, the default), raw terminal restored on every way out, window size at the start and on every change, `TERM` (nothing else is sent as an environment), stderr kept apart, the remote exit status as ours (255 for `ssh` itself failing, a signal, `~.`), `-N`, `-n`, `-s`, `-e`, escapes `~. ~? ~~ ~R ~V ~v ~#` |
| Forwarding | `-L`, `-R`, `-D` (SOCKS 4, 4a, 5); listeners on the loopback unless an address is given; windows as the only buffers |
| Files | `scp` and `sftp` over SFTP version 3: recursive copies, `-p`, remote wildcards, batch files, `ls get put mkdir rm rmdir rename chmod ln -s cd lcd`; 16 reads or writes of 32 KiB in flight |
| Configuration | `~/.ssh/config`, `/etc/ssh/ssh_config`, `-F`, `-o` with `Host`, `Match`, `Include`, `%` tokens ([ssh_config(5)](../initrd/ScarletDevilMansion/share/patchouli/5/ssh_config.pw)); first value wins |
| Keepalive and timeouts | `ServerAliveInterval` / `ServerAliveCountMax`, `ConnectTimeout` |

## What is not (and what is said when it is asked for)

- Other key exchanges, host key types and ciphers: RSA or ECDSA-only servers, AES, Diffie-Hellman. The message names what the server offered
  (`no matching host key type found. Their offer: ssh-rsa,...`). Most OpenSSH servers have an ed25519 host key and chacha20; a Go-based
  server (Forgejo's built-in) or an old router (Dropbear) may not.
- Agent forwarding and X11 (`-A -X -Y`: refused; `ForwardAgent yes` in a file: said to stay off), certificates, GSSAPI, hostbased login, security keys,
  password change on login, ProxyJump / `-J`, ProxyCommand (an error: it would send the connection somewhere else than the file says), `-W`,
  multiplexing, `-f`, IPv6 (this system has none), `SSH_ASKPASS`.
- A key that is not ed25519 in `~/.ssh` is skipped with a note; `ssh-keygen` makes ed25519 keys only (it reads and prints keys of any type).

## Security decisions

- **A changed host key is always refused**, whatever `StrictHostKeyChecking` says (OpenSSH's `no` would go on with a warning). The warning shows the new
  fingerprint and where the old key is on record; `ssh-keygen -R host` is the cure after a real reinstall. A `@revoked` key is refused for every host.
- **Unknown hosts**: asked on the terminal with the fingerprint; refused when there is no terminal to ask on (`BatchMode`, a script, a pipe).
- **No agent forwarding, no X11**, so a login cannot hand anything of this machine to the far side.
- **Private key files must not be readable by group or others** (checked on the open descriptor); one that is, is ignored with OpenSSH's warning.
  Passphrases and passwords are read from the terminal with echo off, a signal restores the terminal before it takes effect, and the buffers are wiped.
- **The passphrase is asked only for a key the server would accept**: the key is offered first, so a person is never asked to unlock a key for nothing.
- Banners and server text are printed with everything but printable ASCII and newlines turned into `?`, so a server cannot move the cursor, set the title or
  type into the terminal.
- Strict key exchange is enforced when announced (the server's KEXINIT must be its first packet, nothing else may come between KEXINIT and NEWKEYS,
  the sequence numbers restart at every NEWKEYS). A server that does not announce it is allowed, as in OpenSSH.
- Packet sizes, window sizes and counts are checked before use; the packet counter (the cipher's nonce) forces new keys long before it could repeat.
- The random numbers come from the kernel generator; the client stops rather than go on without them.

## Tests

Host tests run in WSL's Ubuntu (gcc, AddressSanitizer, UBSan; the sources are the ones the OS is built from, `-DSSHC_HOST` only changes
`sshc_random()`), against OpenSSH's own tools, the Python `cryptography` and `bcrypt` packages, and `tests/sshsrv.py`:

```
python tools/hosttest.py discover -s tests/host -p "test_sshclient.py"        # 54 tests, about 80 s
python tools/qemu-slot.py -- python tests/run_all.py sshclient_               # the guest tests, 7 of them
```

`tests/sshsrv.py` is an SSH-2 server written for the tests, separately from both the client and the OS's sshd, so that a misreading of the RFCs that
one author makes on both sides shows up as a failure. It has password, keyboard-interactive and public-key login, banners, server and client re-keys,
sessions with a toy shell, SFTP, `direct-tcpip` and `tcpip-forward`, and knobs to misbehave (a Terrapin-shaped KEXINIT, a bad signature, unsupported
algorithms, a frozen server). It is itself checked with the real OpenSSH client (`OpenSshAgainstTestServer`, `OpenSshSftpAgainstTestServer`).

| Host test class | What it shows |
|---|---|
| `Primitives` | SHA-1/HMAC, the Blowfish tables are the digits of pi, `bcrypt_pbkdf` equals the `bcrypt` package's, AES-CTR equals `cryptography`'s, base64, path tokens |
| `PublicKeys` | fingerprints, sizes, labels and randomart equal `ssh-keygen -l`/`-lv` for ed25519, RSA and ECDSA keys; every known_hosts decision (patterns, `!`, ports, case, revoked, CA, hashed entries made by `ssh-keygen -H`, entries added and found again by `ssh-keygen -F`) |
| `KeyFiles` | files from `ssh-keygen` (plain, and with four ciphers and two round counts) are opened, files written here are read by `ssh-keygen -y` / `-p`, damaged and foreign files are refused |
| `OpenSshAgainstTestServer` | the real OpenSSH client against the test server: exec, status, streams, 2 MB with re-keys, a server-started re-key, password, keyboard-interactive and banner, `-L` |
| `ClientExec`, `ClientBehaviour` | the built `ssh`: config files and precedence, config errors, wrong key and the method list, a banner with escape codes, Terrapin-shaped and bad-signature servers, unsupported algorithms, disconnects, a silent server, refused connections, a changed or revoked host key (all four policies), hashed known_hosts, a frozen server (`ServerAlive`), exit by signal, 1 MB each way through `cat` with re-keys |
| `ClientInteractive` | on a pseudo-terminal: raw mode and its restoration, window changes, `~` escapes, the host-key question (wrong answer, no, the fingerprint), password prompts without echo, a passphrase asked for only for a key the server takes, key files others can read |
| `ClientTools` | `ssh-keygen` output equals OpenSSH's, a key made here logs in through OpenSSH, passphrase changes, `-F -R -H`; `ssh-keyscan` output and hashed output |
| `ClientScpSftp`, `OpenSshSftpAgainstTestServer` | `scp` and `sftp` on files, trees, wildcards, `-p`, errors, batches; the test server's SFTP checked with OpenSSH's `scp` and `sftp` |
| `ClientForwarding` | `-L` with 3 MB both ways and several connections, refused and prohibited targets, `ExitOnForwardFailure`, SOCKS 4 / 4a / 5, `-R` |

| Guest test (`tests/case_sshclient.py`) | What it shows |
|---|---|
| `sshclient_login_and_exec` | `ssh-keygen` in the guest makes a key, `ssh` logs in to the guest's own sshd; output, status, stderr, a refused key, all host-key policies, a changed key refused under four policies |
| `sshclient_pty_shell` | the console session: prompt, environment, Ctrl+C reaching the far program, `exit N`, `~?`, `~.`, the console cooked again |
| `sshclient_passwords_and_the_python_server` | passwords typed on the console (a wrong one first, no echo), keyboard-interactive with an echoed prompt, `NumberOfPasswordPrompts`, BatchMode, banner and `-q` |
| `sshclient_rekeys_and_bulk_data_against_the_python_server` | 100000 lines up with the client re-keying every 128 KB, a server-started re-key, 600 KB down, exit status and signal |
| `sshclient_scp_and_sftp` | against the guest's sftp-server: 1.3 MB each way intact, trees, a wildcard, an unreadable file, a missing one, a directory without `-r`, an sftp batch and its early stop |
| `sshclient_keygen_keyscan_and_passphrases` | the guest's fingerprint and picture equal the host OpenSSH's; passphrases typed on the console; the key `ssh-keyscan` fetches is the one sshd logged and works as a known_hosts line |
| `sshclient_manual_pages` | all seven pages exist and `patchouli --check-all` passes |

Measured: stripped sizes `ssh` 206 KB, `scp` 206 KB, `sftp` 223 KB, `ssh-keygen` 144 KB, `ssh-keyscan` 165 KB. `scp` of 1.3 MB through the guest's own sshd and
sftp-server over the loopback ran at 0.4 to 1.6 MB/s (guest clock; the PC was shared by a dozen agents). A passphrase check (`bcrypt_pbkdf`, 16 rounds, plain C
without any speed tricks) was not timed separately; it fits inside the guest tests' timeouts.

## Not verified

- Against a real OpenSSH `sshd`, Dropbear, or Go's SSH server: there was none on the development PC (WSL has the OpenSSH client only). What stands in for it is the
  Python server (checked with OpenSSH's client, which gives the server's reading of the protocol an independent referee) and the OS's own sshd.
- On the Dell, and over a real network (DNS names, Tailscale, packet loss).
- `-L`, `-D`, `-R` against the OS's sshd with the forwarding of `agent/ssh-forward` (it was not in main when the client was written); only against the Python server.
- The existing `curve25519.c` shifts negative numbers left (`c << 16` in `car25519`, TweetNaCl's idiom; the compilers treat it as two's complement). UBSan's
  `shift-base` check is therefore off in the host builds. Replacing it by `c * 65536` would remove the exception.

## State when paused / how to resume

The branch `agent/ssh-client` has everything described above committed, with `main` (the first wave, including the server's port forwarding) merged in.
Green at the pause: the 54 host tests and the 7 guest tests (`sshclient_*`), the OS build, `patchouli --check-all`.

Done and tested: the transport and key exchange; login by key, keyboard-interactive and password; host-key checking with all policies; raw-terminal sessions with
escapes, window changes and exit status; `ssh-keygen`; `ssh-keyscan`; `scp`; `sftp`; `-L -R -D`; `ssh_config`; the manual pages; `tests/sshsrv.py`.

Not started, in the order the owner will probably want them:

1. **Try it on the real world**: the dev PC's OpenSSH server, the Forgejo server (`ssh -T git@server...`). If the handshake says `no matching ... found. Their offer: ...`,
   the missing algorithm is the next piece of work (below). `ssh -v` shows every step.
2. **Wider server compatibility**, each as new files in `user/lib/sshcrypto/src/` and an extra entry in the KEXINIT of `sshc_conn.c`: `rsa-sha2-256/512` host keys (a
   small big-number modular exponentiation to verify; no signing needed), `ecdsa-sha2-nistp256` host keys, `aes128-ctr`/`aes256-ctr` with `hmac-sha2-256` (AES exists in `sshc_aes.c`,
   HMAC-SHA256 is a few lines), `diffie-hellman-group14-sha256` (2048-bit modular exponentiation). The Python server's `kex_algs`, `hostkey_algs` and `ciphers` knobs already offer them
   so a test can ask for them, but it would need those algorithms implemented on its side too.
3. **Forwarding against the OS's sshd** (`sshd.forward=local`, `sshd.remoteforward=local`): a guest test like `ssh -L 2222:127.0.0.1:22 marisa@127.0.0.1` then `ssh -p 2222 ...`
   through the tunnel; and `ssh -W host:port` and `-J` (ProxyJump is a `direct-tcpip` channel carrying a second SSH connection; `sshc_login()` takes a socket-like
   fd, so a jump is a small change in `sshc_tcp_connect()`'s caller).
4. `SSH_ASKPASS` (scripts), `ssh -f`, the `~^Z` suspend escape, connection multiplexing.
5. Small things seen but not done: `sftp ls` prints names in plain columns (OpenSSH aligns them differently), `scp` shows its progress meter only on a terminal, the window size
   is polled once a second in addition to SIGWINCH, `ssh-keygen` has no `-t rsa` (it says so).

To pick up: `git switch agent/ssh-client`, `python tools/build.py`, run the two commands under *Tests*. The code is commented for the *why*; start with `sshc.h` (the whole API in
one header), `sshc_conn.c` (transport), `sshc_auth.c` and `user/bin/ssh/session.c`.
