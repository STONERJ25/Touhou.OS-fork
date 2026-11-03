/*
 * BitLocker, read only (see kernel/include/bitlocker.h for what this is for and what it refuses).
 *
 * Sources: the libbde "BitLocker Drive Encryption (BDE) format" document for the metadata, FIPS-197 for AES, NIST SP 800-38C for CCM, IEEE 1619 / NIST SP
 * 800-38E for XTS, Ferguson's 2006 paper "AES-CBC + Elephant diffuser" for the diffuser. The structures as implemented are listed in docs/NTFS.md, which also
 * says which of them were checked only against tests/host/ntfsimg.py (written by the same hand) and which against independent code (the `cryptography`
 * package, the paper's own definitions).
 *
 * The volume, in short (Windows 7 and later):
 *   - the first sector of the volume is a BitLocker header: '-FVE-FS-' at byte 3, a GUID at 160 and the byte offsets of three copies of the FVE metadata at 176,
 *     184 and 192. The original first sectors of the volume (16 of them, usually) were moved, encrypted, to an offset the metadata names ("volume header offset"),
 *     and everything else is encrypted where it is;
 *   - the metadata has a 64-byte block header, a 48-byte metadata header and a list of entries (size, entry type, value type, version, value). A VMK entry (volume
 *     master key) exists per protector. The clear-key one holds the clear key itself and the VMK encrypted with it (AES-CCM); the FVEK entry holds the full volume
 *     encryption key, encrypted with the VMK;
 *   - sectors are encrypted one by one with the FVEK: AES-XTS (Windows 10/11 default, tweak = sector number), AES-CBC with the IV derived from the sector's byte
 *     offset, or AES-CBC with the Elephant diffuser (Vista/7 default).
 */
#include "ntfs_priv.h"
#include "bitlocker.h"
#ifndef NTFS_HOST
#include "ntfs.h"
#endif

/* ===================================================================================================== AES (FIPS-197) */

typedef struct { u32 rk[60]; int rounds; } AesKey;

static u8 g_sbox[256], g_isbox[256];
static u32 g_te0[256], g_td0[256];
static bool g_aes_ready;

static inline u32 rol32(u32 x, unsigned n) { return n ? (x << n) | (x >> (32 - n)) : x; }
static inline u32 ror32(u32 x, unsigned n) { return n ? (x >> n) | (x << (32 - n)) : x; }
static inline u8 xtime(u8 a) { return (u8)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

static u8 gmul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/* The S-box is the multiplicative inverse in GF(2^8) followed by an affine map; it is computed here rather than typed in (and tested against FIPS-197). */
static void aes_tables(void) {
    if (g_aes_ready) return;
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));              /* p := p * 3 */
        q ^= (u8)(q << 1);                                              /* q := q / 3 */
        q ^= (u8)(q << 2);
        q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;
        u8 x = (u8)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        g_sbox[p] = (u8)(x ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_isbox[g_sbox[i]] = (u8)i;
    for (int i = 0; i < 256; i++) {
        u8 s = g_sbox[i], is = g_isbox[i];
        g_te0[i] = (u32)gmul(s, 2) | (u32)s << 8 | (u32)s << 16 | (u32)gmul(s, 3) << 24;
        g_td0[i] = (u32)gmul(is, 14) | (u32)gmul(is, 9) << 8 | (u32)gmul(is, 13) << 16 | (u32)gmul(is, 11) << 24;
    }
    g_aes_ready = true;
}

static inline u32 sub_word(u32 w) {
    return (u32)g_sbox[w & 255] | (u32)g_sbox[(w >> 8) & 255] << 8 | (u32)g_sbox[(w >> 16) & 255] << 16 | (u32)g_sbox[w >> 24] << 24;
}

