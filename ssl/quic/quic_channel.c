/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "internal/quic_channel.h"
#include "internal/quic_error.h"
#include "internal/quic_rx_depack.h"
#include "../ssl_local.h"
#include "quic_channel_local.h"
#include <openssl/rand.h>

#define INIT_DCID_LEN           8
#define INIT_CRYPTO_BUF_LEN     8192
#define INIT_APP_BUF_LEN        8192

static int ch_rx(QUIC_CHANNEL *ch);
static int ch_tx(QUIC_CHANNEL *ch);
static void ch_tick(QUIC_TICK_RESULT *res, void *arg);
static void ch_rx_handle_packet(QUIC_CHANNEL *ch);
static OSSL_TIME ch_determine_next_tick_deadline(QUIC_CHANNEL *ch);
static int ch_retry(QUIC_CHANNEL *ch,
                    const unsigned char *retry_token,
                    size_t retry_token_len,
                    const QUIC_CONN_ID *retry_scid);
static void ch_cleanup(QUIC_CHANNEL *ch);
static int ch_generate_transport_params(QUIC_CHANNEL *ch);
static int ch_on_transport_params(const unsigned char *params,
                                  size_t params_len,
                                  void *arg);
static int ch_on_handshake_alert(void *arg, unsigned char alert_code);
static int ch_on_handshake_complete(void *arg);
static int ch_on_handshake_yield_secret(uint32_t enc_level, int direction,
                                        uint32_t suite_id, EVP_MD *md,
                                        const unsigned char *secret,
                                        size_t secret_len,
                                        void *arg);
static int ch_on_crypto_recv(unsigned char *buf, size_t buf_len,
                             size_t *bytes_read, void *arg);
static int crypto_ensure_empty(QUIC_RSTREAM *rstream);
static int ch_on_crypto_send(const unsigned char *buf, size_t buf_len,
                             size_t *consumed, void *arg);
static OSSL_TIME get_time(void *arg);
static uint64_t get_stream_limit(int uni, void *arg);
static int rx_early_validate(QUIC_PN pn, int pn_space, void *arg);
static int ch_retry(QUIC_CHANNEL *ch,
                    const unsigned char *retry_token,
                    size_t retry_token_len,
                    const QUIC_CONN_ID *retry_scid);
static void ch_update_idle(QUIC_CHANNEL *ch);
static int ch_discard_el(QUIC_CHANNEL *ch,
                         uint32_t enc_level);
static void ch_on_idle_timeout(QUIC_CHANNEL *ch);
static void ch_update_idle(QUIC_CHANNEL *ch);
static void ch_on_terminating_timeout(QUIC_CHANNEL *ch);
static void ch_start_terminating(QUIC_CHANNEL *ch,
                                 const QUIC_TERMINATE_CAUSE *tcause);

static int gen_rand_conn_id(OSSL_LIB_CTX *libctx, size_t len, QUIC_CONN_ID *cid)
{
    if (len > QUIC_MAX_CONN_ID_LEN)
        return 0;

    cid->id_len = (unsigned char)len;

    if (RAND_bytes_ex(libctx, cid->id, len, len * 8) != 1) {
        cid->id_len = 0;
        return 0;
    }

    return 1;
}

/*
 * QUIC Channel Initialization and Teardown
 * ========================================
 */
static int ch_init(QUIC_CHANNEL *ch)
{
    OSSL_QUIC_TX_PACKETISER_ARGS txp_args = {0};
    OSSL_QTX_ARGS qtx_args = {0};
    OSSL_QRX_ARGS qrx_args = {0};
    QUIC_DHS_ARGS dhs_args = {0};
    uint32_t pn_space;

    /* TODO(QUIC): This is only applicable to clients. */
    if (!gen_rand_conn_id(ch->libctx, INIT_DCID_LEN, &ch->init_dcid))
        goto err;

    /* We plug in a network write BIO to the QTX later when we get one. */
    qtx_args.mdpl = QUIC_MIN_INITIAL_DGRAM_LEN;
    ch->rx_max_udp_payload_size = qtx_args.mdpl;

    ch->qtx = ossl_qtx_new(&qtx_args);
    if (ch->qtx == NULL)
        goto err;

    ch->txpim = ossl_quic_txpim_new();
    if (ch->txpim == NULL)
        goto err;

    ch->cfq = ossl_quic_cfq_new();
    if (ch->cfq == NULL)
        goto err;

    if (!ossl_quic_txfc_init(&ch->conn_txfc, NULL))
        goto err;

    if (!ossl_quic_rxfc_init(&ch->conn_rxfc, NULL,
                             2  * 1024 * 1024,
                             10 * 1024 * 1024,
                             get_time, NULL))
        goto err;

    if (!ossl_statm_init(&ch->statm))
        goto err;

    ch->have_statm = 1;
    ch->cc_method = &ossl_cc_dummy_method;
    if ((ch->cc_data = ch->cc_method->new(NULL, NULL, NULL)) == NULL)
        goto err;

    if ((ch->ackm = ossl_ackm_new(get_time, NULL, &ch->statm,
                                  ch->cc_method, ch->cc_data)) == NULL)
        goto err;

    if (!ossl_quic_stream_map_init(&ch->qsm, get_stream_limit, ch))
        goto err;

    ch->have_qsm = 1;

    /* We use a zero-length SCID. */
    txp_args.cur_dcid           = ch->init_dcid;
    txp_args.ack_delay_exponent = 3;
    txp_args.qtx                = ch->qtx;
    txp_args.txpim              = ch->txpim;
    txp_args.cfq                = ch->cfq;
    txp_args.ackm               = ch->ackm;
    txp_args.qsm                = &ch->qsm;
    txp_args.conn_txfc          = &ch->conn_txfc;
    txp_args.conn_rxfc          = &ch->conn_rxfc;
    txp_args.cc_method          = ch->cc_method;
    txp_args.cc_data            = ch->cc_data;
    txp_args.now                = get_time;
    for (pn_space = QUIC_PN_SPACE_INITIAL; pn_space < QUIC_PN_SPACE_NUM; ++pn_space) {
        ch->crypto_send[pn_space] = ossl_quic_sstream_new(INIT_CRYPTO_BUF_LEN);
        if (ch->crypto_send[pn_space] == NULL)
            goto err;

        txp_args.crypto[pn_space] = ch->crypto_send[pn_space];
    }

    ch->txp = ossl_quic_tx_packetiser_new(&txp_args);
    if (ch->txp == NULL)
        goto err;

    if ((ch->demux = ossl_quic_demux_new(/*BIO=*/NULL, /*Short CID Len=*/0,
                                         get_time, NULL)) == NULL)
        goto err;

    qrx_args.demux              = ch->demux;
    qrx_args.short_conn_id_len  = 0; /* We use a zero-length SCID. */
    qrx_args.max_deferred       = 32;

    if ((ch->qrx = ossl_qrx_new(&qrx_args)) == NULL)
        goto err;

    if (!ossl_qrx_set_early_validation_cb(ch->qrx,
                                          rx_early_validate,
                                          ch))
        goto err;

    if (!ossl_qrx_add_dst_conn_id(ch->qrx, &txp_args.cur_scid))
        goto err;

    for (pn_space = QUIC_PN_SPACE_INITIAL; pn_space < QUIC_PN_SPACE_NUM; ++pn_space) {
        ch->crypto_recv[pn_space] = ossl_quic_rstream_new(NULL, NULL);
        if (ch->crypto_recv[pn_space] == NULL)
            goto err;
    }

    if ((ch->stream0 = ossl_quic_stream_map_alloc(&ch->qsm, 0,
                                                  QUIC_STREAM_INITIATOR_CLIENT
                                                  | QUIC_STREAM_DIR_BIDI)) == NULL)
        goto err;

    if ((ch->stream0->sstream = ossl_quic_sstream_new(INIT_APP_BUF_LEN)) == NULL)
        goto err;

    if ((ch->stream0->rstream = ossl_quic_rstream_new(NULL, NULL)) == NULL)
        goto err;

    if (!ossl_quic_txfc_init(&ch->stream0->txfc, &ch->conn_txfc))
        goto err;

    if (!ossl_quic_rxfc_init(&ch->stream0->rxfc, &ch->conn_rxfc,
                             1 * 1024 * 1024,
                             5 * 1024 * 1024,
                             get_time, NULL))
        goto err;

    /* Plug in the dummy handshake layer. */
    dhs_args.crypto_send_cb             = ch_on_crypto_send;
    dhs_args.crypto_send_cb_arg         = ch;
    dhs_args.crypto_recv_cb             = ch_on_crypto_recv;
    dhs_args.crypto_recv_cb_arg         = ch;
    dhs_args.yield_secret_cb            = ch_on_handshake_yield_secret;
    dhs_args.yield_secret_cb_arg        = ch;
    dhs_args.got_transport_params_cb    = ch_on_transport_params;
    dhs_args.got_transport_params_cb_arg= ch;
    dhs_args.handshake_complete_cb      = ch_on_handshake_complete;
    dhs_args.handshake_complete_cb_arg  = ch;
    dhs_args.alert_cb                   = ch_on_handshake_alert;
    dhs_args.alert_cb_arg               = ch;

    if ((ch->dhs = ossl_quic_dhs_new(&dhs_args)) == NULL)
        goto err;

    /*
     * Determine the QUIC Transport Parameters and serialize the transport
     * parameters block. (For servers, we do this later as we must defer
     * generation until we have received the client's transport parameters.)
     */
    if (!ch->is_server && !ch_generate_transport_params(ch))
        goto err;

    ch->rx_max_ack_delay        = QUIC_DEFAULT_MAX_ACK_DELAY;
    ch->rx_ack_delay_exp        = QUIC_DEFAULT_ACK_DELAY_EXP;
    ch->rx_active_conn_id_limit = QUIC_MIN_ACTIVE_CONN_ID_LIMIT;
    ch->max_idle_timeout        = QUIC_DEFAULT_IDLE_TIMEOUT;
    ch->tx_enc_level            = QUIC_ENC_LEVEL_INITIAL;
    ch_update_idle(ch);
    ossl_quic_reactor_init(&ch->rtor, ch_tick, ch,
                           ch_determine_next_tick_deadline(ch));
    return 1;

err:
    ch_cleanup(ch);
    return 0;
}

