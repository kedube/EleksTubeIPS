/*
 * Dynamic TLS record buffers, for the HTTPS server's connections only.
 *
 * mbedTLS gives each connection a 16 KB receive buffer and a 16 KB send buffer for as long as it's open,
 * which is too much for the clock. ESP-IDF can allocate them only while a record is being received or
 * sent (CONFIG_MBEDTLS_DYNAMIC_BUFFER), by wrapping these mbedTLS functions at link time, but Arduino's
 * prebuilt mbedTLS is built without it. This is that code, from ESP-IDF v4.4.6
 * components/mbedtls/port/dynamic, which matches Arduino 2.0.14's mbedTLS 2.28, with these changes:
 *
 * - Client connections, such as the weather's, are left to mbedTLS as they were.
 * - A record's header is read into the idle receive buffer, not onto the stack, so one that arrives in
 *   parts survives a non-blocking read returning MBEDTLS_ERR_SSL_WANT_READ.
 * - Sending allocates for what is sent, at most TX_CHUNK at a time, instead of 16 KB.
 * - The handshake's messages are small (the clock's own certificate is about 500 bytes), so they're
 *   sent from a small buffer too.
 * - The idle send buffer has room for an alert that mbedTLS sends by itself.
 * - After a handshake step, the buffers are freed for the step that ran, which ESP-IDF got wrong when a
 *   session is resumed, see manage_resource().
 *
 * Original: SPDX-FileCopyrightText: 2020-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <sys/param.h>
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_internal.h"
#include "mbedtls/platform.h"

#define TX_CHUNK 4096			// The most one mbedtls_ssl_write() sends
#define HANDSHAKE_TX 1024		// Room for any one of the server's handshake messages but the certificate

#define COUNTER_SIZE 8
#define CACHE_IV_SIZE 16
#define TX_CACHE_SIZE (CACHE_IV_SIZE + COUNTER_SIZE)
#define TX_IDLE_SIZE 64			// The cache, and an encrypted alert written into it: 13 + 8 + 2 + 16
#define RX_CACHE_SIZE 16
#define RX_IDLE_SIZE (RX_CACHE_SIZE + MBEDTLS_SSL_HEADER_LEN)	// The cache, then the next record's header

typedef enum {
	BUF_CACHED,		// Holds a record being received or sent
	BUF_NO_CACHED,	// Idle: only the counters, and the next record's header
} buf_state;

struct ssl_buf {
	buf_state state;
	unsigned int len;
	unsigned char buf[];
};

#define BUF_HEAD_SIZE ((int)offsetof(struct ssl_buf, buf))

int __real_mbedtls_ssl_handshake_server_step(mbedtls_ssl_context *ssl);
int __real_mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len);
int __real_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len);
void __real_mbedtls_ssl_free(mbedtls_ssl_context *ssl);
int __real_mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl);
int __real_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf);
int __real_mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message);
int __real_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl);

static int dynamic(const mbedtls_ssl_context *ssl) {
	return ssl->conf != NULL && ssl->conf->endpoint == MBEDTLS_SSL_IS_SERVER;
}

static struct ssl_buf *head(unsigned char *buf) {
	return __containerof(buf, struct ssl_buf, buf[0]);
}

static void free_buf(unsigned char *buf) {
	mbedtls_free(head(buf));
}

static unsigned char *alloc_buf(size_t len, buf_state state) {
	struct ssl_buf *b = mbedtls_calloc(1, BUF_HEAD_SIZE + len);
	if (b == NULL) {
		return NULL;
	}
	b->state = state;
	b->len = len;
	return b->buf;
}

static size_t record_len(size_t len) {
	return len + MBEDTLS_SSL_HEADER_LEN + MBEDTLS_SSL_COMPRESSION_ADD + MBEDTLS_MAX_IV_LENGTH + MBEDTLS_SSL_MAC_ADD + MBEDTLS_SSL_PADDING_ADD;
}

// In mbedTLS, ssl->out_msg = ssl->out_buf + offset. Without a buffer, the offset is kept in ssl->out_msg.
static void init_tx_buffer(mbedtls_ssl_context *ssl, unsigned char *buf) {
	if (!buf) {
		int out_msg_off = (int)ssl->out_msg - (int)ssl->out_buf;
		if (!out_msg_off) {
			out_msg_off = MBEDTLS_SSL_HEADER_LEN;
		}
		ssl->out_buf = NULL;
		ssl->out_ctr = NULL;
		ssl->out_hdr = NULL;
		ssl->out_len = NULL;
		ssl->out_iv  = NULL;
		ssl->out_msg = (unsigned char *)out_msg_off;
	} else {
		int out_msg_off = (int)ssl->out_msg;
		ssl->out_buf = buf;
		ssl->out_ctr = ssl->out_buf;
		ssl->out_hdr = ssl->out_buf +  8;
		ssl->out_len = ssl->out_buf + 11;
		ssl->out_iv  = ssl->out_buf + MBEDTLS_SSL_HEADER_LEN;
		ssl->out_msg = ssl->out_buf + out_msg_off;
	}

	ssl->out_msgtype = 0;
	ssl->out_msglen = 0;
	ssl->out_left = 0;
}

static void init_rx_buffer(mbedtls_ssl_context *ssl, unsigned char *buf) {
	if (!buf) {
		int in_msg_off = (int)ssl->in_msg - (int)ssl->in_buf;
		if (!in_msg_off) {
			in_msg_off = MBEDTLS_SSL_HEADER_LEN;
		}
		ssl->in_buf = NULL;
		ssl->in_ctr = NULL;
		ssl->in_hdr = NULL;
		ssl->in_len = NULL;
		ssl->in_iv  = NULL;
		ssl->in_msg = (unsigned char *)in_msg_off;
	} else {
		int in_msg_off = (int)ssl->in_msg;
		ssl->in_buf = buf;
		ssl->in_ctr = ssl->in_buf;
		ssl->in_hdr = ssl->in_buf +  8;
		ssl->in_len = ssl->in_buf + 11;
		ssl->in_iv  = ssl->in_buf + MBEDTLS_SSL_HEADER_LEN;
		ssl->in_msg = ssl->in_buf + in_msg_off;
	}

	ssl->in_msgtype = 0;
	ssl->in_msglen = 0;
	ssl->in_left = 0;
}

// Replaces the send buffer with an idle one that keeps the counter and IV, or makes the first one
static int idle_tx_buffer(mbedtls_ssl_context *ssl) {
	unsigned char cache[TX_CACHE_SIZE] = { 0 };
	if (ssl->out_buf) {
		memcpy(cache, ssl->out_ctr, COUNTER_SIZE);
		memcpy(cache + COUNTER_SIZE, ssl->out_iv, CACHE_IV_SIZE);
		free_buf(ssl->out_buf);
		init_tx_buffer(ssl, NULL);
	}

	unsigned char *buf = alloc_buf(TX_IDLE_SIZE, BUF_NO_CACHED);
	if (!buf) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	memcpy(buf, cache, TX_CACHE_SIZE);
	init_tx_buffer(ssl, buf);	// At the same offset
	return 0;
}

// Replaces the receive buffer with an idle one that keeps the counter, or makes the first one
static int idle_rx_buffer(mbedtls_ssl_context *ssl) {
	unsigned char cache[RX_CACHE_SIZE] = { 0 };
	if (ssl->in_buf) {
		memcpy(cache, ssl->in_ctr, 8);
		memcpy(cache + 8, ssl->in_iv, 8);
		free_buf(ssl->in_buf);
		init_rx_buffer(ssl, NULL);
	}

	unsigned char *buf = alloc_buf(RX_IDLE_SIZE, BUF_NO_CACHED);
	if (!buf) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	memcpy(buf, cache, RX_CACHE_SIZE);
	init_rx_buffer(ssl, buf);
	return 0;
}

// A buffer for sending a record of up to len bytes, unless one is still being sent
static int add_tx_buffer(mbedtls_ssl_context *ssl, size_t len) {
	unsigned char cache[TX_CACHE_SIZE];
	int cached = 0;

	if (ssl->out_buf) {
		if (head(ssl->out_buf)->state == BUF_CACHED) {
			return 0;
		}
		memcpy(cache, ssl->out_buf, TX_CACHE_SIZE);
		free_buf(ssl->out_buf);
		init_tx_buffer(ssl, NULL);
		cached = 1;
	}

	unsigned char *buf = alloc_buf(record_len(len), BUF_CACHED);
	if (!buf) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	init_tx_buffer(ssl, buf);
	if (cached) {
		memcpy(ssl->out_ctr, cache, COUNTER_SIZE);
		memcpy(ssl->out_iv, cache + COUNTER_SIZE, CACHE_IV_SIZE);
	}
	return 0;
}

static int free_tx_buffer(mbedtls_ssl_context *ssl) {
	if (!ssl->out_buf || head(ssl->out_buf)->state == BUF_NO_CACHED) {
		return 0;
	}
	return idle_tx_buffer(ssl);
}

// Reads the next record's header and makes a buffer the record's size, unless one is still being read
static int add_rx_buffer(mbedtls_ssl_context *ssl) {
	int ret;

	if (!ssl->in_buf && (ret = idle_rx_buffer(ssl)) != 0) {
		return ret;
	}
	if (head(ssl->in_buf)->state == BUF_CACHED) {
		return 0;
	}

	// Into the idle buffer, where what's come so far is kept if the rest hasn't
	unsigned char *msg_head = ssl->in_buf + RX_CACHE_SIZE;
	ssl->in_hdr = msg_head;
	ssl->in_len = msg_head + 3;
	if ((ret = mbedtls_ssl_fetch_input(ssl, mbedtls_ssl_in_hdr_len(ssl))) != 0) {
		return ret;
	}

	size_t in_left = ssl->in_left;
	size_t in_msglen = (ssl->in_len[0] << 8) | ssl->in_len[1];
	if (in_msglen > MBEDTLS_SSL_IN_BUFFER_LEN - MBEDTLS_SSL_HEADER_LEN) {
		return MBEDTLS_ERR_SSL_INVALID_RECORD;	// Bigger than mbedTLS takes
	}
	// record_len() allows for more than a record's length includes, so a full one is capped to mbedTLS's own size
	size_t buffer_len = MIN(record_len(in_msglen), MBEDTLS_SSL_IN_BUFFER_LEN);

	unsigned char cache[RX_IDLE_SIZE];
	memcpy(cache, ssl->in_buf, RX_CACHE_SIZE + in_left);
	free_buf(ssl->in_buf);
	init_rx_buffer(ssl, NULL);

	unsigned char *buf = alloc_buf(buffer_len, BUF_CACHED);
	if (!buf) {
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	init_rx_buffer(ssl, buf);
	memcpy(ssl->in_ctr, cache, 8);
	memcpy(ssl->in_iv, cache + 8, 8);
	memcpy(ssl->in_hdr, cache + RX_CACHE_SIZE, in_left);
	ssl->in_left = in_left;
	ssl->in_msglen = 0;
	return 0;
}

static int free_rx_buffer(mbedtls_ssl_context *ssl) {
	// Not while more than one handshake message from the record is still to be read
	if (!ssl->in_buf || (ssl->in_hslen && (ssl->in_hslen < ssl->in_msglen)) || head(ssl->in_buf)->state == BUF_NO_CACHED) {
		return 0;
	}

	// The record was skipped, so "ssl->in_msglen = 0". The server reads the ClientHello by itself, without
	// setting in_msgtype, and is then in the ServerHello state.
	if (!ssl->in_msgtype && !(ssl->conf->endpoint == MBEDTLS_SSL_IS_SERVER && ssl->state == MBEDTLS_SSL_SERVER_HELLO)) {
		return 0;
	}

	return idle_rx_buffer(ssl);
}

static size_t crt_size(mbedtls_x509_crt *cert, size_t *num) {
	size_t n = 0;
	size_t bytes = 0;
	while (cert) {
		bytes += cert->raw.len;
		n++;
		cert = cert->next;
	}
	*num = n;
	return bytes;
}

#define CHECK(f) do { int _ret = (f); if (_ret != 0) return _ret; } while (0)

// Before and after the handshake step for state, the one about to run or that just ran. ESP-IDF took the step
// that just ran to be ssl->state - 1, but a server resuming a session goes from ClientFinished to WRAPUP, so
// the buffer the client's Finished was read into was kept, and the next record was read into it.
static int manage_resource(mbedtls_ssl_context *ssl, int add, int state) {

	if (ssl->state == MBEDTLS_SSL_HANDSHAKE_OVER || ssl->handshake == NULL) {
		return 0;
	}

	if (!add && !ssl->out_left) {
		CHECK(free_tx_buffer(ssl));
	}

	switch (state) {
		case MBEDTLS_SSL_HELLO_REQUEST:
			ssl->major_ver = MBEDTLS_SSL_MAJOR_VERSION_3;
			break;

		case MBEDTLS_SSL_CLIENT_HELLO:
		case MBEDTLS_SSL_CLIENT_CERTIFICATE:
		case MBEDTLS_SSL_CLIENT_KEY_EXCHANGE:
		case MBEDTLS_SSL_CERTIFICATE_VERIFY:
		case MBEDTLS_SSL_CLIENT_CHANGE_CIPHER_SPEC:
		case MBEDTLS_SSL_CLIENT_FINISHED:
			CHECK(add ? add_rx_buffer(ssl) : free_rx_buffer(ssl));
			break;

		case MBEDTLS_SSL_SERVER_CERTIFICATE:
			if (add) {
				size_t buffer_len = 3;
				mbedtls_ssl_key_cert *key_cert = ssl->conf->key_cert;
				while (key_cert && key_cert->cert) {
					size_t num;
					buffer_len += crt_size(key_cert->cert, &num);
					buffer_len += num * 3;
					key_cert = key_cert->next;
				}
				CHECK(add_tx_buffer(ssl, MAX(buffer_len + 4, HANDSHAKE_TX)));
			}
			break;

		case MBEDTLS_SSL_SERVER_HELLO:
		case MBEDTLS_SSL_SERVER_KEY_EXCHANGE:
		case MBEDTLS_SSL_CERTIFICATE_REQUEST:
		case MBEDTLS_SSL_SERVER_HELLO_DONE:
		case MBEDTLS_SSL_SERVER_CHANGE_CIPHER_SPEC:	// Also the session ticket, about 200 bytes
		case MBEDTLS_SSL_SERVER_FINISHED:
			if (add) {
				CHECK(add_tx_buffer(ssl, HANDSHAKE_TX));
			}
			break;

		default:
			break;
	}

	return 0;
}

// For the HTTPS server's figures: whether a handshake resumed an earlier session. mbedTLS frees what says so
// when the handshake ends, so the server step notes it.
static const mbedtls_ssl_context *resumed[8];

static void note_resumed(const mbedtls_ssl_context *ssl) {
	for (int i = 0; i < 8; i++) {
		if (resumed[i] == ssl) {
			return;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (resumed[i] == NULL) {
			resumed[i] = ssl;
			return;
		}
	}
}

int tls_handshake_resumed(const mbedtls_ssl_context *ssl) {	// And forgets it
	for (int i = 0; i < 8; i++) {
		if (resumed[i] == ssl) {
			resumed[i] = NULL;
			return 1;
		}
	}
	return 0;
}

int __wrap_mbedtls_ssl_handshake_server_step(mbedtls_ssl_context *ssl) {
	int state = ssl->state;
	CHECK(manage_resource(ssl, 1, state));
	int ret = __real_mbedtls_ssl_handshake_server_step(ssl);
	if (ssl->handshake && ssl->handshake->resume) {
		note_resumed(ssl);
	}
	CHECK(ret);
	CHECK(manage_resource(ssl, 0, state));
	return 0;
}

// mbedTLS's own, which are static
static void ssl_update_checksum_start(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len) {
#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
	mbedtls_md5_update_ret(&ssl->handshake->fin_md5 , buf, len);
	mbedtls_sha1_update_ret(&ssl->handshake->fin_sha1, buf, len);
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
	mbedtls_sha256_update_ret(&ssl->handshake->fin_sha256, buf, len);
#endif
#if defined(MBEDTLS_SHA512_C)
	mbedtls_sha512_update_ret(&ssl->handshake->fin_sha512, buf, len);
#endif
#endif
}

static void ssl_handshake_params_init(mbedtls_ssl_handshake_params *handshake) {
	memset(handshake, 0, sizeof(mbedtls_ssl_handshake_params));

#if defined(MBEDTLS_SSL_PROTO_SSL3) || defined(MBEDTLS_SSL_PROTO_TLS1) || defined(MBEDTLS_SSL_PROTO_TLS1_1)
	mbedtls_md5_init(&handshake->fin_md5);
	mbedtls_sha1_init(&handshake->fin_sha1);
	mbedtls_md5_starts_ret(&handshake->fin_md5);
	mbedtls_sha1_starts_ret(&handshake->fin_sha1);
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#if defined(MBEDTLS_SHA256_C)
	mbedtls_sha256_init(&handshake->fin_sha256);
	mbedtls_sha256_starts_ret(&handshake->fin_sha256, 0);
#endif
#if defined(MBEDTLS_SHA512_C)
	mbedtls_sha512_init(&handshake->fin_sha512);
	mbedtls_sha512_starts_ret(&handshake->fin_sha512, 1);
#endif
#endif

	handshake->update_checksum = ssl_update_checksum_start;

#if defined(MBEDTLS_SSL_PROTO_TLS1_2) && defined(MBEDTLS_KEY_EXCHANGE_WITH_CERT_ENABLED)
	mbedtls_ssl_sig_hash_set_init(&handshake->hash_algs);
#endif
#if defined(MBEDTLS_DHM_C)
	mbedtls_dhm_init(&handshake->dhm_ctx);
#endif
#if defined(MBEDTLS_ECDH_C)
	mbedtls_ecdh_init(&handshake->ecdh_ctx);
#endif
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
	mbedtls_ecjpake_init(&handshake->ecjpake_ctx);
#if defined(MBEDTLS_SSL_CLI_C)
	handshake->ecjpake_cache = NULL;
	handshake->ecjpake_cache_len = 0;
#endif
#endif
#if defined(MBEDTLS_SSL_ECP_RESTARTABLE)
	mbedtls_x509_crt_restart_init(&handshake->ecrs_ctx);
#endif
#if defined(MBEDTLS_SSL_SERVER_NAME_INDICATION)
	handshake->sni_authmode = MBEDTLS_SSL_VERIFY_UNSET;
#endif
#if defined(MBEDTLS_X509_CRT_PARSE_C) && !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
	mbedtls_pk_init(&handshake->peer_pubkey);
#endif
}

static int ssl_handshake_init(mbedtls_ssl_context *ssl) {
	if (ssl->transform_negotiate) {
		mbedtls_ssl_transform_free(ssl->transform_negotiate);
	}
	if (ssl->session_negotiate) {
		mbedtls_ssl_session_free(ssl->session_negotiate);
	}
	if (ssl->handshake) {
		mbedtls_ssl_handshake_free(ssl);
	}

	if (ssl->transform_negotiate == NULL) {
		ssl->transform_negotiate = mbedtls_calloc(1, sizeof(mbedtls_ssl_transform));
	}
	if (ssl->session_negotiate == NULL) {
		ssl->session_negotiate = mbedtls_calloc(1, sizeof(mbedtls_ssl_session));
	}
	if (ssl->handshake == NULL) {
		ssl->handshake = mbedtls_calloc(1, sizeof(mbedtls_ssl_handshake_params));
	}

	if (ssl->handshake == NULL || ssl->transform_negotiate == NULL || ssl->session_negotiate == NULL) {
		mbedtls_free(ssl->handshake);
		mbedtls_free(ssl->transform_negotiate);
		mbedtls_free(ssl->session_negotiate);
		ssl->handshake = NULL;
		ssl->transform_negotiate = NULL;
		ssl->session_negotiate = NULL;
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}

	mbedtls_ssl_session_init(ssl->session_negotiate);
	mbedtls_ssl_transform_init(ssl->transform_negotiate);
	ssl_handshake_params_init(ssl->handshake);
	return 0;
}

int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf) {
	if (conf->endpoint != MBEDTLS_SSL_IS_SERVER || conf->transport != MBEDTLS_SSL_TRANSPORT_STREAM) {
		return __real_mbedtls_ssl_setup(ssl, conf);
	}

	ssl->conf = conf;
	CHECK(ssl_handshake_init(ssl));

	// Without a buffer, ssl->out_msg and ssl->in_msg hold the offset that their buffers will have
	ssl->out_buf = NULL;
	ssl->out_msg = (unsigned char *)MBEDTLS_SSL_HEADER_LEN;
	CHECK(idle_tx_buffer(ssl));

	ssl->in_buf = NULL;
	ssl->in_msg = (unsigned char *)MBEDTLS_SSL_HEADER_LEN;
	CHECK(idle_rx_buffer(ssl));
	return 0;
}

int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len) {
	if (!dynamic(ssl)) {
		return __real_mbedtls_ssl_write(ssl, buf, len);
	}

	// What mbedTLS doesn't take now, it's asked to send again
	len = MIN(len, TX_CHUNK);
	CHECK(add_tx_buffer(ssl, len));
	int ret = __real_mbedtls_ssl_write(ssl, buf, len);
	if (!ssl->out_left) {
		CHECK(free_tx_buffer(ssl));
	}
	return ret;
}

int __wrap_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len) {
	if (!dynamic(ssl)) {
		return __real_mbedtls_ssl_read(ssl, buf, len);
	}

	int ret = add_rx_buffer(ssl);
	if (ret == MBEDTLS_ERR_SSL_CONN_EOF) {
		return 0;
	} else if (ret < 0) {
		return ret;
	}

	ret = __real_mbedtls_ssl_read(ssl, buf, len);
	if (!ssl->in_msglen) {
		CHECK(free_rx_buffer(ssl));
	}
	return ret;
}

void __wrap_mbedtls_ssl_free(mbedtls_ssl_context *ssl) {
	if (dynamic(ssl)) {
		if (ssl->out_buf) {
			free_buf(ssl->out_buf);
			ssl->out_buf = NULL;
		}
		if (ssl->in_buf) {
			free_buf(ssl->in_buf);
			ssl->in_buf = NULL;
		}
	}

	__real_mbedtls_ssl_free(ssl);
}

int __wrap_mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl) {
	if (!dynamic(ssl)) {
		return __real_mbedtls_ssl_session_reset(ssl);
	}

	// mbedTLS clears both buffers whole, so they have to be full size for it
	if (ssl->out_buf) {
		free_buf(ssl->out_buf);
	}
	unsigned char *out = alloc_buf(MBEDTLS_SSL_OUT_BUFFER_LEN, BUF_CACHED);
	if (!out) {
		ssl->out_buf = NULL;
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	ssl->out_msg = (unsigned char *)MBEDTLS_SSL_HEADER_LEN;
	init_tx_buffer(ssl, out);

	if (ssl->in_buf) {
		free_buf(ssl->in_buf);
	}
	unsigned char *in = alloc_buf(MBEDTLS_SSL_IN_BUFFER_LEN, BUF_CACHED);
	if (!in) {
		ssl->in_buf = NULL;
		return MBEDTLS_ERR_SSL_ALLOC_FAILED;
	}
	ssl->in_msg = (unsigned char *)MBEDTLS_SSL_HEADER_LEN;
	init_rx_buffer(ssl, in);

	CHECK(__real_mbedtls_ssl_session_reset(ssl));

	CHECK(idle_tx_buffer(ssl));
	CHECK(idle_rx_buffer(ssl));
	return 0;
}

int __wrap_mbedtls_ssl_send_alert_message(mbedtls_ssl_context *ssl, unsigned char level, unsigned char message) {
	if (!dynamic(ssl)) {
		return __real_mbedtls_ssl_send_alert_message(ssl, level, message);
	}

	CHECK(add_tx_buffer(ssl, 2));
	int ret = __real_mbedtls_ssl_send_alert_message(ssl, level, message);
	if (!ssl->out_left) {
		CHECK(free_tx_buffer(ssl));
	}
	return ret;
}

int __wrap_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl) {
	if (!dynamic(ssl)) {
		return __real_mbedtls_ssl_close_notify(ssl);
	}

	CHECK(add_tx_buffer(ssl, 2));
	int ret = __real_mbedtls_ssl_close_notify(ssl);
	if (!ssl->out_left) {
		CHECK(free_tx_buffer(ssl));
	}
	return ret;
}

/*
 * EXPERIMENT: multiplication of numbers as small as elliptic curve ones in software. ESP32's RSA hardware
 * multiplies too, but it has to be claimed, loaded and read for each one, which may cost more than the
 * multiplication itself. mpi_soft turns it on and off.
 */
