#include "HTTPSProxy.h"

#include <new>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/sha256.h>

extern "C" int tls_handshake_resumed(const mbedtls_ssl_context *ssl);	// TLSDynamicBuffers.c
extern "C" volatile int mpi_soft;
extern "C" volatile unsigned mpi_soft_calls, mpi_hw_calls;


namespace {
	const int PORT = 443;
	const int BACKEND_PORT = 80;
	const int MAX_SESSIONS = 4;
	const size_t CHUNK = 1400;					// Plaintext per record: a record fits in one TCP segment
	const uint32_t HANDSHAKE_TIMEOUT_MS = 10000;
	const size_t MIN_LARGEST_BLOCK = 12000;		// A new connection waits until there's this much in one block

	struct Session {
		int tls = -1;
		int back = -1;
		enum { HANDSHAKE, CONNECTING, OPEN } state = HANDSHAKE;
		bool wantWrite = false;
		bool resumed = false;
		bool backClosed = false;
		uint32_t start = 0;
		int64_t cpuUs = 0;
		size_t toClient = 0;		// What mbedtls_ssl_write() is to be called with again after WANT_WRITE
		uint8_t *toBack = nullptr;	// What the web server hasn't taken yet
		size_t toBackLen = 0;
		size_t toBackOff = 0;
		mbedtls_ssl_context ssl;
	};

	Session *sessions[MAX_SESSIONS] = {};
	uint8_t scratch[CHUNK];
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_ticket_context ticket;
	int listener = -1;
	uint32_t deferUntil = 0;
	int maxSessions = MAX_SESSIONS;	// EXPERIMENT: at most MAX_SESSIONS
	TaskHandle_t proxyTask = NULL;
	TaskHandle_t samplerTask = NULL;
	String host;

	struct Stats {
		uint32_t accepted, deferred, full, resumed, failed, closed;
		int lastError;
		int lastCloseError;	// EXPERIMENT: why the last session that failed did
		uint32_t closeErrors;
		uint32_t fullMaxUs, fullSumUs, resumedMaxUs, resumedSumUs;
		uint32_t certMs, certLen;
		uint32_t bytesIn, bytesOut;
		int maxOpen;
	} st = {};

	// The heap, sampled, for comparing HTTP with HTTPS
	volatile size_t minFree = SIZE_MAX, minLargest = SIZE_MAX;
	volatile uint32_t failedAllocs = 0, failedMax = 0;

	void failedAlloc(size_t size, uint32_t caps, const char *function) {
		failedAllocs++;
		if (size > failedMax) {
			failedMax = size;
		}
	}

	void samplerFn(void *) {
		for (;;) {
			size_t f = heap_caps_get_free_size(MALLOC_CAP_8BIT);
			size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
			if (f < minFree) minFree = f;
			if (l < minLargest) minLargest = l;
			vTaskDelay(2);
		}
	}

	int rng(void *, unsigned char *out, size_t len) {
		esp_fill_random(out, len);	// The hardware's, random while Wi-Fi is on
		return 0;
	}

	int bioSend(void *ctx, const unsigned char *buf, size_t len) {
		int n = send((int)(intptr_t)ctx, buf, len, 0);
		if (n >= 0) {
			return n;
		}
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
	}

	int bioRecv(void *ctx, unsigned char *buf, size_t len) {
		int n = recv((int)(intptr_t)ctx, buf, len, 0);
		if (n >= 0) {
			return n;	// 0 when it's closed
		}
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
	}

	size_t derEntry(uint8_t *p, uint8_t tag, const void *data, size_t len) {
		p[0] = tag;
		p[1] = len;		// Under 128
		memcpy(p + 2, data, len);
		return len + 2;
	}

