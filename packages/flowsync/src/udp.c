// SPDX-License-Identifier: GPL-2.0-only
/* UDP socket, peers, datagram assembly and fan-out */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include "flowsync.h"

int udp_fd = -1;
static struct sockaddr_in6 peer_sa[MAX_PEERS];

/* the datagram under assembly; the header is written on flush */
static struct {
	uint8_t buf[MAX_DGRAM];
	unsigned int count;
} dgram;

void set_rcvbuf(int fd, const char *what, unsigned long size)
{
	int v = size, eff = 0;
	socklen_t len = sizeof(eff);

	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &v, sizeof(v)) &&
	    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)))
		logmsg(LOG_WARNING, "%s: SO_RCVBUF %d: %s", what, v, strerror(errno));
	/* the kernel doubles the value for bookkeeping overhead */
	if (!getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &eff, &len) && eff / 2 < v)
		logmsg(LOG_WARNING, "%s: receive buffer is %d, wanted %d "
		       "(raise net.core.rmem_max)", what, eff / 2, v);
}

int udp_open(bool bind_port)
{
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6 };
	char abuf[INET6_ADDRSTRLEN];
	int fd, off = 0;

	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		logmsg(LOG_ERR, "udp socket: %s", strerror(errno));
		return -1;
	}
	/* dual stack: IPv4 addresses are handled as v4-mapped */
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
	sa.sin6_addr = cfg.bind_set ? cfg.bind : in6addr_any;
	sa.sin6_port = bind_port ? htons(cfg.port) : 0;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		logmsg(LOG_ERR, "bind %s port %u: %s",
		       addr_str(&sa.sin6_addr, abuf, sizeof(abuf)),
		       ntohs(sa.sin6_port), strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

void peers_init(void)
{
	unsigned int i;

	for (i = 0; i < cfg.n_peer; i++) {
		memset(&peer_sa[i], 0, sizeof(peer_sa[i]));
		peer_sa[i].sin6_family = AF_INET6;
		peer_sa[i].sin6_addr = cfg.peer[i];
		peer_sa[i].sin6_port = htons(cfg.port);
	}
}

uint64_t peer_rx[MAX_PEERS];
uint32_t peer_last[MAX_PEERS];

/* index of a configured peer, or -1 */
int peer_index(const struct in6_addr *a)
{
	unsigned int i;

	for (i = 0; i < cfg.n_peer; i++)
		if (IN6_ARE_ADDR_EQUAL(a, &cfg.peer[i]))
			return i;
	return -1;
}

/* send the datagram under assembly to all peers; never blocks */
void dgram_flush(void)
{
	static uint64_t last_log;
	char abuf[INET6_ADDRSTRLEN];
	unsigned int i;
	size_t len;

	if (!dgram.count)
		return;
	wire_put_hdr(dgram.buf, dgram.count, 0);
	len = WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN;
	DBG("tx: %u records in %zu bytes to %u peers", dgram.count, len, cfg.n_peer);
	for (i = 0; i < cfg.n_peer; i++) {
		if (sendto(udp_fd, dgram.buf, len, MSG_DONTWAIT,
			   (struct sockaddr *)&peer_sa[i], sizeof(peer_sa[i])) < 0) {
			cnt.tx_errors++;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "send to %s: %s",
				       addr_str(&cfg.peer[i], abuf, sizeof(abuf)),
				       strerror(errno));
		} else {
			cnt.tx_datagrams++;
		}
	}
	dgram.count = 0;
}

/* a datagram without records to all peers: heartbeat, or a resync request */
void dgram_control(uint8_t flags)
{
	static uint64_t last_log;
	char abuf[INET6_ADDRSTRLEN];
	uint8_t hdr[WIRE_HDR_LEN];
	unsigned int i;

	wire_put_hdr(hdr, 0, flags);
	for (i = 0; i < cfg.n_peer; i++) {
		if (sendto(udp_fd, hdr, sizeof(hdr), MSG_DONTWAIT,
			   (struct sockaddr *)&peer_sa[i], sizeof(peer_sa[i])) < 0) {
			cnt.tx_errors++;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "send to %s: %s",
				       addr_str(&cfg.peer[i], abuf, sizeof(abuf)), strerror(errno));
		} else {
			cnt.tx_control++;
		}
	}
}

void dgram_add(const struct flow *f)
{
	if (dgram.count >= cfg.batch_lines)
		dgram_flush();
	/* batch_lines <= WIRE_MAX_RECORDS, so this always fits */
	wire_put(dgram.buf + WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN, f);
	dgram.count++;
}
