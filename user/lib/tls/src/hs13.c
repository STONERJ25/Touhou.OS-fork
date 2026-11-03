/*
 * The TLS 1.3 client handshake after the ServerHello (RFC 8446 4.3, 4.4, 7.1), one message per call, and the messages that may
 * follow the handshake (NewSessionTicket, KeyUpdate: 4.6).
 *
 *   ServerHello -> hs13_after_server_hello: the ECDHE secret and the key schedule up to the handshake traffic keys
 *   ST13_WAIT_EE       EncryptedExtensions
 *   ST13_WAIT_CERT_CR  CertificateRequest (answered with an empty Certificate) or Certificate
 *   ST13_WAIT_CV       CertificateVerify: the server's signature over the transcript
 *   ST13_WAIT_FIN      Finished, then our [ChangeCipherSpec] [Certificate] Finished and the application traffic keys
 */
#include "conn.h"

static void traffic_keys(TlsConn *c, const u8 *secret, cipher_state *cs) {
    int alg = c->suite->hash;
    u8 key[32], iv[12];
    int kl = cipher_keylen(c->suite->cipher);
    tlsc_hkdf_expand_label(alg, secret, "key", NULL, 0, key, (size_t)kl);
    tlsc_hkdf_expand_label(alg, secret, "iv", NULL, 0, iv, 12);
    rec_set_keys(cs, c->suite->cipher, key, iv, 12);
    tlsc_wipe(key, sizeof key);
}

/* RFC 8446 5.1: "Handshake messages MUST NOT span key changes": nothing may wait in the buffer when the keys change. */
static int key_change_boundary(TlsConn *c, const char *what) {
    if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake data after the %s in the same record (it must end at a key change)", what);
    return 0;
}

int hs13_after_server_hello(TlsConn *c) {
    const suite_info *s = c->suite;
    const int alg = s->hash;
    const size_t hl = tlsc_hash_len(alg);
    u8 shared[66];
    size_t shl;
    int rc;
    if ((rc = key_change_boundary(c, "ServerHello"))) return rc;
    if (c->sh_group == GROUP_X25519) {
        if (tlsc_x25519(shared, c->x25519_priv, c->sh_share) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server's x25519 share is a point of small order");
        shl = 32;
        c->info.group_name = "x25519";
    } else {
        int idx = c->sh_group == GROUP_SECP256R1 ? 0 : 1;
        int curve = idx == 0 ? TLSC_P256 : TLSC_P384;
        if (!c->ec_have[idx]) return tls_fail(c, TLS_E_PROTOCOL, AL_INTERNAL_ERROR, "no key for the server's group");
        if (tlsc_ecdh(curve, c->ec_priv[idx], c->sh_share, c->sh_share_len, shared) != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "the server's EC share is not a point on the curve");
        shl = tlsc_ec_size(curve);
        c->info.group_name = idx == 0 ? "secp256r1" : "secp384r1";
    }

    /* the key schedule (RFC 8446 7.1): no PSK, so the early secret is HKDF-Extract(0, 0) */
    u8 zeros[64] = { 0 }, early[64], derived[64], th[64], empty[64];
    tlsc_hkdf_extract(alg, NULL, 0, zeros, hl, early);
    tlsc_hash_once(alg, "", 0, empty);
    tlsc_hkdf_expand_label(alg, early, "derived", empty, hl, derived, hl);
    tlsc_hkdf_extract(alg, derived, hl, shared, shl, c->secret_hs);
    tlsc_wipe(shared, sizeof shared);
    tlsc_wipe(early, sizeof early);
    transcript_hash(c, alg, th);                                     /* ClientHello .. ServerHello */
    tlsc_hkdf_expand_label(alg, c->secret_hs, "c hs traffic", th, hl, c->secret_c_hs, hl);
    tlsc_hkdf_expand_label(alg, c->secret_hs, "s hs traffic", th, hl, c->secret_s_hs, hl);
    tlsc_hkdf_expand_label(alg, c->secret_hs, "derived", empty, hl, derived, hl);
    tlsc_hkdf_extract(alg, derived, hl, zeros, hl, c->secret_master);
    tlsc_wipe(derived, sizeof derived);
    traffic_keys(c, c->secret_s_hs, &c->rd);
    traffic_keys(c, c->secret_c_hs, &c->wr);
    tlsc_wipe(c->x25519_priv, sizeof c->x25519_priv);
    tlsc_wipe(c->ec_priv, sizeof c->ec_priv);
    c->info.version = 0x0304;
    c->info.version_name = "TLS 1.3";
    c->info.suite_name = s->name;
    c->state = ST13_WAIT_EE;
    return 0;
}

static int encrypted_extensions(TlsConn *c, const u8 *p, size_t n) {
    rd_t r = { p, n, 0 };
    rd_t e = rd_sub(&r, rd16(&r));
    if (r.err || e.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "malformed EncryptedExtensions");
    unsigned seen[OFFER_MAX];
    int nseen = 0;
    while (e.n > 0) {
        unsigned type = rd16(&e);
        rd_t d = rd_sub(&e, rd16(&e));
        if (e.err || d.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "malformed EncryptedExtensions");
        if (offered(seen, nseen, type) || nseen >= OFFER_MAX) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "EncryptedExtensions repeats extension 0x%04x", type);
        seen[nseen++] = type;
        if (!offered(c->offer.exts, c->offer.nexts, type))
            return tls_fail(c, TLS_E_SUITE, AL_UNSUPPORTED_EXTENSION, "the server answered with extension 0x%04x, which was not offered", type);
        if (type == EXT_SUPPORTED_VERSIONS || type == EXT_KEY_SHARE || type == EXT_SIGNATURE_ALGORITHMS || type == EXT_EC_POINT_FORMATS ||
            type == EXT_EXTENDED_MASTER_SECRET || type == EXT_RENEGOTIATION_INFO)
            return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER, "extension 0x%04x does not belong in EncryptedExtensions", type);
        if (type == EXT_ALPN) {                                      /* exactly one protocol, one of ours */
            rd_t list = rd_sub(&d, rd16(&d));
            unsigned nl = rd8(&list);
            const u8 *name = rdn(&list, nl);
            if (d.err || list.err || list.n != 0 || d.n != 0 || nl == 0 || nl >= sizeof c->info.alpn)
                return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed ALPN answer");
            char got[32];
            memcpy(got, name, nl);
            got[nl] = 0;
            int ok = 0;
            for (int i = 0; i < c->offer.nalpn; i++) ok |= strcmp(got, c->offer.alpn[i]) == 0;
            if (!ok) return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server picked application protocol '%s', which was not offered", got);
            memcpy(c->info.alpn, got, nl + 1);
        }
    }
    return 0;
}

