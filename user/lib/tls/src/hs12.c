/*
 * The TLS 1.2 client handshake, ECDHE only (RFC 5246, RFC 8422 for the curves and EdDSA, RFC 5289 for the GCM suites, RFC 7905
 * for ChaCha20-Poly1305), one message per call. The extended master secret (RFC 7627) is used when the server offers it. No
 * session is ever resumed and a HelloRequest is answered with no_renegotiation, so the triple handshake attack, which needs one
 * of the two, has nothing to work with when a server lacks the extension.
 *
 *   ST12_WAIT_CERT   Certificate (verified here)
 *   ST12_WAIT_SKE    ServerKeyExchange: ECDHE parameters signed with the certificate's key
 *   ST12_WAIT_SHD    [CertificateRequest] ServerHelloDone, then our [Certificate] ClientKeyExchange ChangeCipherSpec Finished
 *   ST12_WAIT_CCS    the server's ChangeCipherSpec (a record, see hs12_ccs)
 *   ST12_WAIT_FIN    Finished
 */
#include "conn.h"

static int scheme_matches_suite(unsigned scheme, int auth) {
    switch (scheme) {
    case 0x0403: case 0x0503: case 0x0807: return auth == AUTH_ECDSA;          /* RFC 8422: ECDHE_ECDSA covers EdDSA too */
    case 0x0401: case 0x0501: case 0x0601: case 0x0804: case 0x0805: case 0x0806: case 0x0809: case 0x080a: case 0x080b: return auth == AUTH_RSA;
    default: return 0;
    }
}

static int key_matches_suite(const x509_cert *leaf, int auth) {
    if (auth == AUTH_ECDSA) return leaf->key_type == X_KEY_P256 || leaf->key_type == X_KEY_P384 || leaf->key_type == X_KEY_ED25519;
    return leaf->key_type == X_KEY_RSA || leaf->key_type == X_KEY_RSA_PSS;
}

