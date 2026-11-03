"""The SSH crypto library inside the OS: user/bin/sshkat checks SHA-512, X25519, Ed25519, ChaCha20 and Poly1305 against vectors made with an independent
library (tools/gen_sshcrypto_vectors.py, which needs the `cryptography` package only when the set is regenerated). docs/SSH.md."""
import re

import test_shell
from harness import test, Qemu


@test
def sshcrypto_known_answers():
    """sshkat: SHA-512 (padding edges, split updates), X25519 (RFC 7748 and random), Ed25519 (RFC 8032, random, corrupted and malleable signatures are refused),
    ChaCha20 with a 64-bit nonce and Poly1305 (RFC 8439 and edge keys) all give the answers an independent library gave; the kernel's random generator
    passes the statistical sanity checks (bit balance, histogram, no repeated blocks, short reads)."""
    with Qemu(cmdline="text", name="t-sshcrypto") as vm:
        vm.expect(test_shell.PROMPT, timeout=180)
        out, rc = test_shell.run(vm, "sshkat", timeout=900)
        assert rc == 0 and "SSHKAT PASS" in out, out[-1500:]
        m = re.search(r"sshkat: (\d+) checks, 0 failed", out)
        assert m and int(m.group(1)) > 100, out[-500:]
