/*
 * Host harness for the manifest parser of `update pull` (user/bin/kappabox/updmanifest.c), driven by tests/host/test_updpull_parser.py.
 *
 * Standard input: records of a 4-byte little-endian length and that many bytes (one candidate manifest each). Standard output, one line per record:
 *   R                                                                  refused
 *   A VERSION ISSUED KEYID BODYLEN SIGHEX N {PATH SIZE SHAHEX}...       accepted, with everything the parser read
 * Each candidate is copied into a heap block of exactly its length, so that AddressSanitizer sees any read past its end.
 */
#include "updmanifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hex(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

int main(void) {
    for (;;) {
        uint8_t l[4];
        if (fread(l, 1, 4, stdin) != 4) break;
        size_t n = (size_t)l[0] | (size_t)l[1] << 8 | (size_t)l[2] << 16 | (size_t)l[3] << 24;
        uint8_t *d = malloc(n ? n : 1);
        if (!d || fread(d, 1, n, stdin) != n) return 3;
        if (!n) { free(d); d = malloc(1); free(d); d = NULL; }       /* (an empty candidate: no byte may be read at all) */
        Manifest m;
        char why[600];
        if (upd_parse_manifest(d, n, &m, why, sizeof why)) {
            if (!why[0] || strlen(why) >= sizeof why) return 4;      /* a refusal always says why */
            puts("R");
        } else {
            printf("A %s %lld %s %zu ", m.version, m.issued, m.key_id, m.body_len);
            hex(m.sig, 64);
            printf(" %d", m.n);
            for (int i = 0; i < m.n; i++) {
                printf(" %s %llu ", m.f[i].path, (unsigned long long)m.f[i].size);
                hex(m.f[i].sha, 32);
            }
            putchar('\n');
        }
        free(d);
    }
    return 0;
}