/* Key expansion for the encryption direction, then the same keys prepared for the "equivalent inverse cipher" (InvMixColumns applied to the middle round keys). */
static void aes_set_key(AesKey *k, AesKey *dk, const u8 *key, unsigned bytes) {
    unsigned nk = bytes / 4, nr = nk + 6, total = 4 * (nr + 1);
    u32 w[60];
    for (unsigned i = 0; i < nk; i++) w[i] = nt_le32(key + 4 * i);
    u8 rcon = 1;
    for (unsigned i = nk; i < total; i++) {
        u32 t = w[i - 1];
        if (i % nk == 0) {
            t = sub_word(ror32(t, 8)) ^ rcon;
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    if (k) {
        memcpy(k->rk, w, total * 4);
        k->rounds = (int)nr;
    }
    if (dk) {
        dk->rounds = (int)nr;
        for (unsigned r = 0; r <= nr; r++) {
            for (unsigned c = 0; c < 4; c++) {
                u32 x = w[4 * (nr - r) + c];
                if (r != 0 && r != nr)
                    x = g_td0[g_sbox[x & 255]] ^ rol32(g_td0[g_sbox[(x >> 8) & 255]], 8) ^ rol32(g_td0[g_sbox[(x >> 16) & 255]], 16) ^ rol32(g_td0[g_sbox[x >> 24]], 24);
                dk->rk[4 * r + c] = x;
            }
        }
    }
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static void aes_encrypt(const AesKey *k, const u8 in[16], u8 out[16]) {
    u32 s[4], t[4];
    for (int c = 0; c < 4; c++) s[c] = nt_le32(in + 4 * c) ^ k->rk[c];
    for (int r = 1; r < k->rounds; r++) {
        for (int c = 0; c < 4; c++)
            t[c] = g_te0[s[c] & 255] ^ rol32(g_te0[(s[(c + 1) & 3] >> 8) & 255], 8) ^ rol32(g_te0[(s[(c + 2) & 3] >> 16) & 255], 16) ^
                   rol32(g_te0[s[(c + 3) & 3] >> 24], 24) ^ k->rk[4 * r + c];
        memcpy(s, t, sizeof s);
    }
    for (int c = 0; c < 4; c++)
        put32(out + 4 * c, ((u32)g_sbox[s[c] & 255] | (u32)g_sbox[(s[(c + 1) & 3] >> 8) & 255] << 8 | (u32)g_sbox[(s[(c + 2) & 3] >> 16) & 255] << 16 |
                            (u32)g_sbox[s[(c + 3) & 3] >> 24] << 24) ^ k->rk[4 * k->rounds + c]);
}

static void aes_decrypt(const AesKey *dk, const u8 in[16], u8 out[16]) {
    u32 s[4], t[4];
    for (int c = 0; c < 4; c++) s[c] = nt_le32(in + 4 * c) ^ dk->rk[c];
    for (int r = 1; r < dk->rounds; r++) {
        for (int c = 0; c < 4; c++)
            t[c] = g_td0[s[c] & 255] ^ rol32(g_td0[(s[(c + 3) & 3] >> 8) & 255], 8) ^ rol32(g_td0[(s[(c + 2) & 3] >> 16) & 255], 16) ^
                   rol32(g_td0[s[(c + 1) & 3] >> 24], 24) ^ dk->rk[4 * r + c];
        memcpy(s, t, sizeof s);
    }
    for (int c = 0; c < 4; c++)
        put32(out + 4 * c, ((u32)g_isbox[s[c] & 255] | (u32)g_isbox[(s[(c + 3) & 3] >> 8) & 255] << 8 | (u32)g_isbox[(s[(c + 2) & 3] >> 16) & 255] << 16 |
                            (u32)g_isbox[s[(c + 1) & 3] >> 24] << 24) ^ dk->rk[4 * dk->rounds + c]);
}

/* ===================================================================================================== AES-CCM (NIST SP 800-38C) */

/* BitLocker wraps keys with a 12-byte nonce, a 16-byte tag and no associated data (so L = 3). Opens `n` bytes of ciphertext in place and checks the tag. */
static int ccm_open(const u8 *key, unsigned keylen, const u8 nonce[12], u8 *data, size_t n, const u8 tag[16]) {
    AesKey k;
    aes_set_key(&k, NULL, key, keylen);
    u8 a[16], s[16], x[16], b[16];
    a[0] = 2;                                                           /* L - 1 */
    memcpy(a + 1, nonce, 12);
    a[13] = a[14] = a[15] = 0;
    aes_encrypt(&k, a, s);                                              /* S0 encrypts the tag */
    u8 want[16];
    for (int i = 0; i < 16; i++) want[i] = tag[i] ^ s[i];
    for (size_t off = 0, ctr = 1; off < n; off += 16, ctr++) {          /* CTR decryption with counters 1, 2, ... */
        a[13] = (u8)(ctr >> 16);
        a[14] = (u8)(ctr >> 8);
        a[15] = (u8)ctr;
        aes_encrypt(&k, a, s);
        size_t m = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < m; i++) data[off + i] ^= s[i];
    }
    b[0] = (u8)(((16 - 2) / 2) << 3 | 2);                               /* flags: no associated data, M = 16, L = 3 */
    memcpy(b + 1, nonce, 12);
    b[13] = (u8)(n >> 16);
    b[14] = (u8)(n >> 8);
    b[15] = (u8)n;
    aes_encrypt(&k, b, x);
    for (size_t off = 0; off < n; off += 16) {                          /* CBC-MAC over the plaintext */
        size_t m = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < m; i++) x[i] ^= data[off + i];
        aes_encrypt(&k, x, x);
    }
    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= (u8)(x[i] ^ want[i]);
    return diff ? -EACCES : 0;
}

/* ===================================================================================================== sector ciphers */

typedef struct BdeKeys {
    AesKey e1, d1, e2;          /* data key (both directions) and the second key (tweak or sector key), encryption only */
    int method;
} BdeKeys;

static void xts_mul_alpha(u8 t[16]) {
    u8 carry = t[15] >> 7;
    for (int i = 15; i > 0; i--) t[i] = (u8)((t[i] << 1) | (t[i - 1] >> 7));
    t[0] = (u8)((t[0] << 1) ^ (carry ? 0x87 : 0));
}

/* IEEE 1619: the tweak is the sector number as a 16-byte little-endian number, encrypted with the second key; every 16-byte block is xored with it before and
 * after the data key, and the tweak is multiplied by x in GF(2^128) between blocks. Sector sizes are multiples of 16, so no ciphertext stealing. */
static void xts_decrypt(const BdeKeys *k, u64 sector_no, u8 *data, size_t len) {
    u8 t[16], blk[16];
    memset(t, 0, 16);
    for (int i = 0; i < 8; i++) t[i] = (u8)(sector_no >> (8 * i));
    aes_encrypt(&k->e2, t, t);
    for (size_t off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) blk[i] = data[off + i] ^ t[i];
        aes_decrypt(&k->d1, blk, blk);
        for (int i = 0; i < 16; i++) data[off + i] = blk[i] ^ t[i];
        xts_mul_alpha(t);
    }
}

