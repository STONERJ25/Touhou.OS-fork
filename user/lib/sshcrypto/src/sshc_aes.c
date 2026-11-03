/*
 * AES (FIPS 197) in counter mode, for one job: reading and writing private keys that ssh-keygen protected with a passphrase (cipher aes256-ctr is its default;
 * aes192-ctr and aes128-ctr are accepted as well). Only encryption of a block is needed, since counter mode turns it into a stream cipher in both directions.
 *
 * The S-box is computed at first use from its definition (the multiplicative inverse in GF(2^8) followed by an affine map) rather than typed in, so there is
 * no table to mistype; the test checks the whole thing against an independent AES. This is straightforward byte-wise code, not a fast or a constant-time one: it
 * touches one short key file, once, on the owner's own machine.
 */
#include <sshc.h>
#include <string.h>

static uint8_t g_sbox[256];
static bool g_sbox_ready;

static uint8_t rotl8(uint8_t x, int k) { return (uint8_t)((x << k) | (x >> (8 - k))); }

static void make_sbox(void) {
    uint8_t p = 1, q = 1;
    do {                                                    /* p runs through every non-zero element as powers of 3, q through their inverses */
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        g_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    g_sbox[0] = 0x63;
    g_sbox_ready = true;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

void sshc_aes_init(SshcAes *a, const uint8_t *key, int keylen) {
    if (!g_sbox_ready) make_sbox();
    int nk = keylen / 4;                                    /* 4, 6 or 8 words */
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);                        /* words of round key */
    uint8_t *w = a->rk;
    memcpy(w, key, (size_t)keylen);
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint8_t t[4];
        memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {                                  /* RotWord, SubWord, Rcon */
            uint8_t u = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ rcon);
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[u];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int k = 0; k < 4; k++) t[k] = g_sbox[t[k]];
        }
        for (int k = 0; k < 4; k++) w[4 * i + k] = (uint8_t)(w[4 * (i - nk) + k] ^ t[k]);
    }
}

void sshc_aes_encrypt_block(const SshcAes *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ a->rk[i]);
    for (int round = 1; round <= a->rounds; round++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = g_sbox[s[i]];                       /* SubBytes */
        for (int c = 0; c < 4; c++)                                              /* ShiftRows: row r rotates left by r (state is column-major) */
            for (int r = 0; r < 4; r++) s[4 * c + r] = t[4 * ((c + r) & 3) + r];
        if (round != a->rounds) {                                                /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                col[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                col[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                col[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * round + i];            /* AddRoundKey */
    }
    memcpy(out, s, 16);
}

void sshc_aes_ctr(const SshcAes *a, uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n) {
    uint8_t ks[16];
    for (size_t off = 0; off < n; off += 16) {
        sshc_aes_encrypt_block(a, iv, ks);
        size_t take = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        for (int i = 15; i >= 0; i--)                                            /* the counter is the whole block, big-endian */
            if (++iv[i] != 0) break;
    }
    memset(ks, 0, sizeof ks);
}