static void ch_cleanup(QUIC_CHANNEL *ch)
{
    uint32_t pn_space;

    if (ch->ackm != NULL)
        for (pn_space = QUIC_PN_SPACE_INITIAL;
             pn_space < QUIC_PN_SPACE_NUM;
             ++pn_space)
            ossl_ackm_on_pkt_space_discarded(ch->ackm, pn_space);

    ossl_quic_tx_packetiser_free(ch->txp);
    ossl_quic_txpim_free(ch->txpim);
    ossl_quic_cfq_free(ch->cfq);
    ossl_qtx_free(ch->qtx);
    if (ch->cc_data != NULL)
        ch->cc_method->free(ch->cc_data);
    if (ch->have_statm)
        ossl_statm_destroy(&ch->statm);
    ossl_ackm_free(ch->ackm);

    if (ch->stream0 != NULL) {
        assert(ch->have_qsm);
        ossl_quic_stream_map_release(&ch->qsm, ch->stream0); /* frees sstream */
    }

    if (ch->have_qsm)
        ossl_quic_stream_map_cleanup(&ch->qsm);

    for (pn_space = QUIC_PN_SPACE_INITIAL; pn_space < QUIC_PN_SPACE_NUM; ++pn_space) {
        ossl_quic_sstream_free(ch->crypto_send[pn_space]);
        ossl_quic_rstream_free(ch->crypto_recv[pn_space]);
    }

    ossl_qrx_pkt_release(ch->qrx_pkt);
    ch->qrx_pkt = NULL;

    ossl_quic_dhs_free(ch->dhs);
    ossl_qrx_free(ch->qrx);
    ossl_quic_demux_free(ch->demux);
    OPENSSL_free(ch->local_transport_params);
}

QUIC_CHANNEL *ossl_quic_channel_new(const QUIC_CHANNEL_ARGS *args)
{
    QUIC_CHANNEL *ch = NULL;

    if ((ch = OPENSSL_zalloc(sizeof(*ch))) == NULL)
        return NULL;

    ch->libctx      = args->libctx;
    ch->propq       = args->propq;
    ch->is_server   = args->is_server;

    if (!ch_init(ch)) {
        OPENSSL_free(ch);
        return NULL;
    }

    return ch;
}

void ossl_quic_channel_free(QUIC_CHANNEL *ch)
{
    if (ch == NULL)
        return;

    ch_cleanup(ch);
    OPENSSL_free(ch);
}

int ossl_quic_channel_get_peer_addr(QUIC_CHANNEL *ch, BIO_ADDR *peer_addr)
{
    *peer_addr = ch->cur_peer_addr;
    return 1;
}

int ossl_quic_channel_set_peer_addr(QUIC_CHANNEL *ch, const BIO_ADDR *peer_addr)
{
    ch->cur_peer_addr = *peer_addr;
    return 1;
}

QUIC_REACTOR *ossl_quic_channel_get_reactor(QUIC_CHANNEL *ch)
{
    return &ch->rtor;
}

QUIC_STREAM_MAP *ossl_quic_channel_get_qsm(QUIC_CHANNEL *ch)
{
    return &ch->qsm;
}

OSSL_STATM *ossl_quic_channel_get_statm(QUIC_CHANNEL *ch)
{
    return &ch->statm;
}

QUIC_STREAM *ossl_quic_channel_get_stream_by_id(QUIC_CHANNEL *ch,
                                                uint64_t stream_id)
{
    return ossl_quic_stream_map_get_by_id(&ch->qsm, stream_id);
}