	bool makeCertificate(Preferences &prefs) {
		uint32_t start = millis();
		int ret;
		if ((ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
			(ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, NULL)) != 0) {
			st.lastError = ret;
			return false;
		}

		const size_t size = 1024;
		uint8_t *der = (uint8_t *)malloc(size);
		if (!der) {
			return false;
		}
		int len = mbedtls_pk_write_key_der(&key, der, size);		// At the end of der
		if (len < 0) {
			st.lastError = len;
			free(der);
			return false;
		}
		prefs.putBytes("key", der + size - len, len);

		mbedtls_x509write_cert crt;
		mbedtls_x509write_crt_init(&crt);
		mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
		mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
		mbedtls_x509write_crt_set_subject_key(&crt, &key);
		mbedtls_x509write_crt_set_issuer_key(&crt, &key);
		String name = "CN=" + host;		// Only letters, digits and hyphens, see validHostname()
		mbedtls_x509write_crt_set_subject_name(&crt, name.c_str());
		mbedtls_x509write_crt_set_issuer_name(&crt, name.c_str());

		uint8_t serialBytes[16];
		esp_fill_random(serialBytes, sizeof(serialBytes));
		serialBytes[0] = (serialBytes[0] & 0x3f) | 0x40;	// Positive, and 16 bytes long
		mbedtls_mpi serial;
		mbedtls_mpi_init(&serial);
		mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes));
		mbedtls_x509write_crt_set_serial(&crt, &serial);

		// 825 days, the most Apple's devices take
		mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20280405000000");
		mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
		mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
		static const uint8_t serverAuth[] = { 0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
		mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_EXTENDED_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, serverAuth, sizeof(serverAuth));

		// The names it's for: the hostname, with and without .local, and the clock's addresses
		uint8_t names[2 + 63 + 2 + 69 + 6 + 6];
		size_t n = 0;
		n += derEntry(names + n, 0x82, host.c_str(), host.length());
		String local = host + ".local";
		n += derEntry(names + n, 0x82, local.c_str(), local.length());
		uint32_t ip = WiFi.localIP();
		if (ip) {
			n += derEntry(names + n, 0x87, &ip, 4);
		}
		uint32_t apIP = IPAddress(192, 168, 4, 1);
		n += derEntry(names + n, 0x87, &apIP, 4);
		uint8_t san[3 + sizeof(names)];
		size_t s = 0;
		san[s++] = 0x30;
		if (n >= 128) {
			san[s++] = 0x81;
		}
		san[s++] = n;
		memcpy(san + s, names, n);
		mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, s + n);

		len = mbedtls_x509write_crt_der(&crt, der, size, rng, NULL);
		mbedtls_x509write_crt_free(&crt);
		mbedtls_mpi_free(&serial);
		if (len < 0) {
			st.lastError = len;
			free(der);
			return false;
		}
		prefs.putBytes("crt", der + size - len, len);
		ret = mbedtls_x509_crt_parse_der(&cert, der + size - len, len);
		free(der);
		st.certMs = millis() - start;
		st.certLen = len;
		return ret == 0;
	}

	bool loadCertificate(Preferences &prefs) {
		size_t keyLen = prefs.getBytesLength("key");
		size_t crtLen = prefs.getBytesLength("crt");
		if (!keyLen || !crtLen) {
			return false;
		}
		uint8_t *buf = (uint8_t *)malloc(keyLen + crtLen);
		if (!buf) {
			return false;
		}
		prefs.getBytes("key", buf, keyLen);
		prefs.getBytes("crt", buf + keyLen, crtLen);
		int ret = mbedtls_pk_parse_key(&key, buf, keyLen, NULL, 0);
		if (ret == 0) {
			ret = mbedtls_x509_crt_parse_der(&cert, buf + keyLen, crtLen);
		}
		free(buf);
		st.certLen = crtLen;
		return ret == 0;
	}

	bool setupConfig() {
		static const int suites[] = {
			MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
			MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
			0
		};
		static const mbedtls_ecp_group_id curves[] = { MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_NONE };
		static const char *alpn[] = { "http/1.1", NULL };

		mbedtls_ssl_config_init(&conf);
		int ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret != 0) {
			st.lastError = ret;
			return false;
		}
		mbedtls_ssl_conf_rng(&conf, rng, NULL);
		mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
		mbedtls_ssl_conf_ciphersuites(&conf, suites);
		mbedtls_ssl_conf_curves(&conf, curves);
		mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
		if ((ret = mbedtls_ssl_conf_own_cert(&conf, &cert, &key)) != 0) {
			st.lastError = ret;
			return false;
		}

		// The web server closes each connection after one request, so browsers come back often
		mbedtls_ssl_ticket_init(&ticket);
		if ((ret = mbedtls_ssl_ticket_setup(&ticket, rng, NULL, MBEDTLS_CIPHER_AES_128_GCM, 24 * 3600)) != 0) {
			st.lastError = ret;
			return false;
		}
		mbedtls_ssl_conf_session_tickets_cb(&conf, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse, &ticket);
		return true;
	}

	void setNonBlocking(int fd) {
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	}

	void setNoDelay(int fd) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}

	void closeSession(int i) {
		Session *s = sessions[i];
		tls_handshake_resumed(&s->ssl);		// Forgotten, if it didn't finish
		mbedtls_ssl_free(&s->ssl);
		if (s->tls >= 0) {
			close(s->tls);
		}
		if (s->back >= 0) {
			close(s->back);
		}
		free(s->toBack);
		delete s;
		sessions[i] = nullptr;
		st.closed++;
	}

	void acceptOne() {
		int slot = -1;
		for (int i = 0; i < MAX_SESSIONS; i++) {
			if (!sessions[i]) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			return;
		}
		if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_LARGEST_BLOCK) {
			st.deferred++;
			deferUntil = millis() + 100;
			return;
		}

		int fd = accept(listener, NULL, NULL);
		if (fd < 0) {
			return;
		}
		setNonBlocking(fd);
		setNoDelay(fd);
		int on = 1, idle = 60, interval = 10, count = 3;	// A phone that's gone away
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
		setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

		Session *s = new (std::nothrow) Session();
		if (!s) {
			close(fd);
			return;
		}
		s->tls = fd;
		s->start = millis();
		mbedtls_ssl_init(&s->ssl);
		int ret = mbedtls_ssl_setup(&s->ssl, &conf);
		if (ret != 0) {
			st.lastError = ret;
			st.failed++;
			mbedtls_ssl_free(&s->ssl);
			close(fd);
			delete s;
			return;
		}
		mbedtls_ssl_set_bio(&s->ssl, (void *)(intptr_t)fd, bioSend, bioRecv, NULL);
		sessions[slot] = s;
		st.accepted++;
	}

	// To the web server, at the address the browser used, so it sees which network the browser is on
	bool connectBackend(Session *s) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		if (getsockname(s->tls, (struct sockaddr *)&addr, &len) != 0) {
			return false;
		}
		addr.sin_port = htons(BACKEND_PORT);
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) {
			return false;
		}
		setNonBlocking(fd);
		setNoDelay(fd);
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 && errno != EINPROGRESS) {
			close(fd);
			return false;
		}
		s->back = fd;
		s->state = Session::CONNECTING;
		return true;
	}

	bool closeError(int e) {
		st.lastCloseError = e;
		st.closeErrors++;
		return false;
	}

	// False when the session is over
	bool service(Session *s, fd_set &r, fd_set &w) {
		switch (s->state) {
		case Session::HANDSHAKE: {
			if (!FD_ISSET(s->tls, &r) && !FD_ISSET(s->tls, &w)) {
				return millis() - s->start < HANDSHAKE_TIMEOUT_MS;
			}
			int64_t t0 = esp_timer_get_time();
			int ret = mbedtls_ssl_handshake(&s->ssl);
			int64_t took = esp_timer_get_time() - t0;
			s->cpuUs += took;
			if (took > 20000) {
				vTaskDelay(1);	// So core 0's idle task runs between handshakes, or the task watchdog restarts the clock
			}
			if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
				s->wantWrite = ret == MBEDTLS_ERR_SSL_WANT_WRITE;
				return true;
			}
			if (ret != 0) {
				st.failed++;
				st.lastError = ret;
				return false;
			}
			uint32_t us = s->cpuUs;
			s->resumed = tls_handshake_resumed(&s->ssl);
			if (s->resumed) {
				st.resumed++;
				st.resumedSumUs += us;
				st.resumedMaxUs = max(st.resumedMaxUs, us);
			} else {
				st.full++;
				st.fullSumUs += us;
				st.fullMaxUs = max(st.fullMaxUs, us);
			}
			return connectBackend(s);
		}

		case Session::CONNECTING: {
			if (!FD_ISSET(s->back, &w)) {
				return true;
			}
			int err = 0;
			socklen_t len = sizeof(err);
			getsockopt(s->back, SOL_SOCKET, SO_ERROR, &err, &len);
			if (err) {
				st.lastError = -err;
				return false;
			}
			s->state = Session::OPEN;
			return true;
		}

		case Session::OPEN:
			// From the browser to the web server
			if (s->toBackLen == 0 && (FD_ISSET(s->tls, &r) || mbedtls_ssl_get_bytes_avail(&s->ssl) > 0)) {
				int n = mbedtls_ssl_read(&s->ssl, scratch, CHUNK);
				if (n > 0) {
					st.bytesIn += n;
					int m = send(s->back, scratch, n, 0);
					if (m < 0) {
						if (errno != EAGAIN && errno != EWOULDBLOCK) {
							return closeError(-1000 - errno);
						}
						m = 0;
					}
					if (m < n) {
						s->toBack = (uint8_t *)malloc(n - m);
						if (!s->toBack) {
							return false;
						}
						memcpy(s->toBack, scratch + m, n - m);
						s->toBackLen = n - m;
						s->toBackOff = 0;
					}
				} else if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
					return n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? false : closeError(n);	// The browser closed it, or it failed
				}
			} else if (s->toBackLen && FD_ISSET(s->back, &w)) {
				int m = send(s->back, s->toBack + s->toBackOff, s->toBackLen, 0);
				if (m < 0) {
					if (errno != EAGAIN && errno != EWOULDBLOCK) {
						return false;
					}
				} else {
					s->toBackOff += m;
					s->toBackLen -= m;
					if (!s->toBackLen) {
						free(s->toBack);
						s->toBack = nullptr;
					}
				}
			}

			// From the web server to the browser
			if (s->toClient) {
				if (FD_ISSET(s->tls, &w)) {
					int m = mbedtls_ssl_write(&s->ssl, scratch, s->toClient);	// Already in mbedTLS's buffer
					if (m > 0) {
						s->toClient = 0;
					} else if (m != MBEDTLS_ERR_SSL_WANT_WRITE && m != MBEDTLS_ERR_SSL_WANT_READ) {
						return closeError(m);
					}
				}
			} else if (!s->backClosed && FD_ISSET(s->back, &r)) {
				int n = recv(s->back, scratch, CHUNK, 0);
				if (n > 0) {
					st.bytesOut += n;
					int m = mbedtls_ssl_write(&s->ssl, scratch, n);	// All of it, as it's less than a record
					if (m == MBEDTLS_ERR_SSL_WANT_WRITE || m == MBEDTLS_ERR_SSL_WANT_READ) {
						s->toClient = n;
					} else if (m < 0) {
						return closeError(m);
					}
				} else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
					s->backClosed = true;
				}
			}

			if (s->backClosed && !s->toClient) {
				mbedtls_ssl_close_notify(&s->ssl);
				return false;
			}
			return true;
		}
		return false;
	}

	void proxyFn(void *) {
		// An address for the certificate, and Wi-Fi on for random numbers
		while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && !(WiFi.getMode() & WIFI_AP)) {
			delay(500);
		}

		Preferences prefs;
		prefs.begin("https", false);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_init(&key);
		if (!loadCertificate(prefs)) {
			mbedtls_x509_crt_free(&cert);
			mbedtls_x509_crt_init(&cert);
			mbedtls_pk_free(&key);
			mbedtls_pk_init(&key);
			if (!makeCertificate(prefs)) {
				prefs.end();
				proxyTask = NULL;
				vTaskDelete(NULL);
			}
		}
		prefs.end();

		if (!setupConfig()) {
			proxyTask = NULL;
			vTaskDelete(NULL);
		}

		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		int on = 1;
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(PORT);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, MAX_SESSIONS) != 0) {
			st.lastError = -errno;
			close(listener);
			proxyTask = NULL;
			vTaskDelete(NULL);
		}
		setNonBlocking(listener);

		for (;;) {
			fd_set r, w;
			FD_ZERO(&r);
			FD_ZERO(&w);
			int maxfd = -1;
			bool pending = false;
			int open = 0;
			auto watch = [&](int fd, fd_set &set) {
				FD_SET(fd, &set);
				if (fd > maxfd) {
					maxfd = fd;
				}
			};

			for (Session *s : sessions) {
				if (!s) {
					continue;
				}
				open++;
				switch (s->state) {
				case Session::HANDSHAKE:
					watch(s->tls, s->wantWrite ? w : r);
					break;
				case Session::CONNECTING:
					watch(s->back, w);
					break;
				case Session::OPEN:
					if (s->toBackLen) {
						watch(s->back, w);
					} else if (mbedtls_ssl_get_bytes_avail(&s->ssl) > 0) {
						pending = true;		// Already read and decrypted
					} else {
						watch(s->tls, r);
					}
					if (s->toClient) {
						watch(s->tls, w);
					} else if (!s->backClosed) {
						watch(s->back, r);
					}
					break;
				}
			}
			if (open < maxSessions && (int32_t)(millis() - deferUntil) >= 0) {
				watch(listener, r);
			}
			if (open > st.maxOpen) {
				st.maxOpen = open;
			}

			struct timeval tv = { 0, pending ? 0 : 200000 };
			if (select(maxfd + 1, &r, &w, NULL, &tv) < 0) {
				delay(10);
				continue;
			}
			if (FD_ISSET(listener, &r)) {
				acceptOne();
			}
			for (int i = 0; i < MAX_SESSIONS; i++) {
				if (sessions[i] && !service(sessions[i], r, w)) {
					closeSession(i);
				}
			}
		}
	}
}

