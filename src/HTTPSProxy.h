#ifndef HTTPSPROXY_H_
#define HTTPSPROXY_H_

#include <Arduino.h>

/*
 * HTTPS on port 443, in front of the web server on port 80. A task takes each TLS connection and passes
 * what's in it to the web server over a connection to the clock's own address, so every page, the web
 * socket and the login work as they do over HTTP. The certificate is self-signed, made by the clock the
 * first time, and kept in NVS.
 *
 * PROTOTYPE, to see whether the clock has the memory for it.
 */
namespace HTTPSProxy {
	void begin(const char *hostname);
	void setMaxSessions(int n);	// EXPERIMENT: 1 to 4
	String stats(bool reset);
	String bench(int soft);	// EXPERIMENT: -2 starts it, -1 gives the result; 0 or 1 sets the multiplication	// JSON: the heap, sampled, and the connections so far
}

#endif /* HTTPSPROXY_H_ */
