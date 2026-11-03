# TLS and HTTPS: libtls, libhttp and `https-get`

## Decision on the parked libtls (2026-11-02): adapt, with a new protocol engine

An earlier agent left a `libtls` of 20 files (about 4,700 lines) on `verify/staging` and `github/parked/network-https` (the two
copies are byte-identical). It was read file by file, built, its own tests were run (all eight `tls_host_*` tests of its
`tests/case_tls.py` pass on this PC: known answers against Python's `cryptography`, handshakes against OpenSSL in every suite,
certificates, names, injected faults, the root bundle), and its DER and X.509 code was probed with 300,000 mutated certificates
under AddressSanitizer and UBSan in WSL (no finding). **It is adapted, not rewritten**: the parts that are hard to get right and
were found right are kept, the part that does not fit the brief is replaced.

| Criterion | What the parked code has |
|-----------|--------------------------|
| Protocol coverage | TLS 1.3 and 1.2 client with the six required suites, x25519 / P-256 / P-384, HelloRetryRequest, KeyUpdate, ALPN, SNI, extended master secret, the TLS 1.3 downgrade sentinel. **Missing:** a non-blocking mode (the handshake is straight-line code over a blocking read loop), Ed25519 and RSASSA-PSS signatures on certificates, name constraints, any notion of an untrusted clock, a timeout for a silent server. |
| Correctness | Interoperates with OpenSSL 3 in every suite. **Defects found in review:** handshake bytes that follow the ServerHello in the same plaintext record would be carried across the key change (RFC 8446 section 5.1 requires a record boundary there, also before KeyUpdate); a HelloRetryRequest that carries only a cookie is refused; chain building with backtracking has no budget (a server can send ten look-alike certificates and make the client try 9! orders); duplicate extensions in certificates and hellos are accepted; validity is checked on the trust anchor too, which is stricter than RFC 5280 but rejects nothing real. |
| Constant time | The X25519 ladder swaps with masks; P-256/P-384 use the complete addition formulas of Renes-Costello-Batina and a 4-bit window read with a masked scan of the whole table; the Montgomery arithmetic has fixed loop counts and masked final subtractions; GHASH is either PCLMULQDQ or a bit-serial loop with masks; ChaCha20 and Poly1305 have no tables. The one table-driven part is the portable AES (S-box lookups), used only without AES-NI; ChaCha20 is then offered first. |
| Memory safety | Every parser reads through bounds-checked cursors; DER is strict (minimal lengths, no indefinite form, one-byte tags); the fuzz probe found nothing. |
| Tests | Good differential tests, but compiled as a Windows DLL with a bump allocator that never frees (so no use-after-free or leak can show), no AddressSanitizer, no RFC 8448 vectors, no DER torture. They are replaced by WSL host tests with ASan/UBSan (`tests/host/test_tls_vectors.py`). |
| Size | Compact: about 4,700 lines for the primitives, certificates and both protocol versions. |

**Kept** (reviewed, then changed only where noted in the git history): the primitives `sha2.c chacha.c aes.c bn.c ecc.c x25519.c
rsa.c`, the generated constants `tables.h` (the host tests recompute every value), and the certificate code `asn1.c x509.c pem.c`,
extended with Ed25519, RSASSA-PSS, name constraints, a path-building budget and duplicate-extension checks. **Rewritten:** the
protocol engine (record layer, both handshakes, the public API) as a message-driven state machine, so that one code path serves
blocking and non-blocking sockets, plus the clock and the trust store loading. Ed25519 comes from `user/lib/sshcrypto`.

The rest of this file describes the library as it is now. It is a careful hobby implementation, tested hard, **not a hardened
production TLS stack**: read "Security notes" before trusting it with anything that matters.

## What there is

