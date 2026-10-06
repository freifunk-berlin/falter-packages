// SPDX-License-Identifier: GPL-2.0-only
/* main loop and subcommands */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "flowsync.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/*
 * Once the programs are loaded and the sockets open: keep CAP_NET_ADMIN only,
 * as cfg.user if given, with no_new_privs. It is what attaching the programs
 * again (a re-created uplink), reopening the sync socket and nft need; the
 * maps and the event ring are used through descriptors that are open by then.
 * The capability is ambient too, so that the nft we run (fw.c) has it. The
 * network-facing parser then never runs with full root. The status file's
 * directory is created for that user first.
 */
static int drop_privileges(void)
{
	struct __user_cap_header_struct h = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct d[2];
	struct passwd *pw = NULL;
	char dir[PATH_MAX], *slash;
	int cap;

	if (cfg.user) {
		pw = getpwnam(cfg.user);
		if (!pw) {
			logmsg(LOG_ERR, "user %s: unknown", cfg.user);
			return -1;
		}
	}
	snprintf(dir, sizeof(dir), "%s", status_path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		struct stat st;

		*slash = 0;
		/* ours, or root's (left by a run without user, or made by hand):
		 * another user's directory keeps its owner */
		if (pw && (!mkdir(dir, 0755) || (!stat(dir, &st) && S_ISDIR(st.st_mode) &&
						  !st.st_uid)) &&
		    chown(dir, pw->pw_uid, pw->pw_gid))
			logmsg(LOG_WARNING, "%s: chown: %s", dir, strerror(errno));
	}
	/* the bounding set first, while CAP_SETPCAP is still there */
	for (cap = 0; cap < 64; cap++)
		if (cap != CAP_NET_ADMIN && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) && errno != EINVAL) {
			logmsg(LOG_ERR, "capability bounding set: %s", strerror(errno));
			return -1;
		}
	if (pw && (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) || setgroups(0, NULL) ||
		   setgid(pw->pw_gid) || setuid(pw->pw_uid))) {
		logmsg(LOG_ERR, "user %s: %s", cfg.user, strerror(errno));
		return -1;
	}
	memset(d, 0, sizeof(d));
	d[CAP_TO_INDEX(CAP_NET_ADMIN)].effective = CAP_TO_MASK(CAP_NET_ADMIN);
	d[CAP_TO_INDEX(CAP_NET_ADMIN)].permitted = CAP_TO_MASK(CAP_NET_ADMIN);
	d[CAP_TO_INDEX(CAP_NET_ADMIN)].inheritable = CAP_TO_MASK(CAP_NET_ADMIN);
	if (syscall(SYS_capset, &h, d) ||
	    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0, 0) ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		logmsg(LOG_ERR, "capabilities: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int cmd_run(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	sigset_t stopset, waitset;
	struct timespec ts;
	struct pollfd fds[3];
	uint64_t now, next_tick, busy_since = 0, busy, loop_max = 0, last_early = 0;
	char abuf[INET6_ADDRSTRLEN];
	bool first = true, owed = false, fast = false;
	int timeout, nfds;

	if (!cfg.uplink[0]) {
		logmsg(LOG_ERR, "no uplink device: set uplink (or interface)");
		return 1;
	}
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	/* the stop signals are delivered only while the loop waits in ppoll: one
	 * that comes while it works ends that wait at once, instead of being
	 * noticed after a full poll timeout */
	sigemptyset(&stopset);
	sigaddset(&stopset, SIGTERM);
	sigaddset(&stopset, SIGINT);
	sigprocmask(SIG_BLOCK, &stopset, &waitset);

	peers_init();
	udp_fd = udp_open(true);
	/* an interface that does not exist yet is waited for (udp_tick) */
	if ((udp_fd < 0 && errno != ENODEV) || dp_open(true) || fw_open())
		return 1;
	dp_tick();
	if (drop_privileges())
		return 1;
	resync_init();
	if (!cfg.ifname[0])
		logmsg(LOG_WARNING, "no interface set: datagrams with a peer's source address are "
		       "accepted from every interface, the mesh side too (set it to the uplink)");

	logmsg(LOG_NOTICE, "started on %s port %lu, uplink %s: %u peers, %u prefixes, interval %lus, "
	       "element_timeout %lus", cfg.bind_set ? addr_str(&cfg.bind, abuf, sizeof(abuf)) : "*",
	       cfg.port, cfg.uplink, cfg.n_peer, cfg.prefix.n, cfg.interval, cfg.element_timeout);

	next_tick = mono_ms();
	while (!stop) {
		now = mono_ms();
		if (now >= next_tick) {
			gauge.loop_max_ms = loop_max;
			loop_max = 0;
			if (!first)
				log_counters();
			/* the uplink came, was created anew, or lost our filters */
			if (!first)
				dp_tick();
			first = false;
			/* the sync interface came, or came anew: we were deaf and mute */
			if (udp_tick())
				resync_request();
			switch (refresh_start()) {
			case 1:
				owed = false;
				refresh_fast(fast);	/* pulled for a resync request */
				fast = false;
				resync_round_started();	/* it serves the peers' requests */
				break;
			case 0:
				/* the previous round is still being sent: the next
				 * one is owed and starts once its queue is empty */
				owed = true;
				break;
			default:
				/* failed: retried at the next tick, not pulled again
				 * at once (owed would make that a busy loop) */
				owed = false;
				break;
			}
			heartbeat();
			write_status();
			next_tick = now + cfg.interval * 1000;
		}
		fw_tick();
		/* before the checks below: a queue that this sends empty lets them
		 * pull the next round now, not at the next unrelated wake-up */
		if (refresh_pending())
			refresh_tick();
		/* new flows were not announced (event ring or send buffer full):
		 * they wait for the next round. Pull it forward (a second from now,
		 * once the burst that caused it has passed), at most once per
		 * interval so that a sustained overload at most doubles the round
		 * rate. */
		if (events_lost() && !refresh_pending() &&
		    now - last_early >= cfg.interval * 1000 && next_tick > now + EARLY_ROUND_MS) {
			next_tick = now + EARLY_ROUND_MS;
			last_early = now;
		}
		/* a round owed since an overrun tick: as soon as the previous one
		 * is sent, not a whole interval later (which would double the
		 * period between refreshes), and the interval counts from here */
		if (owed && !refresh_pending())
			next_tick = now;
		/* a peer asked for a round (it restarted or was deaf) */
		if (resync_round_wanted() && !refresh_pending()) {
			next_tick = now;
			fast = true;
			resync_round_pulled();
		}
		resync_tick();

		now = mono_ms();
		timeout = next_tick > now ? (int)(next_tick - now) : 0;
		if (refresh_pending() && refresh_pace_ms() < timeout)
			timeout = refresh_pace_ms();
		/* the rule check a second after the ruleset changed */
		if (timeout > 1000)
			timeout = 1000;

		nfds = 0;
		fds[nfds].fd = dp_events_fd();
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		fds[nfds].fd = udp_fd;
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;
		fds[nfds].fd = fw_fd();
		fds[nfds].events = POLLIN;
		fds[nfds++].revents = 0;

		/* handler time from the last poll return to this poll call */
		if (busy_since) {
			busy = now - busy_since;
			if (busy > loop_max)
				loop_max = busy;
		}
		ts.tv_sec = timeout / 1000;
		ts.tv_nsec = (long)(timeout % 1000) * 1000000;
		if (ppoll(fds, nfds, &ts, &waitset) < 0) {
			if (errno == EINTR)
				continue;
			logmsg(LOG_ERR, "poll: %s", strerror(errno));
			return 1;
		}
		busy_since = mono_ms();
		if (fds[0].revents) {
			dp_handle_events(tx_event);
			tx_events_done();
		}
		if (fds[1].revents)
			handle_rx();
		if (fds[2].revents)
			fw_handle();
	}

	/* the programs stay on the uplink and the maps pinned: local flows and,
	 * until they expire, the peers' keep passing while we are down */
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
	printf("uplink %s\n", cfg.uplink[0] ? cfg.uplink : "-");
	printf("bypass %d\n", cfg.bypass);
	printf("port %lu\n", cfg.port);
	printf("interval %lu\n", cfg.interval);
	printf("element_timeout %lu\n", cfg.element_timeout);
	printf("batch_lines %lu\n", cfg.batch_lines);
	printf("tx_rate %lu\n", cfg.tx_rate);
	printf("rcvbuf %lu\n", cfg.rcvbuf);
	printf("mark 0x%08lx\n", cfg.mark);
	printf("max_flows %lu\n", cfg.max_flows);
	printf("max_copies %lu\n", cfg.max_copies);
	printf("udp_timeout %lu\n", cfg.t_udp);
	printf("tcp_timeout %lu\n", cfg.t_tcp);
	printf("tcp_syn_timeout %lu\n", cfg.t_tcp_syn);
	printf("tcp_close_timeout %lu\n", cfg.t_tcp_close);
	printf("other_timeout %lu\n", cfg.t_other);
	printf("resync_rate %lu\n", cfg.resync_rate);
	for (i = 0; i < 256; i++)
		if (cfg.proto[i])
			printf("proto %s\n", proto_str(i, abuf, sizeof(abuf)));
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

static int flow_args(struct flow *f, int argc, char **argv)
{
	int p;

	memset(f, 0, sizeof(*f));
	f->proto = IPPROTO_UDP;
	if (inet_pton(AF_INET6, argv[0], &f->c) != 1 || parse_port(argv[1], &f->cport) ||
	    inet_pton(AF_INET6, argv[2], &f->s) != 1 || parse_port(argv[3], &f->sport)) {
		fprintf(stderr, "invalid flow, expected: <client-ipv6> <port> <server-ipv6> <port> [proto]\n");
		return -1;
	}
	if (argc == 5) {
		p = proto_num(argv[4]);
		if (p < 0) {
			fprintf(stderr, "unknown protocol '%s'\n", argv[4]);
			return -1;
		}
		f->proto = p;
	}
	return 0;
}

static int maps_open(void)
{
	if (!dp_open(false))
		return 0;
	fprintf(stderr, "no maps in %s (the daemon never ran, or detach)\n", cfg.pin_dir);
	return -1;
}

static int list_map(enum dp_map which)
{
	static struct dp_ent ent[WALK_BATCH];
	char cbuf[INET6_ADDRSTRLEN], sbuf[INET6_ADDRSTRLEN], pbuf[8];
	struct dp_walk *w = dp_walk_start(which);
	bool done = false;
	int n, i;

	while (w && !done) {
		n = dp_walk_next(w, ent, WALK_BATCH, &done);
		if (n < 0) {
			perror("map walk");
			break;
		}
		for (i = 0; i < n; i++) {
			printf("%s %s [%s]:%u -> [%s]:%u left=%u", which == DP_LOCAL ? "local" : "remote",
			       proto_str(ent[i].f.proto, pbuf, sizeof(pbuf)),
			       inet_ntop(AF_INET6, &ent[i].f.c, cbuf, sizeof(cbuf)), ent[i].f.cport,
			       inet_ntop(AF_INET6, &ent[i].f.s, sbuf, sizeof(sbuf)), ent[i].f.sport,
			       ent[i].left);
			if (which == DP_LOCAL)
				printf(" age=%u flags=%u", ent[i].age, ent[i].flags);
			printf("\n");
		}
	}
	dp_walk_end(w);
	return done ? 0 : 1;
}

/* the lifetimes shown are computed with this invocation's timeout options */
static int cmd_flows(int argc, char **argv)
{
	int ret = 0;

	if (maps_open())
		return 1;
	if (!argc || !strcmp(argv[0], "local"))
		ret |= list_map(DP_LOCAL);
	if (!argc || !strcmp(argv[0], "remote"))
		ret |= list_map(DP_REMOTE);
	return ret;
}

/* one flow in both tables; exit status 0 if it is alive in either */
static int cmd_flow(int argc, char **argv)
{
	struct dp_ent e;
	struct flow f;
	bool alive = false;

	if (flow_args(&f, argc, argv) || maps_open())
		return 2;
	if (!dp_get(DP_LOCAL, &f, &e)) {
		printf("local left=%u age=%u flags=%u\n", e.left, e.age, e.flags);
		alive |= e.left > 0;
	} else {
		printf("local none\n");
	}
	if (!dp_get(DP_REMOTE, &f, &e)) {
		printf("remote left=%u\n", e.left);
		alive |= e.left > 0;
	} else {
		printf("remote none\n");
	}
	return alive ? 0 : 1;
}

/* the switch in the pinned control map: takes effect with the next packet,
 * and lasts until the daemon attaches the programs again (it then applies
 * its own configuration) */
static int cmd_bypass(int argc, char **argv)
{
	int on;

	if (maps_open())
		return 2;
	if (argc) {
		if (strcmp(argv[0], "on") && strcmp(argv[0], "off")) {
			fprintf(stderr, "bypass: on or off\n");
			return 2;
		}
		if (dp_set_bypass(!strcmp(argv[0], "on"))) {
			perror("bypass");
			return 1;
		}
	}
	on = dp_get_bypass();
	if (on < 0) {
		perror("bypass");
		return 1;
	}
	printf("bypass %s\n", on ? "on" : "off");
	return 0;
}

static int cmd_detach(void)
{
	if (!cfg.uplink[0]) {
		fprintf(stderr, "no uplink device: set uplink (or interface)\n");
		return 1;
	}
	fw_remove();
	return dp_detach();
}

static int cmd_announce(int argc, char **argv)
{
	struct flow f;
	char fbuf[128];

	if (flow_args(&f, argc, argv))
		return 1;
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
	if (!strcmp(cmd, "flows") && argc - i <= 1)
		return cmd_flows(argc - i, argv + i);
	if (!strcmp(cmd, "flow") && (argc - i == 4 || argc - i == 5))
		return cmd_flow(argc - i, argv + i);
	if (!strcmp(cmd, "bypass") && argc - i <= 1)
		return cmd_bypass(argc - i, argv + i);
	if (!strcmp(cmd, "detach"))
		return cmd_detach();
	usage(stderr);
	return 1;
}
