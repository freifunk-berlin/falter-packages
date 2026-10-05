// SPDX-License-Identifier: GPL-2.0-only
/* main loop and subcommands */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "flowsync.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/*
 * A copy is announced only while its remaining timeout exceeds
 * element_timeout, which a packet causes by setting the protocol's natural
 * timeout. If that natural timeout is not larger than element_timeout, copies
 * on this gateway can never show traffic and stay silent (still correct, but
 * a rerouted flow is then not re-announced from here).
 */
static void check_kernel_timeouts(void)
{
	static const struct {
		int proto;
		const char *path;
	} t[] = {
		{ IPPROTO_UDP, "/proc/sys/net/netfilter/nf_conntrack_udp_timeout_stream" },
		{ IPPROTO_TCP, "/proc/sys/net/netfilter/nf_conntrack_tcp_timeout_unacknowledged" },
	};
	unsigned long v;
	unsigned int i;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (!cfg.proto[t[i].proto] || read_sysctl(t[i].path, &v))
			continue;
		if (v <= cfg.element_timeout)
			logmsg(LOG_WARNING, "%s is %lu, not above element_timeout %lu: %s copies can "
			       "never show traffic and will not be announced from here",
			       strrchr(t[i].path, '/') + 1, v, cfg.element_timeout,
			       proto_name(t[i].proto));
	}
	/* 0: no event extension on new entries, so no NEW and no DESTROY events
	 * at all, while subscribing still succeeds */
	if (!read_sysctl("/proc/sys/net/netfilter/nf_conntrack_events", &v) && !v)
		logmsg(LOG_ERR, "nf_conntrack_events is 0: the kernel sends no conntrack events, "
		       "new flows are announced only by the next round and lost copies go "
		       "unnoticed (set it to 1 or 2)");
}