static void cbc_decrypt(const AesKey *dk, const u8 iv[16], u8 *data, size_t len) {
    u8 prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        memcpy(cur, data + off, 16);
        aes_decrypt(dk, cur, data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

static void sector_tweak(u64 byte_off, u8 e[16]) {
    memset(e, 0, 16);
    for (int i = 0; i < 8; i++) e[i] = (u8)(byte_off >> (8 * i));
}

/* Ferguson 2006, section 4.4: both diffusers work on the sector as little-endian 32-bit words d[0..n-1], indices modulo n. In the decryption direction
 *   B: for i = 0 .. 3n-1:  d[i] += d[i+2] ^ rotl(d[i+5], Rb[i mod 4]),  Rb = 0, 10, 0, 25
 *   A: for i = 0 .. 5n-1:  d[i] += d[i-2] ^ rotl(d[i-5], Ra[i mod 4]),  Ra = 9, 0, 13, 0
 * and a sector is decrypted as AES-CBC, then B, then A, then xor with the sector key. */
static void elephant_decrypt(const BdeKeys *k, u64 byte_off, u8 *data, size_t len, u32 *stackw) {
    u8 e[16], iv[16];
    sector_tweak(byte_off, e);
    aes_encrypt(&k->e1, e, iv);                                         /* the CBC IV: the data key encrypts the tweak */
    cbc_decrypt(&k->d1, iv, data, len);
    u32 n = (u32)(len / 4);
    for (u32 i = 0; i < n; i++) stackw[i] = nt_le32(data + 4 * i);
    static const u8 Ra[4] = { 9, 0, 13, 0 }, Rb[4] = { 0, 10, 0, 25 };
    for (u32 i = 0; i < 3 * n; i++) {
        u32 x = i % n;
        stackw[x] += stackw[(x + 2) % n] ^ rol32(stackw[(x + 5) % n], Rb[i & 3]);
    }
    for (u32 i = 0; i < 5 * n; i++) {
        u32 x = i % n;
        stackw[x] += stackw[(x + n - 2) % n] ^ rol32(stackw[(x + n - 5) % n], Ra[i & 3]);
    }
    u8 sk[32], e2[16];
    aes_encrypt(&k->e2, e, sk);                                         /* the sector key: the second key encrypts the tweak, then the tweak with its last byte 0x80 */
    memcpy(e2, e, 16);
    e2[15] = 0x80;
    aes_encrypt(&k->e2, e2, sk + 16);
    for (u32 i = 0; i < n; i++) {
        u8 w[4];
        put32(w, stackw[i]);
        for (int j = 0; j < 4; j++) data[4 * i + j] = (u8)(w[j] ^ sk[(4 * i + (u32)j) & 31]);
    }
}

/* ===================================================================================================== the volume */

enum { M_CBC128_EL = 0x8000, M_CBC256_EL = 0x8001, M_CBC128 = 0x8002, M_CBC256 = 0x8003, M_XTS128 = 0x8004, M_XTS256 = 0x8005 };

struct BdeVol {
    BdeIo io;
    u32 sector;
    u64 size;
    u64 enc_size;               /* bytes from the start of the volume that are encrypted */
    u64 hdr_off;                /* where the first hdr_sectors sectors of the volume are kept */
    u32 hdr_sectors;
    BdeKeys keys;
    bool phys_tweak;            /* the relocated sectors are tweaked by where they are stored, not by where they belong (only if that is what unlocked it) */
};

static const char *method_name(unsigned m) {
    switch (m) {
    case M_CBC128_EL: return "AES-CBC 128 with Elephant diffuser";
    case M_CBC256_EL: return "AES-CBC 256 with Elephant diffuser";
    case M_CBC128: return "AES-CBC 128";
    case M_CBC256: return "AES-CBC 256";
    case M_XTS128: return "AES-XTS 128";
    case M_XTS256: return "AES-XTS 256";
    default: return "unknown method";
    }
}

static const char *prot_name(unsigned p) {
    switch (p) {
    case 0x0000: return "clear key";
    case 0x0100: return "TPM";
    case 0x0200: return "startup key";
    case 0x0500: return "TPM + PIN";
    case 0x0600: return "TPM + startup key";
    case 0x0800: return "recovery password";
    case 0x2000: return "password";
    default: return "unknown protector";
    }
}

bool bde_is_bitlocker(const u8 *s0) { return !memcmp(s0 + 3, "-FVE-FS-", 8); }
u64 bde_size(const BdeVol *b) { return b->size; }
u32 bde_sector(const BdeVol *b) { return b->sector; }

/* `scratch` must hold ss/4 words when the method is one of the Elephant ones (the caller allocates it once per request: a 4096-byte sector would be 4 KiB of stack). */
static void decrypt_sector(const BdeKeys *k, u64 pos, u64 tweak_pos, u32 ss, u8 *data, u32 *scratch) {
    switch (k->method) {
    case M_XTS128: case M_XTS256: xts_decrypt(k, tweak_pos / ss, data, ss); break;
    case M_CBC128: case M_CBC256: {
        u8 e[16], iv[16];
        sector_tweak(tweak_pos, e);
        aes_encrypt(&k->e1, e, iv);
        cbc_decrypt(&k->d1, iv, data, ss);
        break;
    }
    default: elephant_decrypt(k, tweak_pos, data, ss, scratch); break;
    }
    (void)pos;
}

/* Prepares the ciphers for a method from the key material of the FVEK entry. `variant` selects between layouts that real volumes might use (see bde_open). */
static bool keys_setup(BdeKeys *k, unsigned method, const u8 *key, u32 klen, int variant) {
    memset(k, 0, sizeof *k);
    k->method = (int)method;
    unsigned half;
    switch (method) {
    case M_XTS128: half = 16; break;
    case M_XTS256: half = 32; break;
    case M_CBC128: case M_CBC128_EL: half = 16; break;
    case M_CBC256: case M_CBC256_EL: half = 32; break;
    default: return false;
    }
    if (method == M_XTS128 || method == M_XTS256) {
        u32 second = variant == 0 ? half : 2 * half;                    /* variant 1: the key blob padded to 512 bits, second key in the second 256-bit half */
        if (klen < second + half) return false;
        aes_set_key(&k->e1, &k->d1, key, half);
        aes_set_key(&k->e2, NULL, key + second, half);
        return true;
    }
    if (method == M_CBC128 || method == M_CBC256) {
        if (klen < half) return false;
        aes_set_key(&k->e1, &k->d1, key, half);
        return true;
    }
    if (klen < 32 + half) return false;                                 /* the Elephant methods use a 512-bit blob: the FVEK, then the "tweak key" */
    aes_set_key(&k->e1, &k->d1, key, half);
    aes_set_key(&k->e2, NULL, key + 32, half);
    return true;
}

typedef struct { const u8 *p; u32 len; } Blob;

typedef struct {
    Blob vmk_key[8], vmk_blob[8];                   /* per clear-key VMK: the clear key and the encrypted VMK */
    int n_clear;
    Blob fvek;
    u16 prot[BDE_MAX_PROT];
    int nprot;
    u16 method_hdr;
    u64 enc_size, hdr_off;
    u32 hdr_sectors;
    Blob area;                                      /* metadata area descriptor, if any */
} Meta;

/* Walks entries (size, entry type, value type, version, value) between pos and end. Sizes need not be multiples of 8; the walk advances by exactly `size`. */
static int entry_next(const u8 *buf, u32 *pos, u32 end, u32 *etype, u32 *vtype, Blob *val) {
    if (*pos > end || end - *pos < 8) return 0;
    u32 size = nt_le16(buf + *pos);
    if (size < 8 || size > end - *pos) return -EIO;
    *etype = nt_le16(buf + *pos + 2);
    *vtype = nt_le16(buf + *pos + 4);
    val->p = buf + *pos + 8;
    val->len = size - 8;
    *pos += size;
    return 1;
}

static int meta_parse(const u8 *blk, u32 blen, Meta *m) {
    if (blen < 0x70 || memcmp(blk, "-FVE-FS-", 8)) return -EINVAL;
    u32 version = nt_le16(blk + 10);
    if (version != 2) return -ENOTSUP;                                  /* version 1 is Windows Vista's layout */
    m->enc_size = nt_le64(blk + 16);
    m->hdr_sectors = nt_le32(blk + 28);
    m->hdr_off = nt_le64(blk + 56);
    u32 msize = nt_le32(blk + 0x40), mver = nt_le32(blk + 0x44), hsize = nt_le32(blk + 0x48);
    if (mver != 1 || hsize != 48 || msize < 48 || msize > blen - 0x40) return -EIO;
    m->method_hdr = nt_le16(blk + 0x40 + 36);
    u32 pos = 0x40 + hsize, end = 0x40 + msize, et, vt;
    Blob v;
    int rc;
    m->nprot = m->n_clear = 0;
    m->fvek.p = NULL;
    m->area.p = NULL;
    while ((rc = entry_next(blk, &pos, end, &et, &vt, &v)) > 0) {
        if (et == 2 && vt == 8) {                                       /* a volume master key, with the properties of one protector */
            if (v.len < 28) return -EIO;
            u32 prot = nt_le16(v.p + 26);
            if (m->nprot < BDE_MAX_PROT) m->prot[m->nprot++] = (u16)prot;
            if (prot != 0 || m->n_clear >= 8) continue;
            u32 ip = 28, iet, ivt;
            Blob iv, key = { NULL, 0 }, wrapped = { NULL, 0 };
            int ir;
            while ((ir = entry_next(v.p, &ip, v.len, &iet, &ivt, &iv)) > 0) {
                if (ivt == 1 && iv.len >= 4 + 16) key = (Blob){ iv.p + 4, iv.len - 4 };       /* a key: method(4), then the key itself */
                else if (ivt == 5 && iv.len >= 28 + 12) wrapped = iv;                          /* AES-CCM encrypted key: nonce(12), tag(16), data */
            }
            if (ir < 0) return ir;
            if (key.p && wrapped.p) {
                m->vmk_key[m->n_clear] = key;
                m->vmk_blob[m->n_clear] = wrapped;
                m->n_clear++;
            }
        } else if (et == 3 && vt == 5 && !m->fvek.p) {
            m->fvek = v;
        } else if (et == 0xF && vt == 0xF && v.len >= 16 && !m->area.p) {
            m->area = v;
        }
    }
    return rc;
}

static int open_key_blob(const u8 *key, unsigned keylen, Blob blob, u8 *plain, u32 cap, u32 *klen_out, u32 *method_out, u32 *key_off) {
    if (!(keylen == 16 || keylen == 24 || keylen == 32)) return -EIO;
    if (blob.len < 28 + 12 || blob.len - 28 > cap) return -EIO;
    u32 n = blob.len - 28;
    memcpy(plain, blob.p + 28, n);
    int rc = ccm_open(key, keylen, blob.p, plain, n, blob.p + 12);
    if (rc) return rc;
    u32 size = nt_le32(plain);                                          /* the plaintext is a key structure: size(4) version(2) unknown(2) method(4) key */
    if (size < 12 + 16 || size > n) return -EIO;
    *method_out = nt_le32(plain + 8);
    *klen_out = size - 12;
    *key_off = 12;
    return 0;
}

static void hex16(const u8 *p, char *out) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = H[p[i] >> 4]; out[2 * i + 1] = H[p[i] & 15]; }
    out[32] = 0;
}

