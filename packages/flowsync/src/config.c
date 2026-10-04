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

static int opt_uint(const char *name, const char *val, unsigned long min,
		    unsigned long max, unsigned long *out)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(val, &end, 0);
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
	} else if (!strcmp(name, "bind_address")) {
		cfg.bind_set = false;
		if (!*val)
			return 0;
		if (parse_addr(val, &cfg.bind)) {
			logmsg(LOG_ERR, "option %s: invalid address '%s'", name, val);
			return -1;
		}
		cfg.bind_set = true;
	} else if (!strcmp(name, "port")) {
		return opt_uint(name, val, 1, 65535, &cfg.port);
	} else if (!strcmp(name, "interval")) {
		return opt_uint(name, val, 1, 3600, &cfg.interval);
	} else if (!strcmp(name, "element_timeout")) {
		return opt_uint(name, val, 1, 86400, &cfg.element_timeout);
	} else if (!strcmp(name, "batch_lines")) {
		return opt_uint(name, val, 1, WIRE_MAX_RECORDS, &cfg.batch_lines);
	} else if (!strcmp(name, "tx_rate")) {
		return opt_uint(name, val, 1, 1000000, &cfg.tx_rate);
	} else if (!strcmp(name, "rcvbuf")) {
		return opt_uint(name, val, 4096, INT_MAX / 2, &cfg.rcvbuf);
	} else if (!strcmp(name, "ct_mark")) {
		return opt_uint(name, val, 0, UINT32_MAX, &cfg.ct_mark);
	} else if (!strcmp(name, "ct_mark_mask")) {
		return opt_uint(name, val, 0, UINT32_MAX, &cfg.ct_mark_mask);
	} else if (!strcmp(name, "max_copies")) {
		return opt_uint(name, val, 0, 100000000, &cfg.max_copies);
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
		if (opt_uint(name, val, 1, 65535, &v))
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

/* short option, long option, UCI name, argument name for the help text */
static const struct {
	int c;
	const char *name;
	const char *uci;
	const char *arg;
	const char *help;
} options[] = {
	{ 'b', "bind",             "bind_address",     "ADDR",  "local address (default: any)" },
	{ 'p', "port",             "port",             "N",     "UDP port (3780)" },
	{ 'i', "interval",         "interval",         "SEC",   "refresh and log interval (30)" },
	{ 't', "element-timeout",  "element_timeout",  "SEC",   "timeout of injected entries (90)" },
	{ 'l', "batch-lines",      "batch_lines",      "N",     "records per datagram, 1..34 (30)" },
	{ 'r', "tx-rate",          "tx_rate",          "N",     "refresh datagrams per second per peer (500)" },
	{ 'B', "rcvbuf",           "rcvbuf",           "BYTES", "socket receive buffers (8388608)" },
	{ 'm', "ct-mark",          "ct_mark",          "HEX",   "mark set on injected entries (0x01000000)" },
	{ 'M', "ct-mark-mask",     "ct_mark_mask",     "HEX",   "mask of that mark (0x01000000)" },
	{ 'C', "max-copies",       "max_copies",       "N",     "most copies held, 0: nf_conntrack_max/4 (0)" },
	{ 'P', "proto",            "proto",            "NAME",  "synced protocol, repeatable (udp tcp)" },
	{ 'S', "skip-server-port", "skip_server_port", "N",     "server port never synced, repeatable (53)" },
	{ 'e', "peer",             "peer",             "ADDR",  "other gateway, repeatable" },
	{ 'x', "prefix",           "prefix",           "CIDR",  "synced client prefix, repeatable" },
	{ 'X', "exclude",          "exclude",          "CIDR",  "client prefix not synced, repeatable" },
	{ 'D', "exclude-dst",      "exclude_dst",      "CIDR",  "server prefix not synced, repeatable" },
	{ 's', "status-file",      NULL,               "PATH",  "status file (" STATUS_FILE ")" },
	{ 'd', "debug",            "debug",            NULL,    "log every record and injection" },
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
		"  status                                     print counters, peers, conntrack count\n"
		"  announce <client> <cport> <server> <sport> [proto] send one record to all peers\n"
		"options (defaults in parentheses):\n");
	for (i = 0; i < N_OPTIONS; i++)
		fprintf(out, "  -%c, --%-18s %-6s %s\n", options[i].c, options[i].name,
			options[i].arg ? options[i].arg : "", options[i].help);
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
	cfg.ct_mark = 0x01000000;
	cfg.ct_mark_mask = 0x01000000;

	memset(longopts, 0, sizeof(longopts));
	*s++ = ':';
	for (i = 0; i < N_OPTIONS; i++) {
		longopts[i].name = options[i].name;
		longopts[i].has_arg = options[i].arg ? required_argument : no_argument;
		longopts[i].val = options[i].c;
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
		cfg.proto[IPPROTO_UDP] = true;
		cfg.proto[IPPROTO_TCP] = true;
		cfg.n_proto = 2;
	}
	if (!cfg.n_skip_port) {
		cfg.skip_port[53 / 8] |= 1 << (53 % 8);
		cfg.n_skip_port = 1;
	}

	if (!cfg.ct_mark_mask || !cfg.ct_mark || (cfg.ct_mark & ~cfg.ct_mark_mask)) {
		logmsg(LOG_ERR, "ct_mark 0x%08lx / ct_mark_mask 0x%08lx: mark must be "
		       "non-zero and inside the mask", cfg.ct_mark, cfg.ct_mark_mask);
		ret = -1;
	}
	if (cfg.element_timeout < 2 * cfg.interval) {
		logmsg(LOG_ERR, "element_timeout %lu must be at least 2 x interval %lu: one "
		       "lost datagram would expire entries before the next refresh",
		       cfg.element_timeout, cfg.interval);
		ret = -1;
	}
	if (!cfg.prefix.n)
		logmsg(LOG_WARNING, "no prefix configured, nothing will be synced");
	if (!cfg.n_peer)
		logmsg(LOG_WARNING, "no peer configured, nothing will be sent or accepted");

	return ret ? -1 : optind;
}