int ossl_quic_channel_is_active(const QUIC_CHANNEL *ch)
{
    return ch != NULL && ch->state == QUIC_CHANNEL_STATE_ACTIVE;
}

int ossl_quic_channel_is_terminating(const QUIC_CHANNEL *ch)
{
    return ch->state == QUIC_CHANNEL_STATE_TERMINATING_CLOSING
        || ch->state == QUIC_CHANNEL_STATE_TERMINATING_DRAINING;
}

int ossl_quic_channel_is_terminated(const QUIC_CHANNEL *ch)
{
    return ch->state == QUIC_CHANNEL_STATE_TERMINATED;
}

int ossl_quic_channel_is_term_any(const QUIC_CHANNEL *ch)
{
    return ossl_quic_channel_is_terminating(ch)
        || ossl_quic_channel_is_terminated(ch);
}

int ossl_quic_channel_is_handshake_complete(const QUIC_CHANNEL *ch)
{
    return ch->handshake_complete;
}

/*
 * QUIC Channel: Callbacks from Miscellaneous Subsidiary Components
 * ================================================================
 */

/* Used by various components. */
static OSSL_TIME get_time(void *arg)
{
    return ossl_time_now();
}

/* Used by QSM. */
static uint64_t get_stream_limit(int uni, void *arg)
{
    QUIC_CHANNEL *ch = arg;

    return uni ? ch->max_local_streams_uni : ch->max_local_streams_bidi;
}

/*
 * Called by QRX to determine if a packet is potentially invalid before trying
 * to decrypt it.
 */
static int rx_early_validate(QUIC_PN pn, int pn_space, void *arg)
{
    QUIC_CHANNEL *ch = arg;

    /* Potential duplicates should not be processed. */
    if (!ossl_ackm_is_rx_pn_processable(ch->ackm, pn, pn_space))
        return 0;

    return 1;
}

/*
 * QUIC Channel: Handshake Layer Event Handling
 * ============================================
 */
static int ch_on_crypto_send(const unsigned char *buf, size_t buf_len,
                             size_t *consumed, void *arg)
{
    int ret;
    QUIC_CHANNEL *ch = arg;
    uint32_t enc_level = ch->tx_enc_level;
    uint32_t pn_space = ossl_quic_enc_level_to_pn_space(enc_level);
    QUIC_SSTREAM *sstream = ch->crypto_send[pn_space];

    if (!ossl_assert(sstream != NULL))
        return 0;

    ret = ossl_quic_sstream_append(sstream, buf, buf_len, consumed);
    return ret;
}

static int crypto_ensure_empty(QUIC_RSTREAM *rstream)
{
    size_t avail = 0;
    int is_fin = 0;

    if (rstream == NULL)
        return 1;

    if (!ossl_quic_rstream_available(rstream, &avail, &is_fin))
        return 0;

    return avail == 0;
}

static int ch_on_crypto_recv(unsigned char *buf, size_t buf_len,
                             size_t *bytes_read, void *arg)
{
    QUIC_CHANNEL *ch = arg;
    QUIC_RSTREAM *rstream;
    int is_fin = 0; /* crypto stream is never finished, so we don't use this */
    uint32_t i;

    /*
     * After we move to a later EL we must not allow our peer to send any new
     * bytes in the crypto stream on a previous EL. Retransmissions of old bytes
     * are allowed.
     *
     * In practice we will only move to a new EL when we have consumed all bytes
     * which should be sent on the crypto stream at a previous EL. For example,
     * the Handshake EL should not be provisioned until we have completely
     * consumed a TLS 1.3 ServerHello. Thus when we provision an EL the output
     * of ossl_quic_rstream_available() should be 0 for all lower ELs. Thus if a
     * given EL is available we simply ensure we have not received any further
     * bytes at a lower EL.
     */
    for (i = QUIC_ENC_LEVEL_INITIAL; i < ch->tx_enc_level; ++i)
        if (i != QUIC_ENC_LEVEL_0RTT &&
            !crypto_ensure_empty(ch->crypto_recv[ossl_quic_enc_level_to_pn_space(i)])) {
            /* Protocol violation (RFC 9001 s. 4.1.3) */
            ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_PROTOCOL_VIOLATION,
                                                   OSSL_QUIC_FRAME_TYPE_CRYPTO,
                                                   "crypto stream data in wrong EL");
            return 0;
        }

    rstream = ch->crypto_recv[ossl_quic_enc_level_to_pn_space(ch->tx_enc_level)];
    if (rstream == NULL)
        return 0;

    return ossl_quic_rstream_read(rstream, buf, buf_len, bytes_read,
                                  &is_fin);
}

static int ch_on_handshake_yield_secret(uint32_t enc_level, int direction,
                                        uint32_t suite_id, EVP_MD *md,
                                        const unsigned char *secret,
                                        size_t secret_len,
                                        void *arg)
{
    QUIC_CHANNEL *ch = arg;
    uint32_t i;

    if (enc_level < QUIC_ENC_LEVEL_HANDSHAKE || enc_level >= QUIC_ENC_LEVEL_NUM)
        /* Invalid EL. */
        return 0;

    if (enc_level <= ch->tx_enc_level)
        /*
         * Does not make sense for us to try and provision an EL we have already
         * attained.
         */
        return 0;

    /*
     * Ensure all crypto streams for previous ELs are now empty of available
     * data.
     */
    for (i = QUIC_ENC_LEVEL_INITIAL; i < enc_level; ++i)
        if (!crypto_ensure_empty(ch->crypto_recv[i])) {
            /* Protocol violation (RFC 9001 s. 4.1.3) */
            ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_PROTOCOL_VIOLATION,
                                                   OSSL_QUIC_FRAME_TYPE_CRYPTO,
                                                   "crypto stream data in wrong EL");
            return 0;
        }

    if (direction) {
        /* TX */
        if (!ossl_qtx_provide_secret(ch->qtx, enc_level,
                                     suite_id, md,
                                     secret, secret_len))
            return 0;

        ch->tx_enc_level = enc_level;
    } else {
        /* RX */
        if (!ossl_qrx_provide_secret(ch->qrx, enc_level,
                                     suite_id, md,
                                     secret, secret_len))
            return 0;

        ch->have_new_rx_secret = 1;
    }

    return 1;
}

static int ch_on_handshake_complete(void *arg)
{
    QUIC_CHANNEL *ch = arg;

    if (ch->handshake_complete)
        return 0; /* this should not happen twice */

    if (!ossl_assert(ch->tx_enc_level == QUIC_ENC_LEVEL_1RTT))
        return 0;

    if (!ch->got_remote_transport_params)
        /*
         * Was not a valid QUIC handshake if we did not get valid transport
         * params.
         */
        return 0;

    /* Don't need transport parameters anymore. */
    OPENSSL_free(ch->local_transport_params);
    ch->local_transport_params = NULL;

    /* Tell TXP the handshake is complete. */
    ossl_quic_tx_packetiser_notify_handshake_complete(ch->txp);

    ch->handshake_complete = 1;
    return 1;
}