/* Reads the logical sector `lsec` through a candidate key setup, into out (sector bytes). */
static int try_read_logical(BdeVol *b, u32 lsec, bool phys_tweak, u8 *out) {
    u64 pos = (u64)lsec * b->sector;
    int rc = b->io.read(b->io.ctx, b->hdr_off + pos, out, b->sector);
    if (rc) return rc;
    u32 *scratch = NULL;
    if (b->keys.method == M_CBC128_EL || b->keys.method == M_CBC256_EL) {
        scratch = NT_ALLOC(b->sector);
        if (!scratch) return -ENOMEM;
    }
    decrypt_sector(&b->keys, pos, phys_tweak ? b->hdr_off + pos : pos, b->sector, out, scratch);
    if (scratch) NT_FREE(scratch);
    return 0;
}

static bool looks_like_ntfs(const u8 *s) { return !memcmp(s + 3, "NTFS    ", 8) && s[510] == 0x55 && s[511] == 0xAA; }

int bde_open(const BdeIo *io, BdeVol **out, BdeReport *rep) {
    memset(rep, 0, sizeof *rep);
    aes_tables();
    u32 ss = io->sector < 512 ? 512 : io->sector;
    int rc = -ENOMEM;
    u8 *s0 = NT_ALLOC(ss);
    u8 *blk = NT_ALLOC(64 * 1024);
    u8 *plain = NT_ALLOC(512);
    BdeVol *b = NT_ZALLOC(sizeof *b);
    if (!s0 || !blk || !plain || !b) goto done;
    if (io->size < 4 * 64 * 1024 || io->read(io->ctx, 0, s0, 512)) { rc = -EIO; snprintf(rep->text, sizeof rep->text, "bitlocker: cannot read the volume header"); goto done; }
    if (!bde_is_bitlocker(s0) || s0[510] != 0x55 || s0[511] != 0xAA) { rc = -EINVAL; snprintf(rep->text, sizeof rep->text, "bitlocker: no -FVE-FS- volume header"); goto done; }
    b->io = *io;
    b->sector = nt_le16(s0 + 11);
    if (b->sector < 512 || b->sector > 4096 || (b->sector & (b->sector - 1)) || b->sector < io->sector) b->sector = ss;
    b->size = io->size - io->size % b->sector;
    Meta m;
    int which = -1;
    u64 offs[3];
    for (int i = 0; i < 3; i++) offs[i] = nt_le64(s0 + 0xB0 + 8 * i);
    for (int i = 0; i < 3 && which < 0; i++) {
        if (!offs[i] || offs[i] % 512 || offs[i] >= io->size - 4096) continue;
        u64 want = io->size - offs[i] < 65536 ? io->size - offs[i] : 65536;
        memset(blk, 0, 65536);
        if (io->read(io->ctx, offs[i], blk, (size_t)want)) continue;
        if (meta_parse(blk, 65536, &m) == 0) which = i;
    }
    if (which < 0) { rc = -EIO; snprintf(rep->text, sizeof rep->text, "bitlocker: none of the three FVE metadata copies (at %#lx, %#lx, %#lx) is readable", (unsigned long)offs[0], (unsigned long)offs[1], (unsigned long)offs[2]); goto done; }
    rep->nprot = m.nprot;
    memcpy(rep->prot, m.prot, sizeof m.prot);
    rep->method = m.method_hdr;
    char plist[200] = "";
    for (int i = 0; i < m.nprot; i++) {
        size_t o = strlen(plist);
        snprintf(plist + o, sizeof plist - o, "%s%s", i ? ", " : "", prot_name(m.prot[i]));
    }
    if (m.n_clear == 0 || !m.fvek.p) {
        rc = -EACCES;
        snprintf(rep->text, sizeof rep->text, "bitlocker: protectors: %s; %s, so the volume stays locked (a TPM, PIN, password or recovery key is out of scope)",
                 plist[0] ? plist : "none", m.n_clear == 0 ? "there is no clear-key protector (BitLocker is not suspended)" : "the volume key entry is missing");
        goto done;
    }
    /* 1. the clear key opens the VMK; 2. the VMK opens the FVEK */
    u32 klen, method, koff;
    u8 vmk[64], fvek[128];
    bool got = false;
    for (int i = 0; i < m.n_clear && !got; i++) {
        if (open_key_blob(m.vmk_key[i].p, m.vmk_key[i].len >= 32 ? 32 : m.vmk_key[i].len >= 24 ? 24 : 16, m.vmk_blob[i], plain, 512, &klen, &method, &koff)) continue;
        if (klen > sizeof vmk) continue;
        memcpy(vmk, plain + koff, klen);
        u32 vlen = klen;
        if (open_key_blob(vmk, vlen >= 32 ? 32 : vlen >= 24 ? 24 : 16, m.fvek, plain, 512, &klen, &method, &koff) == 0 && klen <= sizeof fvek) {
            memcpy(fvek, plain + koff, klen);
            got = true;
        }
    }
    if (!got) {
        rc = -EACCES;
        snprintf(rep->text, sizeof rep->text, "bitlocker: protectors: %s; the clear key did not open the volume key (the metadata copy %d is damaged or not what was expected)", plist, which + 1);
        goto done;
    }
    rep->method = (u16)method;
    /* where the first sectors live, and how much of the volume is encrypted */
    b->hdr_sectors = m.hdr_sectors;
    b->hdr_off = m.hdr_off;
    if ((!b->hdr_off || !b->hdr_sectors) && m.area.p && m.area.len >= 16) {
        b->hdr_off = nt_le64(m.area.p);
        b->hdr_sectors = (u32)(nt_le64(m.area.p + 8) / b->sector);
    }
    if (!b->hdr_off || !b->hdr_sectors || b->hdr_sectors > 4096 || b->hdr_off % b->sector || b->hdr_off > b->size - (u64)b->hdr_sectors * b->sector) {
        rc = -EIO;
        snprintf(rep->text, sizeof rep->text, "bitlocker: protectors: %s; the relocated volume header (%u sectors at %#lx) is not inside the volume", plist, b->hdr_sectors, (unsigned long)b->hdr_off);
        goto done;
    }
    b->enc_size = m.enc_size ? m.enc_size : b->size;
    /* 3. prove the key: sector 0 of the decrypted volume must be an NTFS boot sector. Real volumes might lay the key blob out in a way the format description does
     *    not spell out, so the plausible layouts are tried; the first that gives NTFS wins and is said so in the log. */
    u8 *chk = NT_ALLOC(b->sector);
    if (!chk) { rc = -ENOMEM; goto done; }
    bool ok = false;
    char tried[96] = "";
    for (int variant = 0; variant < 2 && !ok; variant++) {
        if (!keys_setup(&b->keys, method, fvek, klen, variant)) continue;
        for (int phys = 0; phys < 2 && !ok; phys++) {
            if (try_read_logical(b, 0, phys != 0, chk) == 0 && looks_like_ntfs(chk)) {
                ok = true;
                b->phys_tweak = phys != 0;
                if (variant || phys) snprintf(tried, sizeof tried, " (key layout %d, %s tweak)", variant, phys ? "physical" : "logical");
            }
        }
    }
    if (!ok) {
        char hx[40];
        keys_setup(&b->keys, method, fvek, klen, 0);
        hx[0] = 0;
        if (try_read_logical(b, 0, false, chk) == 0) hex16(chk + 3, hx);
        rc = -EACCES;
        snprintf(rep->text, sizeof rep->text, "bitlocker: protectors: %s; the clear key opened the volume key (%s), but sector 0 of the decrypted volume is not an NTFS boot sector "
                 "(bytes 3..18: %s): this is not an NTFS volume, or the key or the tweak is not used the way this driver assumes", plist, method_name(method), hx);
        NT_FREE(chk);
        goto done;
    }
    NT_FREE(chk);
    snprintf(rep->text, sizeof rep->text, "bitlocker: unlocked with the clear key: %s (method %#x), metadata copy %d of 3, %u volume header sectors relocated to %#lx, "
             "encrypted size %lu MiB, protectors: %s%s", method_name(method), method, which + 1, b->hdr_sectors, (unsigned long)b->hdr_off, (unsigned long)(b->enc_size >> 20), plist, tried);
    rep->unlocked = true;
    *out = b;
    b = NULL;
    rc = 0;
done:
    if (b) NT_FREE(b);
    if (s0) NT_FREE(s0);
    if (blk) NT_FREE(blk);
    if (plain) NT_FREE(plain);
    return rc;
}

