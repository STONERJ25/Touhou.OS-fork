/* The TLS 1.3 client handshake after the ServerHello (RFC 8446), and the messages that may follow it. */
#include "tlsconn.h"

static void traffic_keys(struct tls_conn *c, const u8 *secret, cipher_state *cs) {
    int alg = c->suite->hash;
    u8 key[32], iv[12];
    int kl = tls_cipher_keylen(c->suite->cipher);
    tlsc_hkdf_expand_label(alg, secret, "key", NULL, 0, key, (size_t)kl);
    tlsc_hkdf_expand_label(alg, secret, "iv", NULL, 0, iv, 12);
    tls_cipher_set(cs, c->suite->cipher, key, iv, 12);
    tlsc_wipe(key, sizeof key);
}

static int parse_encrypted_extensions(struct tls_conn *c, const u8 *p, size_t n) {
    rd_t r = { p, n, 0 };
    unsigned elen = rd16(&r);
    rd_t e = rd_sub(&r, elen);
    if (r.err || e.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
    while (e.n > 0) {
        unsigned type = rd16(&e);
        unsigned dlen = rd16(&e);
        rd_t d = rd_sub(&e, dlen);
        if (e.err || d.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        if (type == 0x0010) {                                        /* ALPN: exactly one protocol, one of ours */
            unsigned l = rd16(&d);
            rd_t list = rd_sub(&d, l);
            unsigned nl = rd8(&list);
            const u8 *name = rdn(&list, nl);
            if (d.err || list.err || list.n != 0 || d.n != 0 || nl == 0 || nl >= sizeof c->info.alpn) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
            if (!c->cfg.alpn || strlen(c->cfg.alpn) != nl || memcmp(name, c->cfg.alpn, nl) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
            memcpy(c->info.alpn, name, nl);
            c->info.alpn[nl] = 0;
        }
    }
    return 0;
}

static int send_ccs(struct tls_conn *c) {
    static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    return tls_send_raw(c, ccs, sizeof ccs);
}

int tls13_client(struct tls_conn *c) {
    const suite_info *s = c->suite;
    const int alg = s->hash;
    const size_t hl = tlsc_hash_len(alg);
    u8 shared[66];
    size_t shl;
    int rc;

    /* the key exchange */
    if (c->sh_group == GROUP_X25519) {
        if (tlsc_x25519(shared, c->x25519_priv, c->sh_share) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        shl = 32;
        c->info.group_name = "x25519";
    } else {
        int idx = c->sh_group == GROUP_SECP256R1 ? 0 : 1;
        int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
        if (!c->ec_have[idx]) return tls_fail(c, TLS_E_PROTOCOL, AL_INTERNAL_ERROR);
        if (tlsc_ecdh(curve, c->ec_priv[idx], c->sh_share, c->sh_share_len, shared) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        shl = tlsc_ec_size(curve);
        c->info.group_name = idx == 0 ? "secp256r1" : "secp384r1";
    }

    /* the key schedule (RFC 8446 7.1) */
    u8 zeros[64] = { 0 }, early[64], derived[64], th[64], empty[64];
    tlsc_hkdf_extract(alg, NULL, 0, zeros, hl, early);
    tlsc_hash_once(alg, "", 0, empty);
    tlsc_hkdf_expand_label(alg, early, "derived", empty, hl, derived, hl);
    tlsc_hkdf_extract(alg, derived, hl, shared, shl, c->secret_hs);
    tlsc_wipe(shared, sizeof shared);
    tls_transcript_hash(c, alg, th);                                 /* ClientHello .. ServerHello */
    tlsc_hkdf_expand_label(alg, c->secret_hs, "c hs traffic", th, hl, c->secret_c_hs, hl);
    tlsc_hkdf_expand_label(alg, c->secret_hs, "s hs traffic", th, hl, c->secret_s_hs, hl);
    tlsc_hkdf_expand_label(alg, c->secret_hs, "derived", empty, hl, derived, hl);
    tlsc_hkdf_extract(alg, derived, hl, zeros, hl, c->secret_master);
    traffic_keys(c, c->secret_s_hs, &c->rd);
    traffic_keys(c, c->secret_c_hs, &c->wr);
    c->info.version = 0x0304;
    c->info.version_name = "TLS 1.3";
    c->info.suite_name = s->name;

    u8 type;
    const u8 *msg;
    size_t len;

    /* EncryptedExtensions */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_ENCRYPTED_EXTENSIONS) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    if ((rc = parse_encrypted_extensions(c, msg + 4, len - 4))) return rc;
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);

    /* [CertificateRequest] Certificate */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type == HS_CERTIFICATE_REQUEST) {
        rd_t r = { msg + 4, len - 4, 0 };
        unsigned cl = rd8(&r);
        rdn(&r, cl);
        unsigned el = rd16(&r);
        rdn(&r, el);
        if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        c->cert_requested = 1;                                       /* we have no certificate to offer: an empty Certificate will say so */
        tls_transcript_add(c, msg, len);
        tls_hs_consume(c, len);
        if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    }
    if (type != HS_CERTIFICATE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    {
        rd_t r = { msg + 4, len - 4, 0 };
        unsigned cl = rd8(&r);
        rdn(&r, cl);
        unsigned ll = rd24(&r);
        const u8 *list = rdn(&r, ll);
        if (r.err || r.n != 0 || cl != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        if (ll == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR);
        if ((rc = tls_verify_peer(c, list, ll, 1))) return rc;
    }
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);

    /* CertificateVerify: the signature covers the transcript up to and including the Certificate */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_CERTIFICATE_VERIFY) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    {
        rd_t r = { msg + 4, len - 4, 0 };
        unsigned scheme = rd16(&r);
        unsigned sl = rd16(&r);
        const u8 *sig = rdn(&r, sl);
        if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        u8 content[64 + 33 + 1 + 64];
        memset(content, 0x20, 64);
        memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
        content[97] = 0;
        tls_transcript_hash(c, alg, th);
        memcpy(content + 98, th, hl);
        if ((rc = tls_check_signature(c, scheme, 1, content, 98 + hl, sig, sl))) return rc;
    }
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);

    /* Finished */
    if ((rc = tls_get_handshake(c, &type, &msg, &len))) return rc;
    if (type != HS_FINISHED) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    if (len - 4 != hl) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
    {
        u8 fk[64], expect[64];
        tls_transcript_hash(c, alg, th);
        tlsc_hkdf_expand_label(alg, c->secret_s_hs, "finished", NULL, 0, fk, hl);
        tlsc_hmac(alg, fk, hl, th, hl, expect);
        int ok = tlsc_ct_memeq(expect, msg + 4, hl);
        tlsc_wipe(fk, sizeof fk);
        if (!ok) return tls_fail(c, TLS_E_FINISHED, AL_DECRYPT_ERROR);
    }
    tls_transcript_add(c, msg, len);
    tls_hs_consume(c, len);
    if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);      /* nothing may follow in the same flight */

    /* the application traffic secrets cover the transcript up to the server's Finished */
    tls_transcript_hash(c, alg, th);
    tlsc_hkdf_expand_label(alg, c->secret_master, "c ap traffic", th, hl, c->secret_c_ap, hl);
    tlsc_hkdf_expand_label(alg, c->secret_master, "s ap traffic", th, hl, c->secret_s_ap, hl);

    /* our flight: [dummy ChangeCipherSpec] [empty Certificate] Finished */
    if ((rc = send_ccs(c))) return rc;
    if (c->cert_requested) {
        static const u8 empty_cert[8] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
        if ((rc = tls_send_handshake(c, empty_cert, sizeof empty_cert))) return rc;
    }
    {
        u8 fk[64], vd[64], fin[4 + 64];
        tls_transcript_hash(c, alg, th);
        tlsc_hkdf_expand_label(alg, c->secret_c_hs, "finished", NULL, 0, fk, hl);
        tlsc_hmac(alg, fk, hl, th, hl, vd);
        tlsc_wipe(fk, sizeof fk);
        fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = (u8)hl;
        memcpy(fin + 4, vd, hl);
        if ((rc = tls_send_handshake(c, fin, 4 + hl))) return rc;
    }
    traffic_keys(c, c->secret_s_ap, &c->rd);
    traffic_keys(c, c->secret_c_ap, &c->wr);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- after the handshake */

static void next_secret(struct tls_conn *c, u8 *secret) {
    size_t hl = tlsc_hash_len(c->suite->hash);
    u8 n[64];
    tlsc_hkdf_expand_label(c->suite->hash, secret, "traffic upd", NULL, 0, n, hl);
    memcpy(secret, n, hl);
    tlsc_wipe(n, sizeof n);
}

int tls13_handle_post_handshake(struct tls_conn *c, const u8 *msg, size_t len) {
    switch (msg[0]) {
    case HS_NEW_SESSION_TICKET:                                      /* we keep no sessions */
        return 0;
    case HS_KEY_UPDATE: {
        if (len != 5 || msg[4] > 1) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        next_secret(c, c->secret_s_ap);
        traffic_keys(c, c->secret_s_ap, &c->rd);
        if (msg[4] == 1) {                                           /* the server wants our keys rolled too: say so under the old key, then switch */
            static const u8 ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
            int rc = tls_send_record(c, CT_HANDSHAKE, ku, sizeof ku);
            if (rc) return tls_fail(c, rc, -1);
            next_secret(c, c->secret_c_ap);
            traffic_keys(c, c->secret_c_ap, &c->wr);
        }
        return 0;
    }
    default:
        return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    }
}