#include "mbedtls/bignum.h"

#define SOFT_MAX_LIMBS 40	// Results up to 1280 bits; RSA's are bigger, and stay with the hardware

int __real_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B);
volatile int mpi_soft = 0;
volatile unsigned mpi_soft_calls = 0, mpi_hw_calls = 0;

_Static_assert(sizeof(mbedtls_mpi_uint) == 4, "32-bit limbs");

static size_t used_limbs(const mbedtls_mpi *A) {
	size_t n = A->n;
	while (n > 0 && A->p[n - 1] == 0) {
		n--;
	}
	return n;
}

int __wrap_mbedtls_mpi_mul_mpi(mbedtls_mpi *X, const mbedtls_mpi *A, const mbedtls_mpi *B) {
	size_t na = used_limbs(A), nb = used_limbs(B);
	if (!mpi_soft || na + nb > SOFT_MAX_LIMBS) {
		mpi_hw_calls++;
		return __real_mbedtls_mpi_mul_mpi(X, A, B);
	}
	mpi_soft_calls++;

	uint32_t t[SOFT_MAX_LIMBS];
	memset(t, 0, (na + nb) * sizeof(uint32_t));
	for (size_t i = 0; i < na; i++) {
		uint64_t carry = 0;
		uint32_t a = A->p[i];
		for (size_t j = 0; j < nb; j++) {
			uint64_t v = (uint64_t)a * B->p[j] + t[i + j] + carry;
			t[i + j] = (uint32_t)v;
			carry = v >> 32;
		}
		t[i + nb] = (uint32_t)carry;
	}

	int sign = (na && nb) ? A->s * B->s : 1;	// A and B are read, so X can be either
	int ret = mbedtls_mpi_grow(X, na + nb);
	if (ret != 0) {
		return ret;
	}
	if (X->n) {
		memset(X->p, 0, X->n * sizeof(mbedtls_mpi_uint));
		memcpy(X->p, t, (na + nb) * sizeof(uint32_t));
	}
	X->s = sign;
	return 0;
}