void HTTPSProxy::begin(const char *hostname) {
	if (proxyTask) {
		return;
	}
	host = hostname;
	heap_caps_register_failed_alloc_callback(failedAlloc);
	xTaskCreatePinnedToCore(samplerFn, "heap sampler", 2048, NULL, tskIDLE_PRIORITY + 1, &samplerTask, 0);
	xTaskCreatePinnedToCore(proxyFn, "https", 8192, NULL, tskIDLE_PRIORITY + 1, &proxyTask, 0);
}

void HTTPSProxy::setMaxSessions(int n) {
	maxSessions = constrain(n, 1, MAX_SESSIONS);
}

String HTTPSProxy::stats(bool reset) {
	int open = 0;
	for (Session *s : sessions) {
		if (s) {
			open++;
		}
	}
	char buf[640];
	snprintf(buf, sizeof(buf),
		"{\"free\":%u,\"largest\":%u,\"minFree\":%u,\"minLargest\":%u,\"failedAllocs\":%u,\"failedMax\":%u,"
		"\"open\":%d,\"maxOpen\":%d,\"accepted\":%u,\"deferred\":%u,\"full\":%u,\"resumed\":%u,\"failed\":%u,\"closed\":%u,\"lastError\":%d,"
		"\"fullAvgMs\":%.1f,\"fullMaxMs\":%.1f,\"resumedAvgMs\":%.1f,\"resumedMaxMs\":%.1f,\"certMs\":%u,\"certLen\":%u,"
		"\"bytesIn\":%u,\"bytesOut\":%u,\"stackFree\":%u,\"maxSessions\":%d,\"mpiSoft\":%d,\"closeErrors\":%u,\"lastCloseError\":%d}",
		heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), minFree, minLargest, failedAllocs, failedMax,
		open, st.maxOpen, st.accepted, st.deferred, st.full, st.resumed, st.failed, st.closed, st.lastError,
		st.full ? st.fullSumUs / 1000.0 / st.full : 0.0, st.fullMaxUs / 1000.0, st.resumed ? st.resumedSumUs / 1000.0 / st.resumed : 0.0, st.resumedMaxUs / 1000.0,
		st.certMs, st.certLen, st.bytesIn, st.bytesOut, proxyTask ? uxTaskGetStackHighWaterMark(proxyTask) : 0, maxSessions, mpi_soft, st.closeErrors, st.lastCloseError);
	if (reset) {
		minFree = SIZE_MAX;
		minLargest = SIZE_MAX;
		failedAllocs = 0;
		failedMax = 0;
		st.maxOpen = 0;
	}
	return String(buf);
}

