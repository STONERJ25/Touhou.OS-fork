/*
 * The manifest of `update pull`, format 1 (docs/UPDATE.md, "The manifest, format 1"): the strict parser, apart from the rest of updpull.c so that
 * the host test tests/host/test_updpull_parser.py can compile exactly this code (with ASan and UBSan) and compare it, manifest by manifest, with the
 * reference parser of tools/serve-update.py. Nothing here needs more than <stdint.h>, <stdio.h> and <string.h>.
 */
#ifndef UPDMANIFEST_H
#define UPDMANIFEST_H
#include <stddef.h>
#include <stdint.h>

#define MAX_MANIFEST  4096
#define MAX_MFILES    16

typedef struct {
    char path[129];
    uint64_t size;
    uint8_t sha[32];
} MEntry;

typedef struct {
    char version[65];
    long long issued;
    char key_id[17];
    MEntry f[MAX_MFILES];
    int n;
    size_t body_len;
    uint8_t sig[64];
} Manifest;

int upd_lower_hex(const char *s, size_t n, uint8_t *out);      /* exactly n lowercase hex digits; out gets n/2 bytes (out may be NULL) */
int upd_number_ok(const char *s, size_t n, size_t maxdig);     /* "0" or [1-9][0-9]{0,maxdig-1} */
int upd_path_ok(const char *s, size_t n);
int upd_parse_manifest(const uint8_t *d, size_t n, Manifest *m, char *why, size_t wn);   /* 0, or -1 with why */

#endif