```
 https_get URL ...               user/bin/kappabox/https.c        the command (verifies, -v shows the chain)
   |  http.h: http_get, HttpParser, HttpUrl, the cookie jar
 libhttp                         user/lib/http/src/{url,parse,cookie,client}.c
   |  tls.h: tls_connect / tls_read / tls_write / tls_close, TlsTrust, the clock
 libtls                          user/lib/tls/src/
   api.c       the API, the handshake driver, peer verification, error sentences
   record.c    records (AEAD, sequence numbers, padding, size limits), alerts, ChangeCipherSpec rules, the transcript
   hello.c     ClientHello (built, then read back as "what was offered"), ServerHello, HelloRetryRequest
   hs13.c      TLS 1.3 after the ServerHello: key schedule, EncryptedExtensions .. Finished, KeyUpdate, tickets
   hs12.c      TLS 1.2 ECDHE: Certificate, ServerKeyExchange, ServerHelloDone, our flight, ChangeCipherSpec, Finished
   x509.c asn1.c pem.c clock.c     certificates, DER, the trust store, the clock
   sha2.c chacha.c aes.c bn.c ecc.c x25519.c rsa.c tables.h     the primitives (Ed25519: user/lib/sshcrypto)
   os_io.c     random numbers, time, sockets with MSG_DONTWAIT, poll, files (the same file builds for the Linux host tests)
```

Both libraries are static libraries under `user/lib`, linked into every program that uses them (`tools/build.py` finds them).

## Using libtls

```c
#include <tls.h>

TlsConfig cfg = { 0 };                          /* verify against $SSL_CERT_FILE or /etc/ssl/cacert.pem, 30 s timeout */
cfg.alpn = "http/1.1";
TlsConn *c = tls_connect(fd, "example.org", &cfg);      /* fd: a connected TCP socket; the handshake runs here */
if (tls_status(c) < 0) {
    fprintf(stderr, "%s\n", tls_error_string(c));       /* a sentence: which certificate, which names, which dates, which clock */
    tls_close(c);
    return;
}
tls_write(c, request, len);                             /* blocking: everything is written before it returns */
long n;
while ((n = tls_read(c, buf, sizeof buf)) > 0) use(buf, n);
/* n == 0: close_notify; TLS_E_TRUNCATED: the TCP connection ended without it (see "Truncation") */
tls_close(c);                                           /* sends close_notify, frees; the socket is yours to close */
```

* `tls_connect` returns a connection even when the handshake failed (NULL only without memory), so that the error can be
  explained; `tls_status()` is 0 once connected, `TLS_WANT_*` while a non-blocking handshake runs, else the `TLS_E_*` error.
* **Host names:** a DNS name (lower-cased, a trailing dot removed, letters, digits, `-`, `_`, `.`; internationalized names in their
  `xn--` form) goes into SNI and is matched against the certificate's subjectAltName DNS entries; a dotted IPv4 address is matched
  against its IP entries and no SNI is sent. The Common Name is never used.
* **Flags** (`TlsConfig.flags`): `TLS_NONBLOCK`, `TLS_INSECURE`, `TLS_ONLY_TLS12`, `TLS_ONLY_TLS13`, `TLS_REQUIRE_CLOCK`,
  `TLS_NO_AESNI` (tests). `TlsConfig.suites` restricts the bulk ciphers (`TLS_SUITE_AES128_GCM`, `_AES256_GCM`, `_CHACHA20`).
* **What happened:** `tls_info(c)` gives the version, suite, group, the server's signature scheme, ALPN, whether a
  HelloRetryRequest happened, `verified` / `insecure` / `verify_error`, the clock state and time used, and up to ten
  `TlsCertInfo` entries (subject, issuer, names, dates, key, signature algorithm, SHA-256 fingerprint, `from_store`): the verified
  path from the leaf to the root, or the certificates as sent when nothing was verified. The alert sent or received is there too.

### Non-blocking (for poll loops: the browser, a VNC server)

```c
cfg.flags |= TLS_NONBLOCK;
TlsConn *c = tls_connect(fd, host, &cfg);               /* only starts the handshake: sends the ClientHello if it can */
for (;;) {
    int rc = tls_handshake(c);                          /* also called implicitly by tls_read / tls_write */
    if (rc == 0) break;
    if (rc != TLS_WANT_READ && rc != TLS_WANT_WRITE) { error(tls_error_string(c)); break; }
    struct pollfd p = { fd, tls_poll_events(c), 0 };    /* POLLIN, POLLOUT or both */
    poll(&p, 1, timeout);
}
```

The engine never blocks: it reads with `MSG_DONTWAIT` (the socket need not be `O_NONBLOCK`) and keeps partial records and
partial handshake messages between calls. Rules for the event loop:

* `tls_read` returns `TLS_WANT_READ` when no whole record is there yet; **before polling for input, drain `tls_pending(c)`**
  (decrypted bytes already held).
* `tls_write` returns how many bytes it accepted; they may still be queued. `tls_poll_events` includes `POLLOUT` while
  anything is queued, and `tls_flush(c)` returns 0 once it is all on the socket.
* A read can produce output (the answer to a server's KeyUpdate request): that is why `tls_read` may return `TLS_WANT_WRITE`.
* Timeouts are the caller's business in this mode (`TlsConfig.timeout_ms` is used only while a blocking call waits).

### Trust store

The library ships **no** root certificates. `TlsConfig.ca_file` names a PEM bundle (or a single DER certificate) for one
connection; `TlsConfig.trust` takes a `TlsTrust` the program loaded itself with `tls_trust_load_file/_pem/add_der` (load it once
and share it between connections: parsing Mozilla's 150 roots costs time); otherwise `tls_trust_default()` loads
`$SSL_CERT_FILE` or **`/etc/ssl/cacert.pem`** once per process. Without any store the connection fails with `TLS_E_NO_TRUST`
and a sentence that says where to put one, unless `TLS_INSECURE` is given. Duplicates are dropped, roots with keys the library
cannot use (P-521, DSA) are skipped and counted (`tls_trust_skipped`). A server certificate that is itself in the store is
trusted as it stands (`https_get --ca server.pem` for a self-signed test server).

The owner puts a bundle there, for example Mozilla's (the `certifi` package's `cacert.pem`, or `curl.se/ca/cacert.pem`).
**`/etc` is rebuilt from the initrd at every boot**: a file put into `/etc/ssl/` by hand lasts until the next restart. So `tools/build.py` puts
`assets/local/ssl/cacert.pem` (git-ignored; on the owner's PC a copy of the Mozilla bundle that Git for Windows ships in `mingw64/etc/ssl/certs/ca-bundle.crt`, 150 roots, 229 KB)
into the image as `/etc/ssl/cacert.pem` (2026-11-03); without that file the machine has no trust store until one is copied in.

### The clock

Certificate dates need the time, and a PC's clock can be wrong (the Dell's said 2025 for a while). `tls_clock()` reports, and
`TlsInfo.clock` records for every connection, one of three states:

| State | Meaning | Dates are checked against |
|-------|---------|---------------------------|
| `TLS_CLOCK_SET` | a program said what time it is: `tls_set_time(t)` (pinned), or a source given to `tls_set_time_source()` that calls its answer trusted (after NTP, say) | that time |
| `TLS_CLOCK_SYSTEM` | the system clock, and it is not earlier than the day this library was compiled: plausible, not confirmed | the system clock |
| `TLS_CLOCK_BEHIND` | the system clock is earlier than the build day, so it is certainly wrong | the build day, because the real time is at least that |

With `TLS_CLOCK_BEHIND` a certificate that expired before the build is still refused and one issued after it is accepted, but
one that expired between the build and today cannot be caught: the state is visible (`https_get -v` prints it) so nobody has to
guess. `TLS_REQUIRE_CLOCK` (`https_get --require-clock`) refuses to connect unless the clock is `TLS_CLOCK_SET`. Error
sentences about dates always say what the clock said and which kind it was ("... expired on 2026-01-01 00:00:00 UTC; the clock
says 2026-11-02 12:00:00 UTC (system clock)"). Setting the system clock with `sntp -s` makes `TLS_CLOCK_SYSTEM` right.

**Threads.** A `TlsConn` belongs to one thread at a time. The clock settings and the default store are process-wide and not
locked: set the clock before connections start, and call `tls_trust_default()` once before starting threads that connect.

## Using libhttp

```c
#include <http.h>

static int save(void *ctx, const uint8_t *d, size_t n) { return fwrite(d, 1, n, ctx) == n ? 0 : 1; }

HttpOptions o = { .on_body = save, .ctx = file, .jar = http_jar_new() };   /* .tls = &cfg for TLS settings */
HttpResult r;
if (http_get("https://example.org/", &o, &r) == 0) printf("%d %s, %llu bytes\n", r.status, r.reason, r.body_bytes);
else fprintf(stderr, "%s\n", r.error_text);
```

* `http_get` resolves the name (A records), connects with a timeout, makes TLS (ALPN `http/1.1`, verification as above),
  sends `GET` (or `HEAD`) with `Accept-Encoding: gzip, deflate` and `Connection: close`, follows up to ten redirects (301, 302,
  303, 307, 308; relative `Location`s resolved per RFC 3986; https to http is followed and reported as a WARNING event), keeps
  cookies when given a jar, inflates gzip/deflate bodies, and streams the final body to `on_body`. Callbacks for -v style
  reporting: `on_status`, `on_header`, `on_event`, `on_tls` (each connection's `TlsInfo`, also for a refused certificate).
* **The parser is usable alone** (the browser's case): `http_parser_init(&p, &callbacks, HTTP_DECODE)`, then
  `http_parser_feed(&p, bytes, n, &used)` with whatever the socket gave (any split), `http_parser_eof(&p)` when the connection
  ends. It never touches a socket, so it sits on a non-blocking `TlsConn` as easily as on a plain one. Framing follows RFC 9112
  6.3: no body for HEAD, 1xx, 204, 304; chunked (extensions ignored, trailers read and dropped); Content-Length (a repeated
  identical value is accepted, differing ones refused); otherwise until the end of the connection. Interim responses (100, 103)
  are skipped. An obsolete folded header line is joined to the one before. Limits: lines and fields of 8 KiB (raised from 4 KiB for fields on 2026-11-03: Wikipedia sends a 4.5 KiB Content-Security-Policy), 64 KiB and
  256 fields of header; every number is checked for overflow.
* `http_url_parse` / `http_url_resolve` / `http_url_format`, `http_request_build`: the pieces `http_get` is made of.
* The cookie jar (`http_jar_*`) is RFC 6265 without the public suffix list (a Domain attribute without a dot is refused, so
  `com` is stopped but `co.uk` is not), in memory, at most 300 cookies.

### Truncation

Over TLS, a body framed by `Content-Length` or chunked encoding is complete when the framing says so, and a server that then
closes without `close_notify` (many do) costs nothing. A body that only the end of the connection delimits is complete only if
the server sent `close_notify`; otherwise `http_get` fails with `HTTP_E_TRUNCATED` (the bytes went to `on_body` already), because
whoever can cut the TCP connection could have cut the body. Over plain HTTP there is no way to tell, and the end is accepted.

## https_get

```
https_get URL [-o FILE] [--ca FILE] [--insecure] [-v] [-I] [-q] [--max-redirs N] [--timeout S]
              [--tls12|--tls13] [--cipher aes128|aes256|chacha20] [--time EPOCH] [--require-clock] [--no-decompress]
```

The body goes to standard output, or with `-o` to `FILE.part`, renamed to `FILE` only when everything arrived (a failed download
leaves no file that looks complete). `-v` writes the connection, the TLS parameters, the clock, every certificate of the
verified path (subject, issuer, names, dates, key, signature, fingerprint; the root marked as coming from the store) and the
response headers to standard error. `--insecure` (also `-k`) skips verification and says so on every use, with what
verification would have found. `--time` pins the clock (trusted) for this run. `-I` sends HEAD and prints the header.

Exit status: 0 done, 1 usage, 2 name or network failure, 3 no safe TLS connection (certificate refused, no trust store, failed
handshake), 4 the server answered 400 or above (the body is still written), 5 a bad or cut response, 6 the output could not be
written.

**Its name.** Kappabox applets are C identifiers (`tools/build.py` collects `APPLET(\w+)` and `main.c` compares `argv[0]` exactly),
so the command is `https_get`. The brief's `https-get` needs two one-line changes outside this work (see "Open ends").

## What is implemented

| Area | Done |
|------|------|
| TLS 1.3 (RFC 8446) | full client handshake; x25519 in the first ClientHello, P-256 and P-384 after a HelloRetryRequest (a cookie is echoed; a cookie-only retry works); `TLS_AES_128_GCM_SHA256`, `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256`; server signatures ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384, ed25519, rsa_pss_rsae_sha256/384/512, rsa_pss_pss_sha256/384/512; middlebox compatibility mode (session id, dummy ChangeCipherSpec, only when a session id was sent); KeyUpdate both ways (answering a request, and our own every 2^24 records or on `tls_key_update`); NewSessionTicket read and ignored; ALPN; SNI; a CertificateRequest is answered with an empty Certificate; padding, empty application records (up to 32 in a row), record boundaries enforced at every key change |
| TLS 1.2 (RFC 5246, 5289, 7627, 7905, 8422) | ECDHE-ECDSA and ECDHE-RSA with AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305; x25519, P-256, P-384; Ed25519 certificates under ECDHE_ECDSA; extended master secret when offered (the classic master secret otherwise, see Security notes); the TLS 1.3 downgrade sentinel; renegotiation_info sent empty and checked, a HelloRequest answered with no_renegotiation; a server that tries to resume a session is refused |
| Validation of the server's answers | every extension in ServerHello, HelloRetryRequest and EncryptedExtensions must have been offered (checked against the ClientHello as it went out) and appear once; versions, suites, groups, session id echo, ALPN; malformed or oversized messages and records refused with the matching alert |
| Certificates (RFC 5280, 6125, 4055, 8410) | strict DER; keys RSA 2048-4096 (and RSASSA-PSS keys), P-256, P-384, Ed25519; signatures sha256/384/512 with RSA PKCS#1 v1.5, RSASSA-PSS (MGF1 with the same hash), ECDSA, Ed25519; SHA-1, MD5, MD2, SHA-224 and keys below 2048 bits refused as weak; validity of every certificate; basicConstraints (CA flag, pathLen not counting self-issued certificates), keyUsage (keyCertSign for CAs, digitalSignature for the leaf), extendedKeyUsage (serverAuth or any, also on intermediates); subjectAltName DNS names with wildcards (a whole left-most label, at least two labels after it) and IPv4 addresses; **name constraints** (dNSName with wildcard-aware rules, iPAddress, directoryName) of every CA above the leaf, the root's included; duplicate extensions and unknown critical extensions refused; chain building that tries each candidate issuer (store first, then what the server sent, any order, extras ignored, cross-signed and re-issued intermediates) with every check made while searching, so that a valid alternative is found, within a budget of 64 signature checks |
| Primitives | SHA-256/384/512, HMAC, HKDF, the TLS 1.2 PRF, ChaCha20-Poly1305, AES-128/256 (AES-NI or portable) with GCM (PCLMULQDQ or bit-serial), Montgomery arithmetic up to 4096 bits, X25519, P-256/P-384 ECDH and ECDSA verification, RSA PKCS#1 v1.5 and PSS verification; Ed25519 from `user/lib/sshcrypto` |
| HTTP (libhttp) | see "Using libhttp" |

## What is not implemented

Client certificates; session resumption, tickets, PSK, 0-RTT; renegotiation; **revocation of any kind** (no CRL, no OCSP, no
stapled responses: a revoked certificate is accepted until it expires); Certificate Transparency; policy processing (policies are
read past and never required); fetching a missing intermediate from the Authority Information Access URL (a server that forgets
its intermediate is refused where a browser would manage); P-521, Ed448, DSA, finite-field Diffie-Hellman, RSA key exchange,
CBC suites, anything with SHA-1, RC4, 3DES or export strength; compression; post-quantum hybrid groups (servers fall back to
x25519); Encrypted Client Hello; the record_size_limit extension (records are at most 16 KiB anyway); IPv6 anywhere; a TLS server
(the record layer, the key schedule and the certificate code would serve one; the handshake state machine is client-only);
HTTP/2, keep-alive and pipelining (one request per connection), proxies, `Range`, compression other than gzip/deflate (`br` and
`zstd` are never asked for and passed through untouched if sent anyway), POST and friends in `http_get` (the parser does not care).

## Security notes

* **Not hardened.** Written for a hobby OS from the specifications, reviewed by its authors and tested as described below, never
  audited, never fuzzed with coverage guidance. Treat a connection made by it as better than plain HTTP, not as a bank-grade one.
* **Side channels.** What handles secrets is written to have no secret-dependent branches or table lookups: the X25519 ladder
  (masked swaps), the P-256/P-384 scalar multiplication for ECDH (fixed 4-bit windows, every table entry read and masked,
  complete addition formulas), the Montgomery field arithmetic (fixed loop counts, masked final subtraction), ChaCha20 and
  Poly1305 (no tables), GHASH (PCLMULQDQ or a bit-serial loop with masks), tag comparisons (`tlsc_ct_memeq`). It is C compiled
  with -O2 and nobody has checked the machine code for branches the compiler may have brought back. **The portable AES is not
  constant time** (S-box lookups indexed by secret data): it is used only on a CPU without AES-NI (the Dell's i5-7500 and QEMU's
  emulated CPU have it), and ChaCha20-Poly1305 is offered first there. Signature verification works on public data and is
  variable-time on purpose (`tlsc_mont_pow` may only see public exponents).
* **Random numbers** come from the kernel's ChaCha20 generator (`getrandom`, kernel/lib/random.c, seeded from RDRAND and timing).
  There is no "not seeded yet" signal on this kernel, so the library cannot refuse an early, weakly seeded generator.
* **Key material** is wiped (`tlsc_wipe`) when a key changes, when the handshake ends (ephemeral keys, handshake and master
  secrets) and when the connection is freed; the application traffic secrets live as long as the connection (KeyUpdate needs them),
  and copies in dead stack frames are not chased.
* **TLS 1.2 without the extended master secret** falls back to the classic master secret. The triple handshake attack needs
  resumption or renegotiation, neither of which this client does.
* **`--insecure` / `TLS_INSECURE`** still requires the server to sign the handshake with the key of the certificate it sent, which
  stops a passive listener and nothing else; anyone in the path can substitute a certificate. Every use prints a warning.
* **The clock and the store** bound what verification means: see "The clock" and "Trust store". A bundle that is never updated
  keeps trusting roots Mozilla has since removed.
* **https to http redirects** are followed (as browsers do) and announced with a WARNING; `HttpResult.downgraded` says so.

## Tests

| Where | How | What |
|-------|-----|------|
| host (WSL, gcc, AddressSanitizer and UBSan) | `python tools/hosttest.py discover -s tests/host -p "test_tls_vectors.py" -v` (about 50 s) | `tests/host/test_tls_vectors.py` driving `tests/host/tlstool.c`: the 61 tests listed below (2 of them need the RFC 8448 text) |
| QEMU | `python tools/qemu-slot.py -- python tests/run_all.py tls_` | `tests/case_tls.py`: the guest against servers on the host |

The host tests, by class:

* `Tables`: every constant of `tables.h` recomputed from its definition (cube and square roots of primes for SHA-2, the AES
  S-box from the field inverse and the affine map, the curve parameters as `openssl ecparam` prints them).
* `Hashes`, `Aead`, `PublicKey`: FIPS 180, FIPS 197, RFC 4231, 5869, 7748 (with the 1000-fold iteration), 8032, 8439 vectors,
  and random inputs against `hashlib`, `hmac` and `cryptography`: SHA-2 at every length to 260 and the padding edges, HMAC with
  every key length class, HKDF and HKDF-Expand-Label, the TLS 1.2 PRF against a reference written in the test, ChaCha20,
  Poly1305 and the AEAD (tampered tag, text and AAD refused), AES and GCM in both implementations, X25519 (low-order points
  refused), P-256/P-384 ECDH (off-curve, out-of-range, compressed, infinity points refused) and ECDSA (both s and n-s, r or s 0
  or n refused), RSA PKCS#1 v1.5 and PSS at 2048/3072/4096 bits with three hashes and three salt lengths, an e = 3 Bleichenbacher
  forgery refused, Ed25519 (the S + L malleability refused), Montgomery multiplication and exponentiation from 64 to 4096 bits.
* `Certificates`: good chains in five algorithm mixes and with RSA-PSS signatures, every refusal (name, dates of leaf and
  intermediate, unknown CA, self-signed, bad signature, SHA-1 and MD5, RSA-1024, not a CA, no basicConstraints, pathLen, no
  keyCertSign, client-only EKU, encipherment-only key usage, unknown critical extension, P-521), alternative paths, the search
  budget against nine interchangeable look-alikes (9! orders without it), name constraints, a 22-case host-name table plus
  addresses, the PEM store, certificates made by the openssl command (RSA, EC, Ed25519), truncation at every byte, 200,000
  mutations, duplicate extensions.
* `Handshakes` (openssl s_server and Python's ssl): every suite of both versions with EC, RSA, Ed25519 and P-384 certificates,
  the portable AES, TLS 1.2 groups, HelloRetryRequest to P-256 and P-384, ALPN, version mismatches, KeyUpdate both ways,
  refusals with the alert that was sent, `TLS_INSECURE`, `TLS_REQUIRE_CLOCK`, a missing store, plain HTTP, a silent server, a
  cut ServerHello, 3 MB each way in TLS 1.3, TLS 1.2 and non-blocking mode, close_notify or not.
* `DamagedHandshakes`: a proxy in front of openssl s_server changes one byte of the server's clear handshake per connection
  (160 TLS 1.2 handshakes: ServerHello, the chain, ServerKeyExchange; 80 TLS 1.3 ServerHellos); each ends in a clean error of
  the matching kind or in a verified success, without a hang or an ASan report.
* `Tls13Faults`: an independent TLS 1.3 server written in the test from RFC 8446 with `cryptography`'s primitives, which
  checks the client's Finished and breaks the protocol one way at a time (handshake bytes across the key change, five kinds of
  HelloRetryRequest, an unsolicited extension, bad CertificateVerify and Finished, PKCS#1 in TLS 1.3, wrong suite, version,
  session id, ALPN, a missing key share; after the handshake: KeyUpdate with a request, tickets, a KeyUpdate sharing its record,
  ChangeCipherSpec, a flood of empty records, an oversized record, a tampered record, a warning alert, a cut connection), plus
  coalesced, fragmented and padded records that must work.
* `Rfc8448`: the client replays the RFC 8448 ClientHellos with their private keys and must produce every later record of the
  1-RTT and HelloRetryRequest traces byte for byte (Finished, application data, close_notify). **These two need the RFC's text at
  `build/rfc8448.txt`** (or `$TOUHOU_RFC8448`) and skip without it; the trace reader is checked on a sample (`Rfc8448Parser`).
  They have not been run against the RFC yet (the agent that wrote them could not download it). `ReplayMachinery` proves the
  rest of the chain without it: a real handshake with the Python server is recorded, written out in the RFC's layout (a page
  break inside a value included), read back, and replayed by a client that gets only the ClientHello and the private key (its own
  random numbers fresh): every record it sends matches the recording byte for byte.
* `HttpUrls`, `HttpParsing`, `Cookies`, `HttpClient`: the RFC 3986 resolution examples, URL refusals, the request text; every
  framing fed in pieces of 1 to 100,000 bytes with identical results, gzip (also multi-member), zlib and raw deflate, corrupt
  and cut compressed bodies, 18 kinds of malformed response, 600 corrupted ones; the cookie rules; `http_get` against local plain
  and TLS servers (5 MB, gzip, chunked, three redirects, a loop, cookies across a redirect, close_notify or not, cut bodies,
  certificate and store failures, an unknown name, a refused port).

The QEMU tests (`tests/case_tls.py`; the guest reaches the host as 10.0.2.2, openssl s_server runs in WSL):

* `tls_guest_handshakes`: TLS 1.3 with each suite (`--cipher`), TLS 1.2 with the six ECDHE suites, RSA (PSS) and Ed25519
  certificates, HelloRetryRequest to P-256, an IP-address URL, the `-v` report.
* `tls_guest_refusals`: exit 3 with the right sentence for a wrong name, expired, not yet valid, unknown CA, self-signed, bad
  signature, plain HTTP, a cut handshake, a stalling server (`--timeout 3`), no trust store; `SSL_CERT_FILE`; `--insecure`;
  a pinned clock past the end; `--require-clock`; no file left behind by a refused `-o` download.
* `tls_guest_downloads`: 20 MiB with the right SHA-256, page, chunked, gzip and redirect, https to http with the warning, HEAD,
  404 with exit status 4.
* `tls_guest_internet` (optional, skipped without internet): example.com and www.wikipedia.org against Mozilla's roots from the
  host's `certifi`, expired.badssl.com refused.

## Measured (2026-11-03, the owner's PC, QEMU with TCG, the guest under the shared limiter)

| What | Result |
|------|--------|
| `tls_guest_handshakes` (boot, 13 handshakes with `-v`) | passed, 40 s |
| `tls_guest_refusals` (boot, 17 refusals and checks) | passed, 35 s |
| `tls_guest_downloads`: 20 MiB over TLS 1.3 (AES-128-GCM) into the guest's `/tmp` | right SHA-256, 3.6 s of wall time, 5.9 MB/s |
| `tls_guest_internet` | example.com (TLS 1.3, Cloudflare, four certificates up to SSL.com TLS ECC Root CA 2022) and www.wikipedia.org (Let's Encrypt YE2, ISRG Root YE cross-signed by ISRG Root X2) verified against Mozilla's roots; expired.badssl.com refused ("expired on 2015-04-12 23:59:59 UTC") |
| host suite | 61 tests, about 50 s with ASan/UBSan (2 skipped: the RFC 8448 text) |
| code size | libtls about 107 KB of code and constants, libhttp 23 KB, the applet 5 KB; the stripped `kappabox` is 894 KB |
| memory | a connection holds 2 x 18 KiB of input and 2 x 16 KiB of output buffer, the handshake buffer while it runs, and the TlsInfo (about 8 KiB) |

## Open ends (changes outside this work's files, and what is worth doing next)

* **`https-get` as a name**: `user/bin/kappabox/main.c` would map `-` to `_` before looking an applet up
  (`for (char *q = buf; *q; q++) if (*q == '-') *q = '_';` on a copy of the name), and `tools/build.py` would add
  `("bin/https-get", "kappabox")` to `EXTRA_LINKS`. Until then the command is `https_get`.
* **A root store that survives a restart**: done differently on 2026-11-03: the bundle is part of the image (see above), not of the persistent /etc (whose files are flat names of at most 64 KiB).
  A bundle copied in by hand still lasts until the next boot, and `SSL_CERT_FILE` can point at one on the volume.
* **A manual page** `https_get(1)` (`initrd/ScarletDevilMansion/share/patchouli/1/`), not written here.
* **The RFC 8448 replays** need the RFC's text at `build/rfc8448.txt`; then `python tools/hosttest.py tests.host.test_tls_vectors.Rfc8448 -v`.
* **On the Dell**: AES-NI, PCLMULQDQ and RDRAND have run in QEMU's emulation and natively in WSL, never in the OS on real silicon;
  `https_get -v https://example.com/` with a bundle in `/etc/ssl/cacert.pem` (and the clock set by `sntp -s`) is the first test.
* **Next**: AIA fetching of missing intermediates; OCSP stapling (asking for it, checking it); a constant-time (bit-sliced) AES
  for CPUs without AES-NI; keep-alive in libhttp for the browser; a TLS server on the same record layer.

## Files

`user/lib/tls/include/tls.h`, `user/lib/tls/src/*` (above), `user/lib/http/include/http.h`, `user/lib/http/src/{url,parse,cookie,client}.c`,
`user/bin/kappabox/https.c`, `tests/host/test_tls_vectors.py`, `tests/host/tlstool.c`, `tests/case_tls.py`, this file.

## Seen on the Dell (2026-11-03, build `b0bb254`, the first start with the trust store in the image)

`https_get -I https://example.com/` answered `HTTP 200 OK` on the first try after a restart (no file copied in by hand: `/etc/ssl/cacert.pem` is the 229 KB bundle of `assets/local/ssl/cacert.pem`, and the clock was set at boot by SNTP, `docs/NTP.md`).
`https_get -v -I https://www.wikipedia.org/`: TLS 1.3, `TLS_AES_128_GCM_SHA256`, x25519, an ECDSA P-256 server signature, the verified path leaf (Let's Encrypt, `*.wikipedia.org`) to root; the log line "clock: system clock (not confirmed)" is the library saying the clock was set by the network, not by `--time`.
`https_get -I https://expired.badssl.com/` was refused with the date of the expiry and the clock it compared with. Not tried on the Dell: TLS 1.2-only servers, a self-signed server with `--ca`, large downloads, HTTP redirects across hosts.