static int ch_on_handshake_alert(void *arg, unsigned char alert_code)
{
    QUIC_CHANNEL *ch = arg;

    ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_CRYPTO_ERR_BEGIN + alert_code,
                                           0, "handshake alert");
    return 1;
}

/*
 * QUIC Channel: Transport Parameter Handling
 * ==========================================
 */

/*
 * Called by handshake layer when we receive QUIC Transport Parameters from the
 * peer. Note that these are not authenticated until the handshake is marked
 * as complete.
 */
static int ch_on_transport_params(const unsigned char *params,
                                  size_t params_len,
                                  void *arg)
{
    QUIC_CHANNEL *ch = arg;
    PACKET pkt;
    uint64_t id, v;
    size_t len;
    const unsigned char *body;
    int got_orig_dcid = 0;
    int got_initial_scid = 0;
    int got_retry_scid = 0;
    int got_initial_max_data = 0;
    int got_initial_max_stream_data_bidi_local = 0;
    int got_initial_max_stream_data_bidi_remote = 0;
    int got_initial_max_stream_data_uni = 0;
    int got_initial_max_streams_bidi = 0;
    int got_initial_max_streams_uni = 0;
    int got_ack_delay_exp = 0;
    int got_max_ack_delay = 0;
    int got_max_udp_payload_size = 0;
    int got_max_idle_timeout = 0;
    int got_active_conn_id_limit = 0;
    QUIC_CONN_ID cid;

    if (ch->got_remote_transport_params)
        goto malformed;

    if (!PACKET_buf_init(&pkt, params, params_len))
        return 0;

    while (PACKET_remaining(&pkt) > 0) {
        if (!ossl_quic_wire_peek_transport_param(&pkt, &id))
            goto malformed;

        switch (id) {
            case QUIC_TPARAM_ORIG_DCID:
                if (got_orig_dcid)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_cid(&pkt, NULL, &cid))
                    goto malformed;

                /* Must match our initial DCID. */
                if (!ossl_quic_conn_id_eq(&ch->init_dcid, &cid))
                    goto malformed;

                got_orig_dcid = 1;
                break;

            case QUIC_TPARAM_RETRY_SCID:
                if (got_retry_scid || !ch->doing_retry)
                    /* must not appear more than once or if retry not done */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_cid(&pkt, NULL, &cid))
                    goto malformed;

                /* Must match Retry packet SCID. */
                if (!ossl_quic_conn_id_eq(&ch->retry_scid, &cid))
                    goto malformed;

                got_retry_scid = 1;
                break;

            case QUIC_TPARAM_INITIAL_SCID:
                if (got_initial_scid)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_cid(&pkt, NULL, &cid))
                    goto malformed;

                /* Must match SCID of first Initial packet from server. */
                if (!ossl_quic_conn_id_eq(&ch->init_scid, &cid))
                    goto malformed;

                got_initial_scid = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_DATA:
                if (got_initial_max_data)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v))
                    goto malformed;

                ossl_quic_txfc_bump_cwm(&ch->conn_txfc, v);
                got_initial_max_data = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL:
                if (got_initial_max_stream_data_bidi_local)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v))
                    goto malformed;

                /*
                 * This is correct; the BIDI_LOCAL TP governs streams created by
                 * the endpoint which sends the TP, i.e., our peer.
                 */
                ch->init_max_stream_data_bidi_remote = v;
                got_initial_max_stream_data_bidi_local = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE:
                if (got_initial_max_stream_data_bidi_remote)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v))
                    goto malformed;

                /*
                 * This is correct; the BIDI_REMOTE TP governs streams created
                 * by the endpoint which receives the TP, i.e., us.
                 */
                ch->init_max_stream_data_bidi_local = v;

                /* Apply to stream 0. */
                ossl_quic_txfc_bump_cwm(&ch->stream0->txfc, v);
                got_initial_max_stream_data_bidi_remote = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_UNI:
                if (got_initial_max_stream_data_uni)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v))
                    goto malformed;

                ch->init_max_stream_data_uni_remote = v;
                got_initial_max_stream_data_uni = 1;
                break;

            case QUIC_TPARAM_ACK_DELAY_EXP:
                if (got_ack_delay_exp)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v > QUIC_MAX_ACK_DELAY_EXP)
                    goto malformed;

                ch->rx_ack_delay_exp = (unsigned char)v;
                got_ack_delay_exp = 1;
                break;

            case QUIC_TPARAM_MAX_ACK_DELAY:
                if (got_max_ack_delay)
                    /* must not appear more than once */
                    return 0;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v >= (((uint64_t)1) << 14))
                    goto malformed;

                ch->rx_max_ack_delay = v;
                got_max_ack_delay = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_STREAMS_BIDI:
                if (got_initial_max_streams_bidi)
                    /* must not appear more than once */
                    return 0;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v > (((uint64_t)1) << 60))
                    goto malformed;

                assert(ch->max_local_streams_bidi == 0);
                ch->max_local_streams_bidi = v;
                got_initial_max_streams_bidi = 1;
                break;

            case QUIC_TPARAM_INITIAL_MAX_STREAMS_UNI:
                if (got_initial_max_streams_uni)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v > (((uint64_t)1) << 60))
                    goto malformed;

                assert(ch->max_local_streams_uni == 0);
                ch->max_local_streams_uni = v;
                got_initial_max_streams_uni = 1;
                break;

            case QUIC_TPARAM_MAX_IDLE_TIMEOUT:
                if (got_max_idle_timeout)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v))
                    goto malformed;

                if (v < ch->max_idle_timeout)
                    ch->max_idle_timeout = v;

                ch_update_idle(ch);
                got_max_idle_timeout = 1;
                break;

            case QUIC_TPARAM_MAX_UDP_PAYLOAD_SIZE:
                if (got_max_udp_payload_size)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v < QUIC_MIN_INITIAL_DGRAM_LEN)
                    goto malformed;

                ch->rx_max_udp_payload_size = v;
                got_max_udp_payload_size    = 1;
                break;

            case QUIC_TPARAM_ACTIVE_CONN_ID_LIMIT:
                if (got_active_conn_id_limit)
                    /* must not appear more than once */
                    goto malformed;

                if (!ossl_quic_wire_decode_transport_param_int(&pkt, &id, &v)
                    || v < QUIC_MIN_ACTIVE_CONN_ID_LIMIT)
                    goto malformed;

                ch->rx_active_conn_id_limit = v;
                got_active_conn_id_limit = 1;
                break;

            /*
             * TODO(QUIC): Handle:
             *   QUIC_TPARAM_STATELESS_RESET_TOKEN
             *   QUIC_TPARAM_PREFERRED_ADDR
             */

            case QUIC_TPARAM_DISABLE_ACTIVE_MIGRATION:
                /* We do not currently handle migration, so nothing to do. */
            default:
                /* Skip over and ignore. */
                body = ossl_quic_wire_decode_transport_param_bytes(&pkt, &id,
                                                                   &len);
                if (body == NULL)
                    goto malformed;

                break;
        }
    }

    if (!got_orig_dcid || !got_initial_scid || got_retry_scid != ch->doing_retry)
        /* Transport parameters were not valid. */
        goto malformed;

    ch->got_remote_transport_params = 1;

    if (got_initial_max_data || got_initial_max_stream_data_bidi_remote
        || got_initial_max_streams_bidi || got_initial_max_streams_uni)
        /* If FC credit was bumped, we may now be able to send. */
        ossl_quic_stream_map_update_state(&ch->qsm, ch->stream0);

    /* If we are a server, we now generate our own transport parameters. */
    if (ch->is_server && !ch_generate_transport_params(ch)) {
        ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_INTERNAL_ERROR, 0,
                                               "internal error");
        return 0;
    }

    return 1;