void bde_close(BdeVol *b) {
    if (b) {
        memset(&b->keys, 0, sizeof b->keys);                            /* the volume key does not outlive the mount */
        NT_FREE(b);
    }
}

int bde_read(BdeVol *b, u64 off, void *buf, size_t len) {
    u32 ss = b->sector;
    u8 *out = buf;
    if (off % ss || len % ss || off > b->size || len > b->size - off) return -EINVAL;
    u64 hdr_bytes = (u64)b->hdr_sectors * ss;
    u32 *scratch = NULL;
    if (b->keys.method == M_CBC128_EL || b->keys.method == M_CBC256_EL) {
        scratch = NT_ALLOC(ss);
        if (!scratch) return -ENOMEM;
    }
    int result = 0;
    while (len) {
        size_t n;
        int rc;
        if (off < hdr_bytes) {                                          /* the relocated first sectors: stored encrypted elsewhere, tweaked as if still at the start */
            n = (size_t)(hdr_bytes - off < len ? hdr_bytes - off : len);
            rc = b->io.read(b->io.ctx, b->hdr_off + off, out, n);
            if (rc) { result = rc; break; }
            for (size_t i = 0; i < n; i += ss) decrypt_sector(&b->keys, off + i, b->phys_tweak ? b->hdr_off + off + i : off + i, ss, out + i, scratch);
        } else {
            n = len > (1u << 20) ? (1u << 20) : len;
            rc = b->io.read(b->io.ctx, off, out, n);
            if (rc) { result = rc; break; }
            for (size_t i = 0; i < n && off + i < b->enc_size; i += ss) decrypt_sector(&b->keys, off + i, off + i, ss, out + i, scratch);
        }
        off += n;
        out += n;
        len -= n;
    }
    if (scratch) NT_FREE(scratch);
    return result;
}

