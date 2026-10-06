// SPDX-License-Identifier: GPL-2.0-only
/*
 * Datagrams from the peers: every announced flow that passes the policy goes
 * into the remote map, alive for element_timeout from now. An announcement of
 * a flow that is there already sets its time anew; nothing else is kept per
 * flow.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>

#include "flowsync.h"

static void rx_record(const struct flow *f)
{
	if (!wanted(f)) {
		cnt.rx_policy++;
		flow_dbg("rx policy", f);
		return;
	}
	cnt.rx_records++;
	flow_dbg("rx", f);
	dp_remote_add(f);
}

void handle_rx(void)
{
	static uint8_t buf[65536];
	static uint64_t last_log;
	struct sockaddr_in6 from;
	char abuf[INET6_ADDRSTRLEN];
	struct flow f;
	unsigned int count, r;
	uint8_t flags;
	bool forged;
	ssize_t n;
	int i, p, rc;

	/* drain a bounded burst, then write it to the map in one go */
	for (i = 0; i < DRAIN_MAX; i++) {
		n = udp_recv(buf, sizeof(buf), &from, &forged);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "udp receive: %s", strerror(errno));
			break;
		}
		/* a v4-mapped source that came as IPv6 is nobody's (udp_recv) */
		p = from.sin6_family == AF_INET6 && !forged ? peer_index(&from.sin6_addr) : -1;
		if (p < 0) {
			static uint64_t last_bad;

			cnt.rx_datagrams++;
			cnt.rx_bad_peer++;
			/* a peer whose source address is not the one configured
			 * here (bind_address unset on a multi-homed gateway) looks
			 * exactly like this: say who it was */
			if (log_ok(&last_bad))
				logmsg(LOG_NOTICE, "rx: %zd bytes from %s%s, not a peer", n,
				       addr_str(&from.sin6_addr, abuf, sizeof(abuf)),
				       forged ? " (v4-mapped over IPv6)" : "");
			continue;
		}
		rc = wire_check(buf, n, &count, &flags);
		/* liveness from well-formed datagrams only: garbage with a peer's
		 * address must not hide that the peer is gone */
		if (rc == PARSE_OK) {
			uint32_t now = now_s();

			/* silent for more than a heartbeat spacing (and not since our
			 * start, which asked everybody): it lost that heartbeat, and
			 * whatever it announced meanwhile */
			if (peer_last[p] && now - peer_last[p] > cfg.interval * 3 / 2)
				resync_peer_back(p, now - peer_last[p]);
			peer_rx[p]++;
			peer_last[p] = now;
		}
		if (rc == PARSE_OK && (flags & WIRE_F_RESYNC))
			resync_from(p);
		if (rc == PARSE_OK && !count) {
			cnt.rx_control++;	/* heartbeat or resync request */
			continue;
		}
		cnt.rx_datagrams++;
		switch (rc) {
		case PARSE_OK:
			break;
		case PARSE_VERSION:
			cnt.rx_version++;
			DBG("rx: unknown wire version %u from %s", buf[0],
			    addr_str(&from.sin6_addr, abuf, sizeof(abuf)));
			continue;
		default:
			cnt.rx_parse++;
			DBG("rx: malformed datagram of %zd bytes from %s", n,
			    addr_str(&from.sin6_addr, abuf, sizeof(abuf)));
			continue;
		}
		for (r = 0; r < count; r++) {
			if (wire_get(buf + WIRE_HDR_LEN + r * WIRE_REC_LEN, &f) != PARSE_OK) {
				cnt.rx_parse++;
				continue;
			}
			rx_record(&f);
		}
	}
	dp_remote_flush();
}