malformed:
    ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_TRANSPORT_PARAMETER_ERROR,
                                           0, "bad transport parameter");
    return 0;
}

/*
 * Called when we want to generate transport parameters. This is called
 * immediately at instantiation time for a client and after we receive the
 * client's transport parameters for a server.
 */
static int ch_generate_transport_params(QUIC_CHANNEL *ch)
{
    int ok = 0;
    BUF_MEM *buf_mem = NULL;
    WPACKET wpkt;
    int wpkt_valid = 0;
    size_t buf_len = 0;

    if (ch->local_transport_params != NULL)
        goto err;

    if ((buf_mem = BUF_MEM_new()) == NULL)
        goto err;

    if (!WPACKET_init(&wpkt, buf_mem))
        goto err;

    wpkt_valid = 1;

    if (ossl_quic_wire_encode_transport_param_bytes(&wpkt, QUIC_TPARAM_DISABLE_ACTIVE_MIGRATION,
                                                    NULL, 0) == NULL)
        goto err;

    if (ossl_quic_wire_encode_transport_param_bytes(&wpkt, QUIC_TPARAM_INITIAL_SCID,
                                                    NULL, 0) == NULL)
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_MAX_IDLE_TIMEOUT,
                                                   ch->max_idle_timeout))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_MAX_UDP_PAYLOAD_SIZE,
                                                   QUIC_MIN_INITIAL_DGRAM_LEN))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_ACTIVE_CONN_ID_LIMIT,
                                                   4))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_DATA,
                                                   ossl_quic_rxfc_get_cwm(&ch->conn_rxfc)))
        goto err;

    /*
     * We actually want the default CWM for a new RXFC, but here we just use
     * stream0 as a representative specimen. TODO(QUIC): revisit this when we
     * support multiple streams.
     */
    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
                                                   ossl_quic_rxfc_get_cwm(&ch->stream0->rxfc)))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
                                                   ossl_quic_rxfc_get_cwm(&ch->stream0->rxfc)))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_STREAM_DATA_UNI,
                                                   ossl_quic_rxfc_get_cwm(&ch->stream0->rxfc)))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_STREAMS_BIDI,
                                                   0))
        goto err;

    if (!ossl_quic_wire_encode_transport_param_int(&wpkt, QUIC_TPARAM_INITIAL_MAX_STREAMS_UNI,
                                                   0))
        goto err;

    if (!WPACKET_get_total_written(&wpkt, &buf_len))
        goto err;

    ch->local_transport_params = (unsigned char *)buf_mem->data;
    buf_mem->data = NULL;

    if (!WPACKET_finish(&wpkt))
        goto err;

    wpkt_valid = 0;

    if (!ossl_quic_dhs_set_transport_params(ch->dhs, ch->local_transport_params,
                                            buf_len))
        goto err;

    ok = 1;
err:
    if (wpkt_valid)
        WPACKET_cleanup(&wpkt);
    BUF_MEM_free(buf_mem);
    return ok;
}

/*
 * QUIC Channel: Ticker-Mutator
 * ============================
 */

/*
 * The central ticker function called by the reactor. This does everything, or
 * at least everything network I/O related. Best effort - not allowed to fail
 * "loudly".
 */
static void ch_tick(QUIC_TICK_RESULT *res, void *arg)
{
    OSSL_TIME now, deadline;
    QUIC_CHANNEL *ch = arg;

    /*
     * When we tick the QUIC connection, we do everything we need to do
     * periodically. In order, we:
     *
     *   - handle any incoming data from the network;
     *   - handle any timer events which are due to fire (ACKM, etc.)
     *   - write any data to the network due to be sent, to the extent
     *     possible;
     *   - determine the time at which we should next be ticked.
     */

    /* If we are in the TERMINATED state, there is nothing to do. */
    if (ossl_quic_channel_is_terminated(ch)) {
        res->want_net_read  = 0;
        res->want_net_write = 0;
        res->tick_deadline  = ossl_time_infinite();
        return;
    }

    /*
     * If we are in the TERMINATING state, check if the terminating timer has
     * expired.
     */
    if (ossl_quic_channel_is_terminating(ch)) {
        now = ossl_time_now();

        if (ossl_time_compare(now, ch->terminate_deadline) >= 0) {
            ch_on_terminating_timeout(ch);
            res->want_net_read  = 0;
            res->want_net_write = 0;
            res->tick_deadline  = ossl_time_infinite();
            return; /* abort normal processing, nothing to do */
        }
    }

    do {
        /* Handle any incoming data from the network. */
        ch_rx(ch);

        /*
         * Allow the handshake layer to check for any new incoming data and generate
         * new outgoing data.
         */
        ch->have_new_rx_secret = 0;
        ossl_quic_dhs_tick(ch->dhs);

        /*
         * If the handshake layer gave us a new secret, we need to do RX again
         * because packets that were not previously processable and were
         * deferred might now be processable.
         */
    } while (ch->have_new_rx_secret);

    /*
     * Handle any timer events which are due to fire; namely, the loss detection
     * deadline and the idle timeout.
     *
     * ACKM ACK generation deadline is polled by TXP, so we don't need to handle
     * it here.
     */
    now = ossl_time_now();
    if (ossl_time_compare(now, ch->idle_deadline) >= 0) {
        /*
         * Idle timeout differs from normal protocol violation because we do not
         * send a CONN_CLOSE frame; go straight to TERMINATED.
         */
        ch_on_idle_timeout(ch);
        res->want_net_read  = 0;
        res->want_net_write = 0;
        res->tick_deadline  = ossl_time_infinite();
        return;
    }

    deadline = ossl_ackm_get_loss_detection_deadline(ch->ackm);
    if (!ossl_time_is_zero(deadline) && ossl_time_compare(now, deadline) >= 0)
        ossl_ackm_on_timeout(ch->ackm);

    /* Write any data to the network due to be sent. */
    ch_tx(ch);

    /* Determine the time at which we should next be ticked. */
    res->tick_deadline = ch_determine_next_tick_deadline(ch);

    /* Always process network input. */
    res->want_net_read = 1;

    /* We want to write to the network if we have any in our queue. */
    res->want_net_write = (ossl_qtx_get_queue_len_datagrams(ch->qtx) > 0);
}

