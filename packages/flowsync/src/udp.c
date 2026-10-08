// SPDX-License-Identifier: GPL-2.0-only
/* UDP socket, peers, datagram assembly and fan-out */

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>

#include "flowsync.h"

#ifndef IPV6_FREEBIND
#define IPV6_FREEBIND	78	/* linux/in6.h, not in every libc's netinet/in.h */
#endif

int udp_fd = -1;
static struct sockaddr_in6 peer_sa[MAX_PEERS];
/* IPv4 datagrams on the socket carry IP_PKTINFO (see udp_recv) */
static bool v4info;

/* the datagram under assembly; the header is written on flush */
static struct {
	uint8_t buf[MAX_DGRAM];
	unsigned int count;
} dgram;

/* a socket buffer size, beyond the sysctl limit where the daemon may */
static void set_buf(int fd, int opt, int force_opt, const char *optname,
		    const char *what, const char *sysctl, int v)
{
	int eff = 0;
	socklen_t len = sizeof(eff);

	if (setsockopt(fd, SOL_SOCKET, force_opt, &v, sizeof(v)) &&
	    setsockopt(fd, SOL_SOCKET, opt, &v, sizeof(v)))
		logmsg(LOG_WARNING, "udp: %s %d: %s", optname, v, strerror(errno));
	/* the kernel doubles the value for bookkeeping overhead */
	if (!getsockopt(fd, SOL_SOCKET, opt, &eff, &len) && eff / 2 < v)
		logmsg(LOG_WARNING, "udp: %s buffer is %d, wanted %d (raise %s)",
		       what, eff / 2, v, sysctl);
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
	unsigned long long burst = cfg.tx_rate / 20 ? cfg.tx_rate / 20 : 1;
	unsigned long long want = 2 * burst * (cfg.n_peer ? cfg.n_peer : 1) * 2304;

	if (want < 256 * 1024)
		want = 256 * 1024;
	if (want > INT_MAX / 2)
		want = INT_MAX / 2;
	set_buf(fd, SO_SNDBUF, SO_SNDBUFFORCE, "SO_SNDBUF", "send",
		"net.core.wmem_max", want);
}

/* the interface the sync socket is bound to, by index (0: none) */
static unsigned int bound_ifindex;

int udp_open(bool bind_port)
{
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6 };
	char abuf[INET6_ADDRSTRLEN];
	unsigned int ifindex = 0;
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
		 * any local address on any interface). The kernel keeps the
		 * device's index, so udp_tick() reopens the socket when the
		 * device is created anew (netifd does that to a VLAN uplink on
		 * every ifup). */
		if (cfg.ifname[0]) {
			ifindex = if_nametoindex(cfg.ifname);
			if (!ifindex ||
			    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, cfg.ifname,
				       strlen(cfg.ifname))) {
				if (ifindex)
					logmsg(LOG_ERR, "interface %s: %s", cfg.ifname,
					       strerror(errno));
				close(fd);
				errno = ENODEV;
				return -1;
			}
		}
		v4info = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)) == 0;
		if (!v4info)
			logmsg(LOG_WARNING, "udp: IP_PKTINFO: %s (v4-mapped sources over IPv6 "
			       "not recognised)", strerror(errno));
		/* the bind address may not be configured yet (at boot, or while
		 * the device is away): bind anyway */
		setsockopt(fd, IPPROTO_IPV6, IPV6_FREEBIND, &on, sizeof(on));
		set_sndbuf(fd);
		set_buf(fd, SO_RCVBUF, SO_RCVBUFFORCE, "SO_RCVBUF", "receive",
			"net.core.rmem_max", cfg.rcvbuf);
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
	if (bind_port)
		bound_ifindex = ifindex;
	return fd;
}

/*
 * Every tick: with an interface, (re)open the sync socket when the device has
 * come (it may be missing at startup) or has been created anew under the same
 * name. A socket bound to the old index receives nothing and cannot send; the
 * daemon would be deaf and mute until restarted. Returns true when a new
 * socket was opened: we may have missed announcements meanwhile.
 */
