// SPDX-License-Identifier: GPL-2.0-only
/* UDP socket, peers, datagram assembly and fan-out */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>

#include "flowsync.h"

int udp_fd = -1;
static struct sockaddr_in6 peer_sa[MAX_PEERS];
/* IPv4 datagrams on the socket carry IP_PKTINFO (see udp_recv) */
static bool v4info;

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

/*
 * The send buffer holds a datagram until the device has sent it. A refresh
 * tick sends a burst of tx_rate/20 datagrams to every peer back to back
 * (about 2.3 KiB of buffer each with overhead), and announcements of new
 * flows come on top: room for twice that, so that the default buffer
 * (about 200 KiB) does not drop datagrams from four peers on.
 */
static void set_sndbuf(int fd)
{
	unsigned long burst = cfg.tx_rate / 20 ? cfg.tx_rate / 20 : 1;
	unsigned long want = 2 * burst * (cfg.n_peer ? cfg.n_peer : 1) * 2304;
	int v, eff = 0;
	socklen_t len = sizeof(eff);

	if (want < 256 * 1024)
		want = 256 * 1024;
	if (want > INT_MAX / 2)
		want = INT_MAX / 2;
	v = want;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &v, sizeof(v)) &&
	    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)))
		logmsg(LOG_WARNING, "udp: SO_SNDBUF %d: %s", v, strerror(errno));
	if (!getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &eff, &len) && eff / 2 < v)
		logmsg(LOG_WARNING, "udp: send buffer is %d, wanted %d (raise "
		       "net.core.wmem_max)", eff / 2, v);
}

int udp_open(bool bind_port)
{
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6 };
	char abuf[INET6_ADDRSTRLEN];
	int fd, off = 0, on = 1;

	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		logmsg(LOG_ERR, "udp socket: %s", strerror(errno));
		return -1;
	}
	/* dual stack: IPv4 addresses are handled as v4-mapped */
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
	if (bind_port) {
		/* peers are recognised by their source address only: accept
		 * datagrams from the uplink alone, not from a mesh host that
		 * sends with a peer's address (the kernel delivers a datagram for
		 * any local address on any interface) */
		if (cfg.ifname[0] &&
		    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, cfg.ifname, strlen(cfg.ifname))) {
			logmsg(LOG_ERR, "interface %s: %s", cfg.ifname, strerror(errno));
			close(fd);
			return -1;
		}
		v4info = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0;
		if (!v4info)
			logmsg(LOG_WARNING, "udp: IP_PKTINFO: %s (v4-mapped sources over IPv6 "
			       "not recognised)", strerror(errno));
		set_sndbuf(fd);
	}
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

/*
 * One datagram, without blocking. *forged is set for an IPv6 datagram whose
 * source is a v4-mapped address: recvfrom shows it exactly like a datagram
 * from that IPv4 address, and the IPv6 stack does not drop such sources.
 * Only IPv4 datagrams carry IP_PKTINFO, which tells them apart.
 */
ssize_t udp_recv(uint8_t *buf, size_t len, struct sockaddr_in6 *from, bool *forged)
{
	char ctl[CMSG_SPACE(sizeof(struct in_pktinfo))] __attribute__((aligned(8)));
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr m = {
		.msg_name = from, .msg_namelen = sizeof(*from),
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl, .msg_controllen = sizeof(ctl),
	};
	struct cmsghdr *c;
	bool v4 = false;
	ssize_t n;

	n = recvmsg(udp_fd, &m, MSG_DONTWAIT);
	if (n < 0)
		return n;
	for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
			v4 = true;
	*forged = v4info && from->sin6_family == AF_INET6 &&
		  IN6_IS_ADDR_V4MAPPED(&from->sin6_addr) && !v4;
	return n;
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

uint64_t peer_tx_errors[MAX_PEERS];

/* a refresh datagram that some peers' send buffer had no room for */
static struct {
	uint8_t buf[MAX_DGRAM];
	size_t len;
	uint64_t peers;
} held;

static bool full(int err)
{
	return err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS;
}

/* a datagram to the given peers; returns those whose send buffer was full
 * when keep is set (left to the caller), every failure counts otherwise */
static uint64_t send_to(const uint8_t *buf, size_t len, uint64_t peers, bool keep)
{
	static uint64_t last_log;
	char abuf[INET6_ADDRSTRLEN];
	uint64_t failed = 0;
	unsigned int i;

	for (i = 0; i < cfg.n_peer; i++) {
		if (!(peers & 1ull << i))
			continue;
		if (sendto(udp_fd, buf, len, MSG_DONTWAIT,
			   (struct sockaddr *)&peer_sa[i], sizeof(peer_sa[i])) >= 0) {
			cnt.tx_datagrams++;
			continue;
		}
		if (keep && full(errno)) {
			failed |= 1ull << i;
			continue;
		}
		cnt.tx_errors++;
		peer_tx_errors[i]++;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "send to %s: %s",
			       addr_str(&cfg.peer[i], abuf, sizeof(abuf)), strerror(errno));
	}
	return failed;
}

static uint64_t all_peers(void)
{
	return cfg.n_peer >= 64 ? ~0ull : (1ull << cfg.n_peer) - 1;
}

/*
 * Send the datagram under assembly to all peers, never blocking. With hold
 * (the refresh), a peer whose send buffer is full gets it later: the datagram
 * is held and dgram_held() is true until dgram_resend() got it out, and the
 * refresh waits meanwhile (a slower round, not a lost refresh). Without hold
 * (announcements of new flows), the datagram is lost for such a peer; returns
 * false then, so that the caller can have a round repair it.
 */
bool dgram_send(bool hold)
{
	uint64_t failed;
	size_t len;

	if (!dgram.count)
		return true;
	wire_put_hdr(dgram.buf, dgram.count, 0);
	len = WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN;
	DBG("tx: %u records in %zu bytes to %u peers", dgram.count, len, cfg.n_peer);
	dgram.count = 0;
	failed = send_to(dgram.buf, len, all_peers(), true);
	if (!failed)
		return true;
	if (hold) {
		memcpy(held.buf, dgram.buf, len);
		held.len = len;
		held.peers = failed;
		return true;
	}
	/* an announcement: lost for those peers */
	for (unsigned int i = 0; i < cfg.n_peer; i++)
		if (failed & 1ull << i) {
			cnt.tx_errors++;
			peer_tx_errors[i]++;
		}
	return false;
}

bool dgram_flush(void)
{
	return dgram_send(false);
}

bool dgram_held(void)
{
	return held.peers != 0;
}

/* the held datagram to the peers still missing it; true when all have it */
bool dgram_resend(void)
{
	if (held.peers)
		held.peers = send_to(held.buf, held.len, held.peers, true);
	return !held.peers;
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
			peer_tx_errors[i]++;
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
		dgram_send(false);
	/* batch_lines <= WIRE_MAX_RECORDS, so this always fits */
	wire_put(dgram.buf + WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN, f);
	dgram.count++;
}