/* Process incoming packets and handle frames, if any. */
static int ch_rx(QUIC_CHANNEL *ch)
{
    int handled_any = 0;

    if (!ch->have_sent_any_pkt)
        /*
         * We have not sent anything yet, therefore there is no need to check
         * for incoming data. TODO SERVER
         */
        return 1;

    /*
     * Get DEMUX to BIO_recvmmsg from the network and queue incoming datagrams
     * to the appropriate QRX instance.
     */
    ossl_quic_demux_pump(ch->demux); /* best effort */

    for (;;) {
        assert(ch->qrx_pkt == NULL);

        if (!ossl_qrx_read_pkt(ch->qrx, &ch->qrx_pkt))
            break;

        if (!handled_any)
            ch_update_idle(ch);

        ch_rx_handle_packet(ch); /* best effort */

        /*
         * Regardless of the outcome of frame handling, unref the packet.
         * This will free the packet unless something added another
         * reference to it during frame processing.
         */
        ossl_qrx_pkt_release(ch->qrx_pkt);
        ch->qrx_pkt = NULL;

        handled_any = 1;
    }

    /*
     * When in TERMINATING - CLOSING, generate a CONN_CLOSE frame whenever we
     * process one or more incoming packets.
     */
    if (handled_any && ch->state == QUIC_CHANNEL_STATE_TERMINATING_CLOSING)
        ch->conn_close_queued = 1;

    return 1;
}

/* Handles the packet currently in ch->qrx_pkt->hdr. */
static void ch_rx_handle_packet(QUIC_CHANNEL *ch)
{
    uint32_t enc_level;

    assert(ch->qrx_pkt != NULL);

    if (ossl_quic_pkt_type_is_encrypted(ch->qrx_pkt->hdr->type)) {
        if (!ch->have_received_enc_pkt) {
            ch->init_scid = ch->qrx_pkt->hdr->src_conn_id;
            ch->have_received_enc_pkt = 1;

            /*
             * We change to using the SCID in the first Initial packet as the
             * DCID.
             */
            ossl_quic_tx_packetiser_set_cur_dcid(ch->txp, &ch->init_scid);
        }

        enc_level = ossl_quic_pkt_type_to_enc_level(ch->qrx_pkt->hdr->type);
        if ((ch->el_discarded & (1U << enc_level)) != 0)
            /* Do not process packets from ELs we have already discarded. */
            return;
    }

    /* Handle incoming packet. */
    switch (ch->qrx_pkt->hdr->type) {
        case QUIC_PKT_TYPE_RETRY:
            if (ch->doing_retry)
                /* It is not allowed to ask a client to do a retry more than
                 * once. */
                return;

            /* TODO if server */

            if (ch->qrx_pkt->hdr->len <= QUIC_RETRY_INTEGRITY_TAG_LEN)
                /* Packets with zero-length Retry Tokens are invalid. */
                return;

            /*
             * TODO(QUIC): Theoretically this should probably be in the QRX.
             * However because validation is dependent on context (namely the
             * client's initial DCID) we can't do this cleanly. In the future we
             * should probably add a callback to the QRX to let it call us (via
             * the DEMUX) and ask us about the correct original DCID, rather
             * than allow the QRX to emit a potentially malformed packet to the
             * upper layers. However, special casing this will do for now.
             */
            if (!ossl_quic_validate_retry_integrity_tag(ch->libctx,
                                                        ch->propq,
                                                        ch->qrx_pkt->hdr,
                                                        &ch->init_dcid))
                /* Malformed retry packet, ignore. */
                return;

            ch_retry(ch, ch->qrx_pkt->hdr->data,
                     ch->qrx_pkt->hdr->len - QUIC_RETRY_INTEGRITY_TAG_LEN,
                     &ch->qrx_pkt->hdr->src_conn_id);
            break;

        case QUIC_PKT_TYPE_VERSION_NEG:
            /* TODO(QUIC): Implement version negotiation */
            break;

        case QUIC_PKT_TYPE_0RTT:
            /* TODO if server */
            /* Clients should never receive 0-RTT packets */
            break;

        default:
            if (ch->qrx_pkt->hdr->type == QUIC_PKT_TYPE_HANDSHAKE)
                /*
                 * We automatically drop INITIAL EL keys when first successfully
                 * decrypting a HANDSHAKE packet, as per the RFC.
                 */
                ch_discard_el(ch, QUIC_ENC_LEVEL_INITIAL);

            /* This packet contains frames, pass to the RXDP. */
            ossl_quic_handle_frames(ch, ch->qrx_pkt); /* best effort */
            break;
    }
}

/* Try to generate packets and if possible, flush them to the network. */
static int ch_tx(QUIC_CHANNEL *ch)
{
    if (ch->state == QUIC_CHANNEL_STATE_TERMINATING_CLOSING) {
        /*
         * While closing, only send CONN_CLOSE if we've received more traffic
         * from the peer. Once we tell the TXP to generate CONN_CLOSE, all
         * future calls to it generate CONN_CLOSE frames, so otherwise we would
         * just constantly generate CONN_CLOSE frames.
         */
        if (!ch->conn_close_queued)
            return 0;

        ch->conn_close_queued = 0;
    }

    /*
     * Send a packet, if we need to. Best effort. The TXP consults the CC and
     * applies any limitations imposed by it, so we don't need to do it here.
     *
     * Best effort. In particular if TXP fails for some reason we should still
     * flush any queued packets which we already generated.
     */
    if (ossl_quic_tx_packetiser_generate(ch->txp,
                                         TX_PACKETISER_ARCHETYPE_NORMAL)
        == TX_PACKETISER_RES_SENT_PKT)
        ch->have_sent_any_pkt = 1;

    ossl_qtx_flush_net(ch->qtx); /* best effort */
    return 1;
}

/* Determine next tick deadline. */
static OSSL_TIME ch_determine_next_tick_deadline(QUIC_CHANNEL *ch)
{
    OSSL_TIME deadline;
    uint32_t pn_space;

    deadline = ossl_ackm_get_loss_detection_deadline(ch->ackm);
    if (ossl_time_is_zero(deadline))
        deadline = ossl_time_infinite();

    for (pn_space = QUIC_PN_SPACE_INITIAL; pn_space < QUIC_PN_SPACE_NUM; ++pn_space)
        deadline = ossl_time_min(deadline,
                                 ossl_ackm_get_ack_deadline(ch->ackm, pn_space));

    /* When will CC let us send more? */
    if (ossl_quic_tx_packetiser_has_pending(ch->txp, TX_PACKETISER_ARCHETYPE_NORMAL,
                                            TX_PACKETISER_BYPASS_CC))
        deadline = ossl_time_min(deadline,
                                 ch->cc_method->get_next_credit_time(ch->cc_data));

    /* Is the terminating timer armed? */
    if (ossl_quic_channel_is_terminating(ch))
        deadline = ossl_time_min(deadline,
                                 ch->terminate_deadline);
    else if (!ossl_time_is_infinite(ch->idle_deadline))
        deadline = ossl_time_min(deadline,
                                 ch->idle_deadline);

    return deadline;
}