bool udp_tick(void)
{
	static uint64_t last_log;
	unsigned int ifindex;
	int fd;

	if (!cfg.ifname[0] && udp_fd >= 0)
		return false;
	ifindex = cfg.ifname[0] ? if_nametoindex(cfg.ifname) : 0;
	if (udp_fd >= 0 && ifindex == bound_ifindex)
		return false;
	if (cfg.ifname[0] && !ifindex) {
		/* gone, or not there yet: keep what we have */
		if (udp_fd < 0 && log_ok(&last_log))
			logmsg(LOG_WARNING, "interface %s does not exist (yet): no sync until it "
			       "does", cfg.ifname);
		return false;
	}
	fd = udp_open(true);
	if (fd < 0)
		return false;
	if (udp_fd >= 0) {
		close(udp_fd);
		logmsg(LOG_NOTICE, "interface %s was created anew: sync socket reopened", cfg.ifname);
	} else if (cfg.ifname[0]) {
		logmsg(LOG_NOTICE, "interface %s exists: sync socket open", cfg.ifname);
	}
	udp_fd = fd;
	return true;
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

/* a send to this peer failed for good (errno) */
static void send_failed(unsigned int i)
{
	static uint64_t last_log;
	char abuf[INET6_ADDRSTRLEN];
	int err = errno;

	cnt.tx_errors++;
	peer_tx_errors[i]++;
	if (log_ok(&last_log))
		logmsg(LOG_WARNING, "send to %s: %s",
		       addr_str(&cfg.peer[i], abuf, sizeof(abuf)), strerror(err));
}

/* a datagram to the given peers; returns those whose send buffer was full
 * (left to the caller), every other failure is counted here */
static uint64_t send_to(const uint8_t *buf, size_t len, uint64_t peers)
{
	uint64_t failed = 0;
	unsigned int i;

	if (udp_fd < 0)
		return 0;	/* no socket while the interface is away: nothing to send on */
	for (i = 0; i < cfg.n_peer; i++) {
		if (!(peers & 1ull << i))
			continue;
		if (sendto(udp_fd, buf, len, MSG_DONTWAIT,
			   (struct sockaddr *)&peer_sa[i], sizeof(peer_sa[i])) >= 0) {
			cnt.tx_datagrams++;
			continue;
		}
		if (full(errno))
			failed |= 1ull << i;
		else
			send_failed(i);
	}
	return failed;
}

static uint64_t all_peers(void)
{
	return (1ull << cfg.n_peer) - 1;	/* n_peer <= MAX_PEERS */
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
	len = wire_seal(dgram.buf, WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN);
	DBG("tx: %u records in %zu bytes to %u peers", dgram.count, len, cfg.n_peer);
	dgram.count = 0;
	failed = send_to(dgram.buf, len, all_peers());
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

/* an announcement datagram that dgram_add() had to send on its own (it was
 * full) was lost for some peer */
static bool add_lost;

/* send the announcements; false if any of them since the last flush was lost
 * for some peer */
bool dgram_flush(void)
{
	bool ok = dgram_send(false) && !add_lost;

	add_lost = false;
	return ok;
}

bool dgram_held(void)
{
	return held.peers != 0;
}

/* the held datagram to the peers still missing it; true when all have it */
bool dgram_resend(void)
{
	if (held.peers)
		held.peers = send_to(held.buf, held.len, held.peers);
	return !held.peers;
}

/* a datagram without records to one peer: heartbeat, or a resync request;
 * true if it was sent */
bool dgram_control_to(int peer, uint8_t flags)
{
	uint8_t hdr[WIRE_HDR_LEN + WIRE_TAG_LEN];
	size_t len;

	if (udp_fd < 0)
		return false;
	wire_put_hdr(hdr, 0, flags);
	len = wire_seal(hdr, WIRE_HDR_LEN);
	if (sendto(udp_fd, hdr, len, MSG_DONTWAIT,
		   (struct sockaddr *)&peer_sa[peer], sizeof(peer_sa[peer])) < 0) {
		send_failed(peer);
		return false;
	}
	cnt.tx_control++;
	return true;
}

/* the same to all peers; returns how many it was sent to */
unsigned int dgram_control(uint8_t flags)
{
	unsigned int i, sent = 0;

	for (i = 0; i < cfg.n_peer; i++)
		sent += dgram_control_to(i, flags);
	return sent;
}

void dgram_add(const struct flow *f)
{
	if (dgram.count >= cfg.batch_lines && !dgram_send(false))
		add_lost = true;
	/* batch_lines <= WIRE_MAX_RECORDS, so this always fits */
	wire_put(dgram.buf + WIRE_HDR_LEN + dgram.count * WIRE_REC_LEN, f);
	dgram.count++;
}