/* A read callback that takes any byte range (the NTFS parser's reads are not sector aligned). */
int bde_dev_read(void *ctx, u64 off, void *buf, size_t len) {
    BdeVol *b = ctx;
    u32 ss = b->sector;
    if (!len) return 0;
    if (off > b->size || len > b->size - off) return -EIO;
    u64 start = off & ~(u64)(ss - 1);
    u64 end = (off + len + ss - 1) & ~(u64)(ss - 1);
    if (end > b->size) end = b->size;
    if (start == off && end == off + len) return bde_read(b, off, buf, len);
    u8 *tmp = NT_ALLOC((size_t)(end - start));
    if (!tmp) return -ENOMEM;
    int rc = bde_read(b, start, tmp, (size_t)(end - start));
    if (!rc) memcpy(buf, tmp + (off - start), len);
    NT_FREE(tmp);
    return rc;
}

/* ===================================================================================================== the decrypted view as a block device */

#ifndef NTFS_HOST

static int view_read(BlockDev *d, u64 lba, u32 count, void *buf) {
    BdeVol *b = d->priv;
    return bde_read(b, lba * d->sector_size, buf, (size_t)count * d->sector_size);
}

int bde_attach(BlockDev *src, BdeVol **bdep, BlockDev **viewp, char *why, size_t whyn) {
    BdeIo io = { ntfs_blk_io, src, src->sectors * src->sector_size, src->sector_size };
    BdeReport rep;
    BdeVol *b;
    int rc = bde_open(&io, &b, &rep);
    klog("Keine", "%s: %s", src->name, rep.text);
    if (rc) {
        if (why && whyn) strlcpy(why, rep.text, whyn);
        return rc;
    }
    BlockDev *v = kzalloc(sizeof *v);
    if (!v) { bde_close(b); return -ENOMEM; }
    int n = 0;
    for (; n < 64; n++) {
        snprintf(v->name, sizeof v->name, "bde%d", n);
        if (!blk_find(v->name)) break;
    }
    v->driver = "bitlocker";
    snprintf(v->model, sizeof v->model, "BitLocker view of %s", src->name);
    v->sector_size = bde_sector(b);
    v->sectors = bde_size(b) / v->sector_size;
    v->readonly = true;                                                 /* for ever: there is no write function to call */
    v->read = view_read;
    v->priv = b;
    v->parent = src;
    v->part_start = 0;                                                  /* the view covers exactly the sectors of its source (the mount overlap check relies on it) */
    if (blk_register(v)) { kfree(v); bde_close(b); return -ENOMEM; }
    *bdep = b;
    *viewp = v;
    return 0;
}