/*
 * QUIC Channel: Network BIO Configuration
 * =======================================
 */

/* Determines whether we can support a given poll descriptor. */
static int validate_poll_descriptor(const BIO_POLL_DESCRIPTOR *d)
{
    if (d->type == BIO_POLL_DESCRIPTOR_TYPE_SOCK_FD && d->value.fd < 0)
        return 0;

    return 1;
}

BIO *ossl_quic_channel_get_net_rbio(QUIC_CHANNEL *ch)
{
    return ch->net_rbio;
}

BIO *ossl_quic_channel_get_net_wbio(QUIC_CHANNEL *ch)
{
    return ch->net_wbio;
}

/*
 * QUIC_CHANNEL does not ref any BIO it is provided with, nor is any ref
 * transferred to it. The caller (i.e., QUIC_CONNECTION) is responsible for
 * ensuring the BIO lasts until the channel is freed or the BIO is switched out
 * for another BIO by a subsequent successful call to this function.
 */
int ossl_quic_channel_set_net_rbio(QUIC_CHANNEL *ch, BIO *net_rbio)
{
    BIO_POLL_DESCRIPTOR d = {0};

    if (ch->net_rbio == net_rbio)
        return 1;

    if (net_rbio != NULL) {
        if (!BIO_get_rpoll_descriptor(net_rbio, &d))
            /* Non-pollable BIO */
            d.type = BIO_POLL_DESCRIPTOR_TYPE_NONE;

        if (!validate_poll_descriptor(&d))
            return 0;
    }

    ossl_quic_reactor_set_poll_r(&ch->rtor, &d);
    ossl_quic_demux_set_bio(ch->demux, net_rbio);
    ch->net_rbio = net_rbio;
    return 1;
}

int ossl_quic_channel_set_net_wbio(QUIC_CHANNEL *ch, BIO *net_wbio)
{
    BIO_POLL_DESCRIPTOR d = {0};

    if (ch->net_wbio == net_wbio)
        return 1;

    if (net_wbio != NULL) {
        if (!BIO_get_wpoll_descriptor(net_wbio, &d))
            /* Non-pollable BIO */
            d.type = BIO_POLL_DESCRIPTOR_TYPE_NONE;

        if (!validate_poll_descriptor(&d))
            return 0;
    }

    ossl_quic_reactor_set_poll_w(&ch->rtor, &d);
    ossl_qtx_set_bio(ch->qtx, net_wbio);
    ch->net_wbio = net_wbio;
    return 1;
}

/*
 * QUIC Channel: Lifecycle Events
 * ==============================
 */

int ossl_quic_channel_start(QUIC_CHANNEL *ch)
{
    /* TODO SERVER */
    if (ch->state != QUIC_CHANNEL_STATE_IDLE)
        /* Calls to connect are idempotent */
        return 1;

    /* Inform QTX of peer address. */
    if (!ossl_quic_tx_packetiser_set_peer(ch->txp, &ch->cur_peer_addr))
        return 0;

    /* Plug in secrets for the Initial EL. */
    if (!ossl_quic_provide_initial_secret(ch->libctx,
                                          ch->propq,
                                          &ch->init_dcid,
                                          /*is_server=*/0,
                                          ch->qrx, ch->qtx))
        return 0;

    /* Change state. */
    ch->state                   = QUIC_CHANNEL_STATE_ACTIVE;
    ch->doing_proactive_ver_neg = 0; /* not currently supported */

    /* Handshake layer: start (e.g. send CH). */
    if (!ossl_quic_dhs_tick(ch->dhs))
        return 0;

    ossl_quic_reactor_tick(&ch->rtor); /* best effort */
    return 1;
}

/* Start a locally initiated connection shutdown. */
void ossl_quic_channel_local_close(QUIC_CHANNEL *ch)
{
    QUIC_TERMINATE_CAUSE tcause = {0};

    if (ossl_quic_channel_is_term_any(ch))
        return;

    tcause.app = 1;
    ch_start_terminating(ch, &tcause);
}

static void free_token(const unsigned char *buf, size_t buf_len, void *arg)
{
    OPENSSL_free((unsigned char *)buf);
}

/* Called when a server asks us to do a retry. */
static int ch_retry(QUIC_CHANNEL *ch,
                    const unsigned char *retry_token,
                    size_t retry_token_len,
                    const QUIC_CONN_ID *retry_scid)
{
    void *buf;

    /* We change to using the SCID in the Retry packet as the DCID. */
    if (!ossl_quic_tx_packetiser_set_cur_dcid(ch->txp, retry_scid))
        return 0;

    /*
     * Now we retry. We will release the Retry packet immediately, so copy
     * the token.
     */
    if ((buf = OPENSSL_malloc(retry_token_len)) == NULL)
        return 0;

    memcpy(buf, retry_token, retry_token_len);

    ossl_quic_tx_packetiser_set_initial_token(ch->txp, buf, retry_token_len,
                                              free_token, NULL);

    ch->retry_scid  = *retry_scid;
    ch->doing_retry = 1;

    /*
     * We need to stimulate the Initial EL to generate the first CRYPTO frame
     * again. We can do this most cleanly by simply forcing the ACKM to consider
     * the first Initial packet as lost, which it effectively was as the server
     * hasn't processed it. This also maintains the desired behaviour with e.g.
     * PNs not resetting and so on.
     *
     * The PN we used initially is always zero, because QUIC does not allow
     * repeated retries.
     */
    if (!ossl_ackm_mark_packet_pseudo_lost(ch->ackm, QUIC_PN_SPACE_INITIAL,
                                           /*PN=*/0))
        return 0;

    /*
     * Plug in new secrets for the Initial EL. This is the only time we change
     * the secrets for an EL after we already provisioned it.
     */
    if (!ossl_quic_provide_initial_secret(ch->libctx,
                                          ch->propq,
                                          &ch->retry_scid,
                                          /*is_server=*/0,
                                          ch->qrx, ch->qtx))
        return 0;

    return 1;
}

