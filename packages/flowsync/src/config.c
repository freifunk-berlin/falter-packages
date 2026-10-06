// SPDX-License-Identifier: GPL-2.0-only
/*
 * Configuration from the command line. The option names in apply_option()
 * are the UCI option names; on OpenWrt the init script renders UCI into
 * these options, elsewhere they are passed directly.
 */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "flowsync.h"

struct config cfg;

/* decimal (base 10), or hexadecimal with or without 0x (base 16) for the
 * marks: base 0 would read "01000000" as octal and "010" as 8 */
static int opt_uint(const char *name, const char *val, unsigned long min,
		    unsigned long max, unsigned long *out, int base)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(val, &end, base);
	if (errno || end == val || *end || val[0] == '-' || v < min || v > max) {
		logmsg(LOG_ERR, "option %s: invalid value '%s' (allowed %lu..%lu)",
		       name, val, min, max);
		return -1;
	}
	*out = v;
	return 0;
}

static int opt_prefix(const char *name, const char *val, struct prefix_list *l)
{
	if (l->n == MAX_PREFIXES) {
		logmsg(LOG_ERR, "option %s: more than %d entries", name, MAX_PREFIXES);
		return -1;
	}
	if (parse_prefix(val, &l->p[l->n])) {
		logmsg(LOG_ERR, "option %s: invalid IPv6 prefix '%s'", name, val);
		return -1;
	}
	l->n++;
	return 0;
}