static int cmd_run(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct pollfd fds[5];
	uint64_t now, next_tick, busy_since = 0, busy, loop_max = 0, last_early = 0;
	char abuf[INET6_ADDRSTRLEN];
	bool first = true, owed = false;
	int timeout, nfds;

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (rx_init(RX_TABLE_SIZE))
		return 1;
	rx_limit_init();
	peers_init();
	udp_fd = udp_open(true);
	if (udp_fd < 0 || inj_open() || ev_open() || destroy_open())
		return 1;
	resync_init();
	set_rcvbuf(udp_fd, "udp", cfg.rcvbuf);
	check_kernel_timeouts();
	if (!cfg.ifname[0])
		logmsg(LOG_WARNING, "no interface set: datagrams with a peer's source address are "
		       "accepted from every interface, the mesh side too (set it to the uplink)");

	logmsg(LOG_NOTICE, "started on %s port %lu: %u peers, %u prefixes, interval %lus, "
	       "element_timeout %lus", cfg.bind_set ? addr_str(&cfg.bind, abuf, sizeof(abuf)) : "*",
	       cfg.port, cfg.n_peer, cfg.prefix.n, cfg.interval, cfg.element_timeout);

	next_tick = mono_ms();
	while (!stop) {
		now = mono_ms();
		if (now >= next_tick) {
			gauge.loop_max_ms = loop_max;
			loop_max = 0;
			if (!first)
				log_counters();
			first = false;
			if (events_fd() < 0)
				ev_open();
			if (destroy_fd() < 0)
				destroy_open();
			switch (refresh_start()) {
			case 1:
				owed = false;
				resync_round_started();	/* it serves the peers' requests */
				break;
			case 0:
				/* the previous round is still being sent: the next
				 * one is owed and starts once its queue is empty */
				owed = true;
				break;
			default:
				break;		/* failed: retried at the next tick */
			}
			heartbeat();
			write_status();
			next_tick = now + cfg.interval * 1000;
		}
		/* NEW events were lost: the flows they announced wait for the next
		 * round. Pull it forward (a second from now, once the burst that
		 * caused the overrun has passed), at most once per interval so that a
		 * sustained overload at most doubles the round rate. */
		if (events_lost() && !gauge.refresh_running && !refresh_pending() &&
		    now - last_early >= cfg.interval * 1000 && next_tick > now + EARLY_ROUND_MS) {
			next_tick = now + EARLY_ROUND_MS;
			last_early = now;
		}
		/* a round owed since an overrun tick: as soon as the previous one
		 * is sent, not a whole interval later (which would double the
		 * period between refreshes), and the interval counts from here */
		if (owed && !gauge.refresh_running && !refresh_pending())
			next_tick = now;
		/* a peer asked for a round (it restarted or lost copies) */
		if (resync_round_wanted() && !gauge.refresh_running && !refresh_pending()) {
			next_tick = now;
			resync_round_pulled();
		}
		resync_tick();
		/* we asked for a resync: refresh our copies from the answers */
		if (resync_own_round_wanted() && !gauge.refresh_running && !refresh_pending() &&
		    next_tick > now + EARLY_ROUND_MS)
			next_tick = now + EARLY_ROUND_MS;
		if (refresh_pending())
			refresh_tick();

		now = mono_ms();
		timeout = next_tick > now ? (int)(next_tick - now) : 0;
		if (refresh_pending() && refresh_pace_ms() < timeout)
			timeout = refresh_pace_ms();

		nfds = 0;
		fds[nfds].fd = events_fd();
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		fds[nfds].fd = udp_fd;
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		fds[nfds].fd = inject_fd();
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		fds[nfds].fd = destroy_fd();
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		if (refresh_wants_read()) {
			fds[nfds].fd = refresh_fd();
			fds[nfds].events = POLLIN;
			fds[nfds++].revents = 0;
		}

		/* handler time from the last poll return to this poll call */
		if (busy_since) {
			busy = now - busy_since;
			if (busy > loop_max)
				loop_max = busy;
		}
		if (poll(fds, nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			logmsg(LOG_ERR, "poll: %s", strerror(errno));
			return 1;
		}
		busy_since = mono_ms();
		if (fds[0].revents)
			handle_events();
		if (fds[1].revents)
			handle_rx();
		if (fds[2].revents)
			inj_drain();
		if (fds[3].revents)
			handle_destroy();
		if (nfds > 4 && fds[4].revents)
			handle_refresh();
	}

	refresh_close();
	log_counters();
	logmsg(LOG_NOTICE, "stopped");
	unlink(status_path);
	return 0;
}

static void print_list(const char *name, const struct prefix_list *l)
{
	char abuf[INET6_ADDRSTRLEN];
	unsigned int i;

	for (i = 0; i < l->n; i++)
		printf("%s %s/%u\n", name,
		       inet_ntop(AF_INET6, &l->p[i].addr, abuf, sizeof(abuf)), l->p[i].len);
}

static int cmd_check(void)
{
	char abuf[INET6_ADDRSTRLEN];
	unsigned int i;

	printf("debug %d\n", cfg.debug);
	printf("bind_address %s\n", cfg.bind_set ? addr_str(&cfg.bind, abuf, sizeof(abuf)) : "*");
	printf("interface %s\n", cfg.ifname[0] ? cfg.ifname : "*");
	printf("port %lu\n", cfg.port);
	printf("interval %lu\n", cfg.interval);
	printf("element_timeout %lu\n", cfg.element_timeout);
	printf("batch_lines %lu\n", cfg.batch_lines);
	printf("tx_rate %lu\n", cfg.tx_rate);
	printf("rcvbuf %lu\n", cfg.rcvbuf);
	printf("ct_mark 0x%08lx\n", cfg.ct_mark);
	printf("ct_mark_mask 0x%08lx\n", cfg.ct_mark_mask);
	printf("max_copies %lu\n", cfg.max_copies);
	printf("max_copies_per_client %lu\n", cfg.max_copies_client);
	for (i = 0; i < 256; i++)
		if (cfg.proto[i] && proto_name(i))
			printf("proto %s\n", proto_name(i));
	for (i = 1; i < 65536; i++)
		if (skip_port(i))
			printf("skip_server_port %u\n", i);
	for (i = 0; i < cfg.n_peer; i++)
		printf("peer %s\n", addr_str(&cfg.peer[i], abuf, sizeof(abuf)));
	print_list("prefix", &cfg.prefix);
	print_list("exclude", &cfg.exclude);
	print_list("exclude_dst", &cfg.exclude_dst);
	return 0;
}

static int cmd_announce(int argc, char **argv)
{
	struct flow f = { .proto = IPPROTO_UDP };
	char fbuf[128];
	int p;

	if (inet_pton(AF_INET6, argv[0], &f.c) != 1 || parse_port(argv[1], &f.cport) ||
	    inet_pton(AF_INET6, argv[2], &f.s) != 1 || parse_port(argv[3], &f.sport)) {
		fprintf(stderr, "invalid flow, expected: <client-ipv6> <port> <server-ipv6> <port> [proto]\n");
		return 1;
	}
	if (argc == 5) {
		p = proto_num(argv[4]);
		if (p < 0) {
			fprintf(stderr, "unknown protocol '%s'\n", argv[4]);
			return 1;
		}
		f.proto = p;
	}
	if (!wanted(&f))
		fprintf(stderr, "warning: flow does not match the policy, peers will reject it\n");
	if (!cfg.n_peer) {
		fprintf(stderr, "no peers configured\n");
		return 1;
	}
	peers_init();
	udp_fd = udp_open(false);
	if (udp_fd < 0)
		return 1;
	printf("%s\n", flow_str(&f, fbuf, sizeof(fbuf)));
	dgram_add(&f);
	dgram_flush();
	printf("sent to %llu of %u peers\n", (unsigned long long)cnt.tx_datagrams, cfg.n_peer);
	return cnt.tx_errors ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int i;

	/* the daemon logs to syslog unless run from a terminal */
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "run"))
			log_open(true);

	i = parse_args(argc, argv);
	if (i < 0)
		return 1;
	if (i >= argc) {
		usage(stderr);
		return 1;
	}
	cmd = argv[i++];

	if (!strcmp(cmd, "run"))
		return cmd_run();
	if (!strcmp(cmd, "check"))
		return cmd_check();
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "announce") && (argc - i == 4 || argc - i == 5))
		return cmd_announce(argc - i, argv + i);
	usage(stderr);
	return 1;
}