// EXPERIMENT: how long the handshake's elliptic curve operations take, with the hardware's multiplication and without
namespace {
	String benchResult = "not run";
	volatile bool benchRunning = false;

	uint32_t timeEcdh(mbedtls_ecp_group_id id) {
		mbedtls_ecdh_context a, b;
		mbedtls_ecdh_init(&a);
		mbedtls_ecdh_init(&b);
		mbedtls_ecdh_setup(&a, id);
		mbedtls_ecdh_setup(&b, id);
		unsigned char pa[100], pb[100], secret[66];
		size_t la, lb, ls;
		mbedtls_ecdh_make_public(&b, &lb, pb, sizeof(pb), rng, NULL);
		vTaskDelay(1);
		int64_t t0 = esp_timer_get_time();
		// What the server does: make its key pair, and the secret from the browser's public key
		mbedtls_ecdh_make_public(&a, &la, pa, sizeof(pa), rng, NULL);
		mbedtls_ecdh_read_public(&a, pb, lb);
		int ret = mbedtls_ecdh_calc_secret(&a, &ls, secret, sizeof(secret), rng, NULL);
		uint32_t us = esp_timer_get_time() - t0;
		mbedtls_ecdh_free(&a);
		mbedtls_ecdh_free(&b);
		return ret == 0 ? us : 0;
	}