static int apply_option(const char *name, const char *val)
{
	unsigned long v;
	int p;

	if (!strcmp(name, "debug")) {
		cfg.debug = !strcmp(val, "1") || !strcmp(val, "on") ||
			    !strcmp(val, "true") || !strcmp(val, "yes");
	} else if (!strcmp(name, "bypass")) {
		cfg.bypass = !strcmp(val, "1") || !strcmp(val, "on") ||
			     !strcmp(val, "true") || !strcmp(val, "yes");
	} else if (!strcmp(name, "bind_address")) {
		cfg.bind_set = false;
		if (!*val)
			return 0;
		if (parse_addr(val, &cfg.bind)) {
			logmsg(LOG_ERR, "option %s: invalid address '%s'", name, val);
			return -1;
		}
		cfg.bind_set = true;
	} else if (!strcmp(name, "interface")) {
		if (strlen(val) >= sizeof(cfg.ifname)) {
			logmsg(LOG_ERR, "option %s: name too long '%s'", name, val);
			return -1;
		}
		strcpy(cfg.ifname, val);	/* "" for any */
	} else if (!strcmp(name, "port")) {
		return opt_uint(name, val, 1, 65535, &cfg.port, 10);
	} else if (!strcmp(name, "interval")) {
		return opt_uint(name, val, 1, 3600, &cfg.interval, 10);
	} else if (!strcmp(name, "element_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.element_timeout, 10);
	} else if (!strcmp(name, "batch_lines")) {
		return opt_uint(name, val, 1, WIRE_MAX_RECORDS, &cfg.batch_lines, 10);
	} else if (!strcmp(name, "tx_rate")) {
		return opt_uint(name, val, 1, 1000000, &cfg.tx_rate, 10);
	} else if (!strcmp(name, "rcvbuf")) {
		return opt_uint(name, val, 4096, INT_MAX / 2, &cfg.rcvbuf, 10);
	} else if (!strcmp(name, "uplink")) {
		if (!*val || strlen(val) >= sizeof(cfg.uplink)) {
			logmsg(LOG_ERR, "option %s: invalid device name '%s'", name, val);
			return -1;
		}
		strcpy(cfg.uplink, val);
	} else if (!strcmp(name, "mark")) {
		return opt_uint(name, val, 1, UINT32_MAX, &cfg.mark, 16);
	} else if (!strcmp(name, "max_flows")) {
		return opt_uint(name, val, 1024, 16777216, &cfg.max_flows, 10);
	} else if (!strcmp(name, "max_copies")) {
		return opt_uint(name, val, 1024, 16777216, &cfg.max_copies, 10);
	} else if (!strcmp(name, "udp_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.t_udp, 10);
	} else if (!strcmp(name, "tcp_timeout")) {
		return opt_uint(name, val, 1, 864000, &cfg.t_tcp, 10);
	} else if (!strcmp(name, "tcp_syn_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.t_tcp_syn, 10);
	} else if (!strcmp(name, "tcp_close_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.t_tcp_close, 10);
	} else if (!strcmp(name, "other_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.t_other, 10);

	} else if (!strcmp(name, "resync_rate")) {
		return opt_uint(name, val, 0, 4000000, &cfg.resync_rate, 10);
	} else if (!strcmp(name, "proto")) {
		p = proto_num(val);
		if (p < 0) {
			logmsg(LOG_ERR, "option proto: unknown protocol '%s'", val);
			return -1;
		} else if (!cfg.proto[p]) {
			cfg.proto[p] = true;
			cfg.n_proto++;
		}
	} else if (!strcmp(name, "skip_server_port")) {
		if (opt_uint(name, val, 1, 65535, &v, 10))
			return -1;
		cfg.skip_port[v / 8] |= 1 << (v % 8);
		cfg.n_skip_port++;
	} else if (!strcmp(name, "peer")) {
		if (cfg.n_peer == MAX_PEERS) {
			logmsg(LOG_ERR, "option peer: more than %d peers", MAX_PEERS);
			return -1;
		}
		if (parse_addr(val, &cfg.peer[cfg.n_peer])) {
			logmsg(LOG_ERR, "option peer: invalid address '%s'", val);
			return -1;
		}
		cfg.n_peer++;
	} else if (!strcmp(name, "prefix")) {
		return opt_prefix(name, val, &cfg.prefix);
	} else if (!strcmp(name, "exclude")) {
		return opt_prefix(name, val, &cfg.exclude);
	} else if (!strcmp(name, "exclude_dst")) {
		return opt_prefix(name, val, &cfg.exclude_dst);
	}
	return 0;
}

/* short option (from 256: none), long option, UCI name, argument name for
 * the help text */
enum { OPT_UDP_T = 256, OPT_TCP_T, OPT_TCP_SYN_T, OPT_TCP_CLOSE_T, OPT_OTHER_T,
       OPT_BPF_OBJECT, OPT_PIN_DIR, OPT_FW_TABLE, OPT_BYPASS };
static const struct {
	int c;
	const char *name;
	const char *uci;
	const char *arg;
	const char *help;
} options[] = {
	{ 'b', "bind",             "bind_address",     "ADDR",  "local address (default: any)" },
	{ 'I', "interface",        "interface",        "DEV",   "accept sync datagrams on this device only" },
	{ 'U', "uplink",           "uplink",           "DEV",
	  "the device to the Internet, where the programs attach (interface)" },
	{ 'p', "port",             "port",             "N",     "UDP port (3780)" },
	{ 'i', "interval",         "interval",         "SEC",   "refresh and log interval (30)" },
	{ 't', "element-timeout",  "element_timeout",  "SEC",   "lifetime of a peer's flow after its last announcement (90)" },
	{ 'l', "batch-lines",      "batch_lines",      "N",     "records per datagram, 1..34 (30)" },
	{ 'r', "tx-rate",          "tx_rate",          "N",     "refresh datagrams per second per peer (500)" },
	{ 'R', "resync-rate",      "resync_rate",      "N",
	  "the same for a round that answers a resync request, 0: 4 x tx_rate (0)" },
	{ 'B', "rcvbuf",           "rcvbuf",           "BYTES", "socket receive buffers (8388608)" },
	{ 'm', "mark",             "mark",             "HEX",   "packet mark of accepted packets (0x01000000)" },
	{ 'F', "max-flows",        "max_flows",        "N",     "most local flows (131072)" },
	{ 'C', "max-copies",       "max_copies",       "N",     "most flows held for the peers (131072)" },
	{ OPT_UDP_T, "udp-timeout", "udp_timeout",     "SEC",   "local flow lifetime after its last packet out: UDP (180)" },
	{ OPT_TCP_T, "tcp-timeout", "tcp_timeout",     "SEC",   "TCP (7440)" },
	{ OPT_TCP_SYN_T, "tcp-syn-timeout", "tcp_syn_timeout", "SEC", "TCP, only SYNs so far (120)" },
	{ OPT_TCP_CLOSE_T, "tcp-close-timeout", "tcp_close_timeout", "SEC",
	  "TCP, after the client's FIN or RST (120)" },
	{ OPT_OTHER_T, "other-timeout", "other_timeout", "SEC", "other protocols (600)" },
	{ 'P', "proto",            "proto",            "NAME",
	  "synced protocol, repeatable: tcp udp sctp esp gre ipip ip6ip6 l2tp or a number "
	  "(udp tcp esp gre ipip ip6ip6 l2tp)" },
	{ 'S', "skip-server-port", "skip_server_port", "N",     "server port never synced, repeatable (53)" },
	{ 'e', "peer",             "peer",             "ADDR",  "other gateway, repeatable" },
	{ 'x', "prefix",           "prefix",           "CIDR",  "synced client prefix, repeatable" },
	{ 'X', "exclude",          "exclude",          "CIDR",  "client prefix not synced, repeatable" },
	{ 'D', "exclude-dst",      "exclude_dst",      "CIDR",  "server prefix not synced, repeatable" },
	{ 's', "status-file",      NULL,               "PATH",  "status file (" STATUS_FILE ")" },
	{ 'u', "user",             NULL,               "NAME",  "run as this user, CAP_NET_ADMIN only" },
	{ OPT_BYPASS, "bypass",    "bypass",           NULL,
	  "forward accepted TCP and UDP packets from tc, past netfilter" },
	{ OPT_BPF_OBJECT, "bpf-object", NULL,          "PATH",  "the tc programs (" BPF_OBJECT ")" },
	{ OPT_PIN_DIR, "pin-dir",  NULL,               "PATH",  "where the maps are pinned (" PIN_DIR ")" },
	{ OPT_FW_TABLE, "fw-table", NULL,              "NAME",  "the firewall's inet table (" FW_TABLE ")" },
	{ 'd', "debug",            "debug",            NULL,    "log every record sent and received" },
	{ 'h', "help",             NULL,               NULL,    "this text" },
};
#define N_OPTIONS (sizeof(options) / sizeof(options[0]))

void usage(FILE *out)
{
	unsigned int i;

	fprintf(out,
		"usage: flowsync [options] <command>\n"
		"  run                                        run the daemon\n"
		"  check                                      print the parsed configuration\n"
		"  status                                     print counters, peers, packet counters\n"
		"  announce <client> <cport> <server> <sport> [proto] send one record to all peers\n"
		"  flows [local|remote]                       list the flow tables\n"
		"  flow <client> <cport> <server> <sport> [proto] look one flow up in both tables\n"
		"  bypass [on|off]                            show or switch the bypass of the running programs\n"
		"  detach                                     take the programs, maps and rules away\n"
		"options (defaults in parentheses):\n");
	for (i = 0; i < N_OPTIONS; i++) {
		if (options[i].c < 256)
			fprintf(out, "  -%c, ", options[i].c);
		else
			fprintf(out, "      ");
		fprintf(out, "--%-18s %-6s %s\n", options[i].name,
			options[i].arg ? options[i].arg : "", options[i].help);
	}
}

/* parse the command line into cfg; returns the index of the command or -1 */
int parse_args(int argc, char **argv)
{
	struct option longopts[N_OPTIONS + 1];
	char shortopts[3 * N_OPTIONS + 2], *s = shortopts;
	unsigned int i;
	int c, ret = 0;

	memset(&cfg, 0, sizeof(cfg));
	cfg.port = 3780;
	cfg.interval = 30;
	cfg.element_timeout = 90;
	cfg.batch_lines = MAX_BATCH;
	cfg.tx_rate = 500;
	cfg.rcvbuf = 8388608;
	cfg.mark = 0x01000000;
	cfg.max_flows = 131072;
	cfg.max_copies = 131072;
	cfg.t_udp = 180;
	cfg.t_tcp = 7440;
	cfg.t_tcp_syn = 120;
	cfg.t_tcp_close = 120;
	cfg.t_other = 600;
	cfg.bpf_object = BPF_OBJECT;
	cfg.pin_dir = PIN_DIR;
	cfg.fw_table = FW_TABLE;

	memset(longopts, 0, sizeof(longopts));
	*s++ = ':';
	for (i = 0; i < N_OPTIONS; i++) {
		longopts[i].name = options[i].name;
		longopts[i].has_arg = options[i].arg ? required_argument : no_argument;
		longopts[i].val = options[i].c;
		if (options[i].c >= 256)
			continue;
		*s++ = options[i].c;
		if (options[i].arg)
			*s++ = ':';
	}
	*s = 0;

	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		if (c == 'h') {
			usage(stdout);
			exit(0);
		}
		if (c == 's') {
			status_path = optarg;
			continue;
		}
		if (c == 'u') {
			cfg.user = optarg;
			continue;
		}
		if (c == OPT_BPF_OBJECT) {
			cfg.bpf_object = optarg;
			continue;
		}
		if (c == OPT_PIN_DIR) {
			cfg.pin_dir = optarg;
			continue;
		}
		if (c == OPT_FW_TABLE) {
			cfg.fw_table = optarg;
			continue;
		}
		if (c == ':' || c == '?') {
			/* optopt is 0 for long options; argv[optind-1] has the text */
			logmsg(LOG_ERR, "%s: %s", c == ':' ? "missing argument" :
			       "unknown option", argv[optind - 1]);
			return -1;
		}
		for (i = 0; i < N_OPTIONS; i++)
			if (options[i].c == c)
				ret |= apply_option(options[i].uci, optarg ? optarg : "1");
	}

	if (!cfg.n_proto) {
		/* what clients use across the gateways: everything on TCP and
		 * UDP, IPsec without UDP encapsulation, and plain tunnels */
		static const char *const def[] = { "udp", "tcp", "esp", "gre", "ipip", "ip6ip6",
						   "l2tp" };

		for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
			cfg.proto[proto_num(def[i])] = true;
		cfg.n_proto = i;
	}
	if (!cfg.n_skip_port) {
		cfg.skip_port[53 / 8] |= 1 << (53 % 8);
		cfg.n_skip_port = 1;
	}
	if (!cfg.resync_rate)
		cfg.resync_rate = 4 * cfg.tx_rate;

	/* the tc programs go where the forwarded traffic leaves; by default the
	 * device the sync datagrams come in on, which is the uplink too */
	if (!cfg.uplink[0])
		strcpy(cfg.uplink, cfg.ifname);
	/* a peer's flow lives element_timeout after its last announcement, and a
	 * flow is announced once per interval: with 2 x interval a single lost
	 * datagram expires it just before the next one */
	if (cfg.element_timeout < 3 * cfg.interval) {
		logmsg(LOG_ERR, "element_timeout %lu must be at least 3 x interval %lu: with "
		       "less, one lost datagram expires peers' flows before the next refresh",
		       cfg.element_timeout, cfg.interval);
		ret = -1;
	}
	for (i = 0; i < cfg.n_peer; i++) {
		unsigned int k;

		if (cfg.bind_set && IN6_ARE_ADDR_EQUAL(&cfg.peer[i], &cfg.bind)) {
			logmsg(LOG_ERR, "peer %u is our own bind_address", i + 1);
			ret = -1;
		}
		/* a socket bound to an IPv4 address cannot reach an IPv6 peer and
		 * vice versa: every send would fail */
		if (cfg.bind_set && !IN6_IS_ADDR_UNSPECIFIED(&cfg.bind) &&
		    IN6_IS_ADDR_V4MAPPED(&cfg.peer[i]) != IN6_IS_ADDR_V4MAPPED(&cfg.bind)) {
			logmsg(LOG_ERR, "peer %u and bind_address are of different address families",
			       i + 1);
			ret = -1;
		}
		for (k = 0; k < i; k++)
			if (IN6_ARE_ADDR_EQUAL(&cfg.peer[i], &cfg.peer[k])) {
				logmsg(LOG_ERR, "peer %u is listed twice", k + 1);
				ret = -1;
			}
	}
	if (!cfg.prefix.n)
		logmsg(LOG_WARNING, "no prefix configured, nothing will be synced");
	if (!cfg.n_peer)
		logmsg(LOG_WARNING, "no peer configured, nothing will be sent or accepted");

	return ret ? -1 : optind;
}