static int client_flight(TlsConn *c) {
    const suite_info *s = c->suite;
    const int alg = s->hash;
    const size_t hl = tlsc_hash_len(alg);
    int rc;
    unsigned group = c->sh_group;
    u8 pms[66], our_pub[97];
    size_t pms_len, our_len;
    if (group == GROUP_X25519) {
        if (!c->have_x25519) return tls_fail(c, TLS_E_PROTOCOL, AL_INTERNAL_ERROR, "no x25519 key");
        if (tlsc_x25519(pms, c->x25519_priv, c->sh_share) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server's x25519 value is a point of small order");
        pms_len = 32;
        memcpy(our_pub, c->x25519_pub, 32);
        our_len = 32;
        c->info.group_name = "x25519";
    } else {
        int idx = group == GROUP_SECP256R1 ? 0 : 1;
        int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
        if ((rc = ec_keygen(c, group))) return rc;
        if (tlsc_ecdh(curve, c->ec_priv[idx], c->sh_share, c->sh_share_len, pms) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server's EC value is not a point on the curve");
        pms_len = tlsc_ec_size(curve);
        our_len = idx == 0 ? 65 : 97;
        memcpy(our_pub, c->ec_pub[idx], our_len);
        c->info.group_name = idx == 0 ? "secp256r1" : "secp384r1";
    }
    tlsc_wipe(c->x25519_priv, sizeof c->x25519_priv);
    tlsc_wipe(c->ec_priv, sizeof c->ec_priv);

    if (c->cert_requested) {                                         /* no client certificate: say so */
        static const u8 empty_cert[7] = { HS_CERTIFICATE, 0, 0, 3, 0, 0, 0 };
        if ((rc = hs_send(c, empty_cert, sizeof empty_cert))) return rc;
    }
    u8 cke[4 + 1 + 97];
    cke[0] = HS_CLIENT_KEY_EXCHANGE;
    cke[1] = 0;
    cke[2] = 0;
    cke[3] = (u8)(1 + our_len);
    cke[4] = (u8)our_len;
    memcpy(cke + 5, our_pub, our_len);
    if ((rc = hs_send(c, cke, 5 + our_len))) return rc;

    /* the extended master secret: the PRF over the hash of the handshake so far (RFC 7627); older servers get the classic one */
    u8 th[64];
    transcript_hash(c, alg, th);
    if (c->ems) tlsc_prf(alg, pms, pms_len, "extended master secret", th, hl, NULL, 0, c->master, 48);
    else tlsc_prf(alg, pms, pms_len, "master secret", c->offer.random, 32, c->server_random, 32, c->master, 48);
    tlsc_wipe(pms, sizeof pms);

    size_t kl = (size_t)cipher_keylen(s->cipher);
    size_t il = s->cipher == CIPHER_CHACHA ? 12 : 4;
    u8 kb[2 * 32 + 2 * 12];
    tlsc_prf(alg, c->master, 48, "key expansion", c->server_random, 32, c->offer.random, 32, kb, 2 * kl + 2 * il);
    static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    if ((rc = rec_queue_raw(c, ccs, sizeof ccs))) return tls_fail(c, rc, AL_INTERNAL_ERROR, "could not queue ChangeCipherSpec");
    rec_set_keys(&c->wr, s->cipher, kb, kb + 2 * kl, il);
    memcpy(c->pending_key, kb + kl, kl);                             /* the server's side waits for its ChangeCipherSpec */
    memcpy(c->pending_iv, kb + 2 * kl + il, il);
    tlsc_wipe(kb, sizeof kb);
    u8 fin[4 + 12];
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
    tlsc_prf(alg, c->master, 48, "client finished", th, hl, NULL, 0, fin + 4, 12);   /* th: everything up to the ClientKeyExchange */
    if ((rc = hs_send(c, fin, sizeof fin))) return rc;
    c->state = ST12_WAIT_CCS;
    return 0;
}

int hs12_ccs(TlsConn *c) {
    size_t il = c->suite->cipher == CIPHER_CHACHA ? 12 : 4;
    rec_set_keys(&c->rd, c->suite->cipher, c->pending_key, c->pending_iv, il);
    tlsc_wipe(c->pending_key, sizeof c->pending_key);
    tlsc_wipe(c->pending_iv, sizeof c->pending_iv);
    c->state = ST12_WAIT_FIN;
    return 0;
}

int hs12_message(TlsConn *c, unsigned type, const u8 *msg, size_t len) {
    const suite_info *s = c->suite;
    const u8 *body = msg + 4;
    size_t blen = len - 4;
    int rc;
    switch (c->state) {
    case ST12_WAIT_CERT:
        if (type != HS_CERTIFICATE) break;
        {
            rd_t r = { body, blen, 0 };
            unsigned ll = rd24(&r);
            const u8 *list = rdn(&r, ll);
            if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed Certificate message");
            if (ll == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR, "the server sent no certificate");
            if ((rc = verify_peer(c, list, ll, 0))) return rc;
        }
        if (!key_matches_suite(&c->chain[0], s->auth))
            return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the certificate's key does not fit the cipher suite %s", s->name);
        transcript_add(c, msg, len);
        c->state = ST12_WAIT_SKE;
        return 0;
    case ST12_WAIT_SKE:
        if (type != HS_SERVER_KEY_EXCHANGE) break;
        {
            rd_t r = { body, blen, 0 };
            unsigned curve_type = rd8(&r);
            unsigned group = rd16(&r);
            unsigned pl = rd8(&r);
            const u8 *pub = rdn(&r, pl);
            size_t params_len = 4 + pl;
            unsigned scheme = rd16(&r);
            unsigned sl = rd16(&r);
            const u8 *sig = rdn(&r, sl);
            if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed ServerKeyExchange");
            if (curve_type != 3) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "ServerKeyExchange without a named curve");
            size_t want = group == GROUP_X25519 ? 32 : group == GROUP_SECP256R1 ? 65 : group == GROUP_SECP384R1 ? 97 : 0;
            if (!want || !offered(c->offer.groups, c->offer.ngroups, group))
                return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server picked group 0x%04x, which was not offered", group);
            if (pl != want) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "an ECDHE value of the wrong size");
            if (!scheme_matches_suite(scheme, s->auth)) return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "signature scheme 0x%04x does not fit %s", scheme, s->name);
            memcpy(c->sh_share, pub, pl);
            c->sh_share_len = pl;
            c->sh_group = group;
            u8 signed_data[64 + 4 + 97];                             /* client_random, server_random, the parameters */
            memcpy(signed_data, c->offer.random, 32);
            memcpy(signed_data + 32, c->server_random, 32);
            memcpy(signed_data + 64, body, params_len);
            if ((rc = check_handshake_signature(c, scheme, 0, signed_data, 64 + params_len, sig, sl))) return rc;
        }
        transcript_add(c, msg, len);
        c->state = ST12_WAIT_SHD;
        return 0;
    case ST12_WAIT_SHD:
        if (type == HS_CERTIFICATE_REQUEST && !c->cert_requested) {
            c->cert_requested = 1;
            transcript_add(c, msg, len);
            return 0;
        }
        if (type != HS_SERVER_HELLO_DONE) break;
        if (blen != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a ServerHelloDone with content");
        transcript_add(c, msg, len);
        hs_consume(c, len);
        if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake data after ServerHelloDone");
        return client_flight(c) ? c->err : 1;
    case ST12_WAIT_FIN:
        if (type != HS_FINISHED) break;
        if (blen != 12) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a Finished message of the wrong size");
        {
            u8 th[64], expect[12];
            transcript_hash(c, s->hash, th);                         /* everything up to and including our Finished */
            tlsc_prf(s->hash, c->master, 48, "server finished", th, tlsc_hash_len(s->hash), NULL, 0, expect, 12);
            if (!tlsc_ct_memeq(expect, body, 12)) return tls_fail(c, TLS_E_FINISHED, AL_DECRYPT_ERROR, "the server's Finished message does not match the handshake");
        }
        hs_consume(c, len);
        if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake data after the server's Finished");
        tlsc_wipe(c->master, sizeof c->master);
        c->state = ST_CONNECTED;
        return 1;
    default:
        return tls_fail(c, TLS_E_PROTOCOL, AL_INTERNAL_ERROR, "TLS 1.2 handshake in state %d", c->state);
    }
    return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "unexpected handshake message %u in the TLS 1.2 handshake (state %d)", type, c->state);
}