/* Middlebox compatibility mode (RFC 8446 D.4) is what a non-empty legacy_session_id announces; only then the dummy CCS */
static int queue_ccs_once(TlsConn *c) {
    static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    if (c->ccs_sent || c->offer.sid_len == 0) return 0;
    c->ccs_sent = 1;
    return rec_queue_raw(c, ccs, sizeof ccs);
}

static int client_flight(TlsConn *c, const u8 *th) {
    const int alg = c->suite->hash;
    const size_t hl = tlsc_hash_len(alg);
    int rc;
    /* the application traffic secrets cover the transcript up to the server's Finished */
    tlsc_hkdf_expand_label(alg, c->secret_master, "c ap traffic", th, hl, c->secret_c_ap, hl);
    tlsc_hkdf_expand_label(alg, c->secret_master, "s ap traffic", th, hl, c->secret_s_ap, hl);
    if ((rc = queue_ccs_once(c))) return tls_fail(c, rc, AL_INTERNAL_ERROR, "could not queue ChangeCipherSpec");
    if (c->cert_requested) {                                         /* no client certificate: an empty list says so */
        static const u8 empty_cert[8] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
        if ((rc = hs_send(c, empty_cert, sizeof empty_cert))) return rc;
    }
    u8 fk[64], vd[64], fin[4 + 64], th2[64];
    transcript_hash(c, alg, th2);
    tlsc_hkdf_expand_label(alg, c->secret_c_hs, "finished", NULL, 0, fk, hl);
    tlsc_hmac(alg, fk, hl, th2, hl, vd);
    tlsc_wipe(fk, sizeof fk);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = (u8)hl;
    memcpy(fin + 4, vd, hl);
    if ((rc = hs_send(c, fin, 4 + hl))) return rc;
    traffic_keys(c, c->secret_s_ap, &c->rd);
    traffic_keys(c, c->secret_c_ap, &c->wr);
    tlsc_wipe(c->secret_hs, sizeof c->secret_hs);
    tlsc_wipe(c->secret_master, sizeof c->secret_master);
    tlsc_wipe(c->secret_c_hs, sizeof c->secret_c_hs);
    tlsc_wipe(c->secret_s_hs, sizeof c->secret_s_hs);
    c->state = ST_CONNECTED;
    return 0;
}