/* Called when an EL is to be discarded. */
static int ch_discard_el(QUIC_CHANNEL *ch,
                         uint32_t enc_level)
{
    if (!ossl_assert(enc_level < QUIC_ENC_LEVEL_1RTT))
        return 0;

    if ((ch->el_discarded & (1U << enc_level)) != 0)
        /* Already done. */
        return 1;

    /* Best effort for all of these. */
    ossl_quic_tx_packetiser_discard_enc_level(ch->txp, enc_level);
    ossl_qrx_discard_enc_level(ch->qrx, enc_level);
    ossl_qtx_discard_enc_level(ch->qtx, enc_level);

    if (enc_level != QUIC_ENC_LEVEL_0RTT) {
        uint32_t pn_space = ossl_quic_enc_level_to_pn_space(enc_level);

        ossl_ackm_on_pkt_space_discarded(ch->ackm, pn_space);

        /* We should still have crypto streams at this point. */
        assert(ch->crypto_send[pn_space] != NULL);
        assert(ch->crypto_recv[pn_space] != NULL);

        /* Get rid of the crypto stream state for the EL. */
        ossl_quic_sstream_free(ch->crypto_send[pn_space]);
        ch->crypto_send[pn_space] = NULL;

        ossl_quic_rstream_free(ch->crypto_recv[pn_space]);
        ch->crypto_recv[pn_space] = NULL;
    }

    ch->el_discarded |= (1U << enc_level);
    return 1;
}

/* Intended to be called by the RXDP. */
int ossl_quic_channel_on_handshake_confirmed(QUIC_CHANNEL *ch)
{
    if (ch->handshake_confirmed)
        return 1;

    if (!ch->handshake_complete) {
        /*
         * Does not make sense for handshake to be confirmed before it is
         * completed.
         */
        ossl_quic_channel_raise_protocol_error(ch, QUIC_ERR_PROTOCOL_VIOLATION,
                                               OSSL_QUIC_FRAME_TYPE_HANDSHAKE_DONE,
                                               "handshake cannot be confirmed "
                                               "before it is completed");
        return 0;
    }

    ch_discard_el(ch, QUIC_ENC_LEVEL_HANDSHAKE);
    ch->handshake_confirmed = 1;
    return 1;
}

/*
 * Master function used when we want to start tearing down a connection:
 *
 *   - If the connection is still IDLE we can go straight to TERMINATED;
 *
 *   - If we are already TERMINATED this is a no-op.
 *
 *   - If we are TERMINATING - CLOSING and we have now got a CONNECTION_CLOSE
 *     from the peer (tcause->remote == 1), we move to TERMINATING - CLOSING.
 *
 *   - If we are TERMINATING - DRAINING, we remain here until the terminating
 *     timer expires.
 *
 *   - Otherwise, we are in ACTIVE and move to TERMINATING - CLOSING.
 *     if we caused the termination (e.g. we have sent a CONNECTION_CLOSE). Note
 *     that we are considered to have caused a termination if we sent the first
 *     CONNECTION_CLOSE frame, even if it is caused by a peer protocol
 *     violation. If the peer sent the first CONNECTION_CLOSE frame, we move to
 *     TERMINATING - DRAINING.
 *
 * We record the termination cause structure passed on the first call only.
 * Any successive calls have their termination cause data discarded;
 * once we start sending a CONNECTION_CLOSE frame, we don't change the details
 * in it.
 */
static void ch_start_terminating(QUIC_CHANNEL *ch,
                                 const QUIC_TERMINATE_CAUSE *tcause)
{
    switch (ch->state) {
        default:
        case QUIC_CHANNEL_STATE_IDLE:
            ch->terminate_cause = *tcause;
            ch_on_terminating_timeout(ch);
            break;

        case QUIC_CHANNEL_STATE_ACTIVE:
            ch->state = tcause->remote ? QUIC_CHANNEL_STATE_TERMINATING_DRAINING
                                       : QUIC_CHANNEL_STATE_TERMINATING_CLOSING;
            ch->terminate_cause = *tcause;
            ch->terminate_deadline
                = ossl_time_add(ossl_time_now(),
                                ossl_time_multiply(ossl_ackm_get_pto_duration(ch->ackm),
                                                   3));

            if (!tcause->remote) {
                OSSL_QUIC_FRAME_CONN_CLOSE f = {0};

                /* best effort */
                f.error_code = ch->terminate_cause.error_code;
                f.frame_type = ch->terminate_cause.frame_type;
                f.is_app     = ch->terminate_cause.app;
                ossl_quic_tx_packetiser_schedule_conn_close(ch->txp, &f);
                ch->conn_close_queued = 1;
            }
            break;

        case QUIC_CHANNEL_STATE_TERMINATING_CLOSING:
            if (tcause->remote)
                ch->state = QUIC_CHANNEL_STATE_TERMINATING_DRAINING;

            break;

        case QUIC_CHANNEL_STATE_TERMINATING_DRAINING:
            /* We remain here until the timout expires. */
            break;

        case QUIC_CHANNEL_STATE_TERMINATED:
            /* No-op. */
            break;
    }
}

/* For RXDP use. */
void ossl_quic_channel_on_remote_conn_close(QUIC_CHANNEL *ch,
                                            OSSL_QUIC_FRAME_CONN_CLOSE *f)
{
    QUIC_TERMINATE_CAUSE tcause = {0};

    if (!ossl_quic_channel_is_active(ch))
        return;

    tcause.remote     = 1;
    tcause.app        = f->is_app;
    tcause.error_code = f->error_code;
    tcause.frame_type = f->frame_type;

    ch_start_terminating(ch, &tcause);
}

void ossl_quic_channel_raise_protocol_error(QUIC_CHANNEL *ch,
                                            uint64_t error_code,
                                            uint64_t frame_type,
                                            const char *reason)
{
    QUIC_TERMINATE_CAUSE tcause = {0};

    tcause.error_code = error_code;
    tcause.frame_type = frame_type;

    ch_start_terminating(ch, &tcause);
}

/*
 * Called once the terminating timer expires, meaning we move from TERMINATING
 * to TERMINATED.
 */
static void ch_on_terminating_timeout(QUIC_CHANNEL *ch)
{
    ch->state = QUIC_CHANNEL_STATE_TERMINATED;
}

/*
 * Updates our idle deadline. Called when an event happens which should bump the
 * idle timeout.
 */
static void ch_update_idle(QUIC_CHANNEL *ch)
{
    if (ch->max_idle_timeout == 0)
        ch->idle_deadline = ossl_time_infinite();
    else
        ch->idle_deadline = ossl_time_add(ossl_time_now(),
            ossl_ms2time(ch->max_idle_timeout));
}

/* Called when the idle timeout expires. */
static void ch_on_idle_timeout(QUIC_CHANNEL *ch)
{
    /*
     * Idle timeout does not have an error code associated with it because a
     * CONN_CLOSE is never sent for it. We shouldn't use this data once we reach
     * TERMINATED anyway.
     */
    ch->terminate_cause.app         = 0;
    ch->terminate_cause.error_code  = UINT64_MAX;
    ch->terminate_cause.frame_type  = 0;

    ch->state = QUIC_CHANNEL_STATE_TERMINATED;
}