void bde_detach(BdeVol *b, BlockDev *view) {
    if (view) blk_unregister(view);                                     /* later requests on /dev/bdeN fail with ENODEV; the struct stays (blk.h) */
    bde_close(b);
}

#endif

/* ===================================================================================================== hooks for the host tests */

#ifdef NTFS_HOST

/* tests/host/ntfstool.c drives the ciphers directly and compares them with the `cryptography` package: single AES blocks, CCM opening, whole sectors. */
void bde_test_aes(int decrypt, const u8 *key, unsigned keylen, const u8 in[16], u8 out[16]) {
    aes_tables();
    AesKey e, d;
    aes_set_key(&e, &d, key, keylen);
    if (decrypt) aes_decrypt(&d, in, out);
    else aes_encrypt(&e, in, out);
}

int bde_test_ccm(const u8 *key, unsigned keylen, const u8 nonce[12], u8 *data, size_t n, const u8 tag[16]) {
    aes_tables();
    return ccm_open(key, keylen, nonce, data, n, tag);
}

int bde_test_sector(unsigned method, const u8 *key, u32 klen, u64 byte_off, u32 ss, u8 *data, int variant) {
    aes_tables();
    BdeKeys k;
    if (!keys_setup(&k, method, key, klen, variant)) return -EINVAL;
    u32 *scratch = malloc(ss);
    decrypt_sector(&k, byte_off, byte_off, ss, data, scratch);
    free(scratch);
    return 0;
}

#endif