int hs13_message(TlsConn *c, unsigned type, const u8 *msg, size_t len) {
    const int alg = c->suite->hash;
    const size_t hl = tlsc_hash_len(alg);
    const u8 *body = msg + 4;
    size_t blen = len - 4;
    int rc;
    u8 th[64];
    switch (c->state) {
    case ST13_WAIT_EE:
        if (type != HS_ENCRYPTED_EXTENSIONS) break;
        if ((rc = encrypted_extensions(c, body, blen))) return rc;
        transcript_add(c, msg, len);
        c->state = ST13_WAIT_CERT_CR;
        return 0;
    case ST13_WAIT_CERT_CR:
        if (type == HS_CERTIFICATE_REQUEST) {
            rd_t r = { body, blen, 0 };
            rdn(&r, rd8(&r));                                        /* certificate_request_context */
            rd_t e = rd_sub(&r, rd16(&r));
            if (r.err || e.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed CertificateRequest");
            c->cert_requested = 1;                                   /* we have no certificate: an empty Certificate will say so */
            transcript_add(c, msg, len);
            c->state = ST13_WAIT_CERT;
            return 0;
        }
        /* fall through */
    case ST13_WAIT_CERT:
        if (type != HS_CERTIFICATE) break;
        {
            rd_t r = { body, blen, 0 };
            unsigned ctxlen = rd8(&r);
            rdn(&r, ctxlen);
            unsigned ll = rd24(&r);
            const u8 *list = rdn(&r, ll);
            if (r.err || r.n != 0 || ctxlen != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed Certificate message");
            if (ll == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR, "the server sent no certificate");
            if ((rc = verify_peer(c, list, ll, 1))) return rc;
        }
        transcript_add(c, msg, len);
        c->state = ST13_WAIT_CV;
        return 0;
    case ST13_WAIT_CV:
        if (type != HS_CERTIFICATE_VERIFY) break;
        {
            rd_t r = { body, blen, 0 };
            unsigned scheme = rd16(&r);
            unsigned sl = rd16(&r);
            const u8 *sig = rdn(&r, sl);
            if (r.err || r.n != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed CertificateVerify");
            /* the signed content: 64 spaces, the context string, a zero byte, the transcript hash up to the Certificate */
            u8 content[64 + 33 + 1 + 64];
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
            content[97] = 0;
            transcript_hash(c, alg, th);
            memcpy(content + 98, th, hl);
            if ((rc = check_handshake_signature(c, scheme, 1, content, 98 + hl, sig, sl))) return rc;
        }
        transcript_add(c, msg, len);
        c->state = ST13_WAIT_FIN;
        return 0;
    case ST13_WAIT_FIN:
        if (type != HS_FINISHED) break;
        if (blen != hl) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a Finished message of the wrong size");
        {
            u8 fk[64], expect[64];
            transcript_hash(c, alg, th);
            tlsc_hkdf_expand_label(alg, c->secret_s_hs, "finished", NULL, 0, fk, hl);
            tlsc_hmac(alg, fk, hl, th, hl, expect);
            int ok = tlsc_ct_memeq(expect, body, hl);
            tlsc_wipe(fk, sizeof fk);
            if (!ok) return tls_fail(c, TLS_E_FINISHED, AL_DECRYPT_ERROR, "the server's Finished message does not match the handshake");
        }
        transcript_add(c, msg, len);
        hs_consume(c, len);
        if ((rc = key_change_boundary(c, "server's Finished"))) return rc;
        transcript_hash(c, alg, th);
        return client_flight(c, th) ? c->err : 1;                    /* 1: the message is consumed already */
    default:
        return tls_fail(c, TLS_E_PROTOCOL, AL_INTERNAL_ERROR, "TLS 1.3 handshake in state %d", c->state);
    }
    return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "unexpected handshake message %u in the TLS 1.3 handshake (state %d)", type, c->state);
}

/* ---------------------------------------------------------------------------------------------------- after the handshake */

static void next_secret(TlsConn *c, u8 *secret) {
    size_t hl = tlsc_hash_len(c->suite->hash);
    u8 n[64];
    tlsc_hkdf_expand_label(c->suite->hash, secret, "traffic upd", NULL, 0, n, hl);
    memcpy(secret, n, hl);
    tlsc_wipe(n, sizeof n);
}

/* Our side's new keys: a KeyUpdate under the old ones, then the switch (RFC 8446 4.6.3). */
int hs13_key_update(TlsConn *c, int request_peer) {
    u8 ku[5] = { HS_KEY_UPDATE, 0, 0, 1, (u8)(request_peer ? 1 : 0) };
    if (rec_room(c) < sizeof ku) return TLS_WANT_WRITE;
    int rc = rec_queue(c, CT_HANDSHAKE, ku, sizeof ku);
    if (rc) return tls_fail(c, rc, AL_INTERNAL_ERROR, "could not queue a KeyUpdate");
    next_secret(c, c->secret_c_ap);
    traffic_keys(c, c->secret_c_ap, &c->wr);
    return 0;
}

int hs13_post_handshake(TlsConn *c, const u8 *msg, size_t len) {
    switch (msg[0]) {
    case HS_NEW_SESSION_TICKET:                                      /* no sessions are kept: read and forgotten */
        return 0;
    case HS_KEY_UPDATE: {
        if (len != 5 || msg[4] > 1) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed KeyUpdate");
        if (c->hs_len != len) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake data after a KeyUpdate in the same record");
        next_secret(c, c->secret_s_ap);
        traffic_keys(c, c->secret_s_ap, &c->rd);
        if (msg[4] == 1 && !c->tx_closed) return hs13_key_update(c, 0);   /* the server wants ours renewed too */
        return 0;
    }
    default:
        return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake message %u after the handshake", msg[0]);
    }
}