	void benchFn(void *) {
		String r = "{";
		for (int soft = 0; soft < 2; soft++) {
			mpi_soft = soft;
			unsigned hw0 = mpi_hw_calls, sw0 = mpi_soft_calls;

			mbedtls_mpi a, b, c, d;
			mbedtls_mpi_init(&a); mbedtls_mpi_init(&b); mbedtls_mpi_init(&c); mbedtls_mpi_init(&d);
			mbedtls_mpi_fill_random(&a, 32, rng, NULL);
			mbedtls_mpi_fill_random(&b, 32, rng, NULL);
			int64_t t0 = esp_timer_get_time();
			for (int i = 0; i < 1000; i++) {
				mbedtls_mpi_mul_mpi(&c, &a, &b);
			}
			uint32_t mulNs = (esp_timer_get_time() - t0);	// us per 1000 = ns each
			vTaskDelay(1);

			// Both ways must agree, on random sizes and signs
			int mismatches = 0;
			for (int i = 0; i < 200; i++) {
				mbedtls_mpi_fill_random(&a, 1 + (esp_random() % 80), rng, NULL);
				mbedtls_mpi_fill_random(&b, 1 + (esp_random() % 80), rng, NULL);
				if (esp_random() & 1) a.s = -1;
				if (esp_random() & 1) b.s = -1;
				if (i % 50 == 0) mbedtls_mpi_lset(&b, 0);
				mpi_soft = 1; mbedtls_mpi_mul_mpi(&c, &a, &b);
				mpi_soft = 0; mbedtls_mpi_mul_mpi(&d, &a, &b);
				mpi_soft = soft;
				if (mbedtls_mpi_cmp_mpi(&c, &d) != 0) mismatches++;
				if (i % 20 == 0) vTaskDelay(1);
			}
			mbedtls_mpi_free(&a); mbedtls_mpi_free(&b); mbedtls_mpi_free(&c); mbedtls_mpi_free(&d);

			unsigned char hash[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
			size_t slen;
			memset(hash, 0x5a, sizeof(hash));
			t0 = esp_timer_get_time();
			int sret = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, &slen, rng, NULL);
			uint32_t signUs = esp_timer_get_time() - t0;
			vTaskDelay(1);
			int vret = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, slen);
			vTaskDelay(1);

			uint32_t x25519 = timeEcdh(MBEDTLS_ECP_DP_CURVE25519);
			vTaskDelay(1);
			uint32_t p256 = timeEcdh(MBEDTLS_ECP_DP_SECP256R1);
			vTaskDelay(1);

			char buf[300];
			snprintf(buf, sizeof(buf), "%s\"%s\":{\"mul256ns\":%u,\"mismatches\":%d,\"signMs\":%.1f,\"signOk\":%d,\"x25519Ms\":%.1f,\"p256EcdhMs\":%.1f,\"hwCalls\":%u,\"softCalls\":%u}",
				soft ? "," : "", soft ? "software" : "hardware", mulNs, mismatches, signUs / 1000.0, sret == 0 && vret == 0,
				x25519 / 1000.0, p256 / 1000.0, mpi_hw_calls - hw0, mpi_soft_calls - sw0);
			r += buf;
		}
		mpi_soft = 0;
		benchResult = r + "}";
		benchRunning = false;
		vTaskDelete(NULL);
	}
}

String HTTPSProxy::bench(int soft) {
	if (soft >= 0) {
		mpi_soft = soft;
		return String("{\"mpi_soft\":") + soft + "}";
	}
	if (soft == -2 && !benchRunning && proxyTask) {
		benchRunning = true;
		benchResult = "running";
		xTaskCreatePinnedToCore(benchFn, "bench", 8192, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
		return "started";
	}
	return benchResult;
}
