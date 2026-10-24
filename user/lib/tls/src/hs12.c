/*
 * The TLS 1.2 client handshake (RFC 5246, RFC 5289 GCM suites, RFC 7905 ChaCha20-Poly1305), ECDHE key exchange only.
 * It uses the extended master secret (RFC 7627) when the server offers it. It never resumes a session and never renegotiates,
 * so the triple handshake attack, which needs both, has nothing to work with when a server does not have the extension.
 */
#include "tlsconn.h"

static int ccs_expected(struct tls_conn *c) {
    for (;;) {
        int t;
        u8 *d;
        size_t n;
        int rc = tls_recv_record(c, &t, &d, &n);
        if (rc == 1) return tls_fail(c, TLS_E_IO, -1);
        if (rc < 0) return tls_fail(c, rc, -1);
        if (t == CT_CCS) {
            if (n != 1 || d[0] != 1 || c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            return 0;
        }
        if (t == CT_ALERT) {
            if (n == 2 && d[0] == 1 && d[1] != AL_CLOSE_NOTIFY) continue;        /* a warning */
            if (n == 2) {
                c->alert_rx = d[1];
                c->last_error = TLS_E_ALERT;
                c->state = 4;
                return TLS_E_ALERT;
            }
        }
        return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    }
}

static int scheme_matches_auth(unsigned scheme, int auth) {
    switch (scheme) {
    case 0x0403: case 0x0503: case 0x0603: return auth == AUTH_ECDSA;
    case 0x0401: case 0x0501: case 0x0601: case 0x0804: case 0x0805: case 0x0806: return auth == AUTH_RSA;
    default: return 0;
    }
}

int tls12_client(struct tls_conn *c) {
    const suite_info *s = c->suite;
    const int alg = s->hash;
    const size_t hl = tlsc_hash_len(alg);
    int rc;
    u8 type;
    const u8 *msg;
    size_t len;
    c->info.version = 0x0303;
    c->info.version_name = "TLS 1.2";
    c->info.suite_name = s->name;

    /* Certificate */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_CERTIFICATE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    {
        rd_t r = { msg + 4, len - 4, 0 };
        unsigned ll = rd24(&r);
        const u8 *list = rdn(&r, ll);
        if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        if (ll == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR);
        if ((rc = tls_verify_peer(c, list, ll, 0))) return rc;
    }
    const x509_cert *leaf = &c->chain[0];
    if ((s->auth == AUTH_ECDSA && leaf->key_type != X_KEY_P256 && leaf->key_type != X_KEY_P384) || (s->auth == AUTH_RSA && leaf->key_type != X_KEY_RSA))
        return tls_fail(c, TLS_E_CERT_USAGE, AL_ILLEGAL_PARAMETER);
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);

    /* ServerKeyExchange: ECDHE parameters signed with the certificate's key */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_SERVER_KEY_EXCHANGE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    u8 peer_pub[97];
    size_t peer_len;
    unsigned group;
    {
        rd_t r = { msg + 4, len - 4, 0 };
        unsigned curve_type = rd8(&r);
        group = rd16(&r);
        unsigned pl = rd8(&r);
        const u8 *pub = rdn(&r, pl);
        size_t params_len = 4 + pl;
        unsigned scheme = rd16(&r);
        unsigned sl = rd16(&r);
        const u8 *sig = rdn(&r, sl);
        if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        if (curve_type != 3) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        size_t want = group == GROUP_X25519 ? 32 : group == GROUP_SECP256R1 ? 65 : group == GROUP_SECP384R1 ? 97 : 0;
        if (!want || pl != want) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        if (!scheme_matches_auth(scheme, s->auth)) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        memcpy(peer_pub, pub, pl);
        peer_len = pl;
        u8 signed_data[64 + 4 + 97];
        memcpy(signed_data, c->client_random, 32);
        memcpy(signed_data + 32, c->server_random, 32);
        memcpy(signed_data + 64, msg + 4, params_len);
        if ((rc = tls_check_signature(c, scheme, 0, signed_data, 64 + params_len, sig, sl))) return rc;
    }
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);

    /* [CertificateRequest] ServerHelloDone */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type == HS_CERTIFICATE_REQUEST) {
        c->cert_requested = 1;
        tls_transcript_add(c, msg, len);
        tls_hs_consume(c, len);
        if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    }
    if (type != HS_SERVER_HELLO_DONE || len != 4) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);
    if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);

    /* the shared secret, and our public value for ClientKeyExchange */
    u8 pms[66], our_pub[97];
    size_t pms_len, our_len;
    if (group == GROUP_X25519) {
        if (tlsc_x25519(pms, c->x25519_priv, peer_pub) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        pms_len = 32;
        memcpy(our_pub, c->x25519_pub, 32);
        our_len = 32;
        c->info.group_name = "x25519";
    } else {
        int idx = group == GROUP_SECP256R1 ? 0 : 1;
        int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
        if ((rc = tls_ec_keygen(c, group))) return rc;
        if (tlsc_ecdh(curve, c->ec_priv[idx], peer_pub, peer_len, pms) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        pms_len = tlsc_ec_size(curve);
        our_len = idx == 0 ? 65 : 97;
        memcpy(our_pub, c->ec_pub[idx], our_len);
        c->info.group_name = idx == 0 ? "secp256r1" : "secp384r1";
    }

    if (c->cert_requested) {                                         /* no client certificate: say so */
        static const u8 empty_cert[7] = { HS_CERTIFICATE, 0, 0, 3, 0, 0, 0 };
        if ((rc = tls_send_handshake(c, empty_cert, sizeof empty_cert))) return rc;
    }
    {
        u8 cke[4 + 1 + 97];
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        cke[1] = 0;
        cke[2] = 0;
        cke[3] = (u8)(1 + our_len);
        cke[4] = (u8)our_len;
        memcpy(cke + 5, our_pub, our_len);
        if ((rc = tls_send_handshake(c, cke, 5 + our_len))) return rc;
    }

    /* extended master secret: the PRF over the hash of the handshake so far (RFC 7627); older servers get the classic derivation */
    u8 th[64];
    tls_transcript_hash(c, alg, th);
    if (c->ems) tlsc_prf(alg, pms, pms_len, "extended master secret", th, hl, NULL, 0, c->master, 48);
    else tlsc_prf(alg, pms, pms_len, "master secret", c->client_random, 32, c->server_random, 32, c->master, 48);
    tlsc_wipe(pms, sizeof pms);

    size_t kl = (size_t)tls_cipher_keylen(s->cipher);
    size_t il = s->cipher == CIPHER_CHACHA ? 12 : 4;
    u8 kb[2 * 32 + 2 * 12];
    tlsc_prf(alg, c->master, 48, "key expansion", c->server_random, 32, c->client_random, 32, kb, 2 * kl + 2 * il);
    const u8 *ckey = kb, *skey = kb + kl, *civ = kb + 2 * kl, *siv = kb + 2 * kl + il;

    /* our ChangeCipherSpec and Finished */
    static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    if ((rc = tls_send_raw(c, ccs, sizeof ccs))) return rc;
    tls_cipher_set(&c->wr, s->cipher, ckey, civ, il);
    {
        u8 fin[4 + 12];
        fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
        tlsc_prf(alg, c->master, 48, "client finished", th, hl, NULL, 0, fin + 4, 12);
        if ((rc = tls_send_handshake(c, fin, sizeof fin))) return rc;
    }

    /* the server's ChangeCipherSpec and Finished */
    if ((rc = ccs_expected(c))) return rc;
    tls_cipher_set(&c->rd, s->cipher, skey, siv, il);
    tlsc_wipe(kb, sizeof kb);
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_FINISHED || len != 4 + 12) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    {
        u8 expect[12];
        tls_transcript_hash(c, alg, th);                             /* everything up to and including our Finished */
        tlsc_prf(alg, c->master, 48, "server finished", th, hl, NULL, 0, expect, 12);
        if (!tlsc_ct_memeq(expect, msg + 4, 12)) return tls_fail(c, TLS_E_FINISHED, AL_DECRYPT_ERROR);
    }
    tls_hs_consume(c, len);
    if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    return 0;
}
