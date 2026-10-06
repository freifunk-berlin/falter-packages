// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
/*
 * The nftables rules the datapath needs, and keeping them in place.
 *
 * 1. Our own table: forwarded IPv6 bypasses conntrack (notrack for every
 *    packet whose destination is not this host). Installed one interval after
 *    the programs were attached, so that flows from before have sent a
 *    packet and are in the local map by then; until then conntrack still
 *    accepts their replies.
 * 2. One rule in the firewall's forward chain: accept what the ingress
 *    program marked. An accept in a table of our own would not do, the
 *    firewall's chain still sees the packet and rejects it. The package
 *    ships the rule as an fw4 include (it is there from the first ruleset
 *    on, and after every reload); here it is checked and, if missing, put
 *    back.
 *
 * Both are looked at whenever the ruleset changes (a netlink notification)
 * and every interval. The rules are read and written with the nft tool: this
 * happens a few times an hour at most.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flowsync.h"

#define OWN_TABLE	"flowsync"
#define RULE_TAG	"comment \"flowsync\""
#define SETTLE_MS	500

static int gen_fd = -1;
static bool dirty = true;
static uint64_t dirty_at;
static uint64_t attached_at;	/* when we first saw the programs attached */

/* run nft; script goes to its stdin if given, its stdout into out. Returns
 * its exit status, -1 if it could not be run. */
static int nft(char *const argv[], const char *script, char *out, size_t outlen)
{
	int in[2] = { -1, -1 }, outp[2] = { -1, -1 }, status, null;
	sigset_t none;
	size_t len = 0;
	ssize_t n;
	pid_t pid;

	if (out)
		out[0] = 0;
	if (pipe2(in, O_CLOEXEC) || pipe2(outp, O_CLOEXEC))
		goto fail;
	pid = fork();
	if (pid < 0)
		goto fail;
	if (!pid) {
		/* the daemon blocks its stop signals outside ppoll */
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		null = open("/dev/null", O_WRONLY);
		dup2(in[0], 0);
		dup2(outp[1], 1);
		if (null >= 0)
			dup2(null, 2);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(in[0]);
	close(outp[1]);
	if (script && write(in[1], script, strlen(script)) < 0)
		logmsg(LOG_WARNING, "nft: %s", strerror(errno));
	close(in[1]);
	for (;;) {
		char skip[512];

		/* what does not fit is read and dropped, or nft would block */
		n = out && len + 1 < outlen ? read(outp[0], out + len, outlen - 1 - len) :
					       read(outp[0], skip, sizeof(skip));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (out && len + 1 < outlen) {
			len += n;
			out[len] = 0;
		}
	}
	close(outp[0]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
fail:
	logmsg(LOG_WARNING, "nft: %s", strerror(errno));
	if (in[0] >= 0) {
		close(in[0]);
		close(in[1]);
	}
	if (outp[0] >= 0) {
		close(outp[0]);
		close(outp[1]);
	}
	return -1;
}

static bool own_table_present(void)
{
	char *argv[] = { "nft", "list", "table", "inet", OWN_TABLE, NULL };

	return nft(argv, NULL, NULL, 0) == 0;
}

/*
 * "unicast": routed, not one of our addresses and not multicast, i.e. what
 * will be forwarded. The second chain is never run; a conntrack expression
 * anywhere in the ruleset keeps the kernel's defragmentation hooked in, which
 * the marks of fragmented packets rely on (see bpf/flowsync.bpf.c).
 */
static int own_table_install(void)
{
	char *argv[] = { "nft", "-f", "-", NULL };
	static const char script[] =
		"table inet " OWN_TABLE " {\n"
		"	chain prerouting {\n"
		"		type filter hook prerouting priority raw; policy accept;\n"
		"		meta nfproto ipv6 fib daddr type unicast notrack\n"
		"	}\n"
		"	chain defrag {\n"
		"		ct state untracked accept\n"
		"	}\n"
		"}\n";

	return nft(argv, script, NULL, 0);
}

/* 1: the accept rule is in the firewall's forward chain, 0: it is not,
 * -1: there is no such chain */
static int fw_rule_present(char *buf, size_t len)
{
	char *argv[] = { "nft", "-a", "list", "chain", "inet", (char *)cfg.fw_table, "forward", NULL };

	if (nft(argv, NULL, buf, len))
		return -1;
	return strstr(buf, RULE_TAG) != NULL;
}

static int fw_rule_insert(void)
{
	char *argv[] = { "nft", "-f", "-", NULL };
	char script[256];

	snprintf(script, sizeof(script),
		 "insert rule inet %s forward meta nfproto ipv6 meta mark & 0x%08lx == 0x%08lx "
		 "accept " RULE_TAG "\n", cfg.fw_table, cfg.mark, cfg.mark);
	return nft(argv, script, NULL, 0);
}

static void fw_check(void)
{
	static char buf[65536];
	static uint64_t last_log;
	static bool inserted_before;
	uint64_t now = mono_ms();
	bool ok = true;
	int r;

	dirty = false;
	if (gauge.attached && !attached_at)
		attached_at = now;

	r = fw_rule_present(buf, sizeof(buf));
	if (r < 0) {
		ok = false;
		if (log_ok(&last_log))
			logmsg(LOG_ERR, "no chain forward in table inet %s: nothing accepts the "
			       "marked packets (is the firewall running?)", cfg.fw_table);
	} else if (!r) {
		if (fw_rule_insert()) {
			ok = false;
			if (log_ok(&last_log))
				logmsg(LOG_ERR, "could not add the accept rule to inet %s forward",
				       cfg.fw_table);
		} else {
			cnt.fw_repaired++;
			logmsg(inserted_before ? LOG_WARNING : LOG_NOTICE,
			       "accept rule for mark 0x%08lx added to inet %s forward%s", cfg.mark,
			       cfg.fw_table, inserted_before ?
			       " again: something removed it (a firewall reload without the "
			       "package's include?)" : "");
			inserted_before = true;
		}
	}

	if (!own_table_present()) {
		/* not before the programs are on the uplink, and learning for one
		 * interval: without them nothing would be accepted any more */
		if (!attached_at || now - attached_at < cfg.interval * 1000) {
			ok = false;
		} else if (own_table_install()) {
			ok = false;
			if (log_ok(&last_log))
				logmsg(LOG_ERR, "could not install table inet " OWN_TABLE
				       " (kmod-nft-fib?)");
		} else {
			logmsg(LOG_NOTICE, "table inet " OWN_TABLE " installed: forwarded IPv6 "
			       "bypasses conntrack");
		}
	}
	gauge.fw_ok = ok;
}

/* ruleset change notifications */
int fw_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
				  .nl_groups = 1u << (NFNLGRP_NFTABLES - 1) };

	gen_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_NETFILTER);
	if (gen_fd < 0 || bind(gen_fd, (struct sockaddr *)&sa, sizeof(sa))) {
		logmsg(LOG_WARNING, "nftables notifications: %s (rules are checked every "
		       "interval only)", strerror(errno));
		if (gen_fd >= 0)
			close(gen_fd);
		gen_fd = -1;
	}
	return 0;
}

int fw_fd(void)
{
	return gen_fd;
}

/* something changed the ruleset (or the socket overran: the same to us) */
void fw_handle(void)
{
	char buf[8192];

	while (recv(gen_fd, buf, sizeof(buf), MSG_DONTWAIT) > 0 || errno == ENOBUFS)
		;
	if (!dirty) {
		dirty = true;
		dirty_at = mono_ms();
	}
}

/* main loop, every iteration: check once the change has settled; every
 * interval (force) regardless */
void fw_tick(void)
{
	static uint64_t last;
	uint64_t now = mono_ms();

	if ((dirty && now - dirty_at >= SETTLE_MS) || now - last >= cfg.interval * 1000) {
		last = now;
		fw_check();
		/* our own changes notify us too */
		if (gen_fd >= 0)
			fw_handle();
		dirty = false;
	}
}

/* the detach command: our table and the accept rule go */
void fw_remove(void)
{
	char *del[] = { "nft", "delete", "table", "inet", OWN_TABLE, NULL };
	char *argv[] = { "nft", "-f", "-", NULL };
	static char buf[65536];
	char script[128], *p, *h;

	nft(del, NULL, NULL, 0);
	if (fw_rule_present(buf, sizeof(buf)) != 1)
		return;
	p = strstr(buf, RULE_TAG);
	h = strstr(p, "# handle ");
	if (!h)
		return;
	snprintf(script, sizeof(script), "delete rule inet %s forward handle %lu\n", cfg.fw_table,
		 strtoul(h + 9, NULL, 10));
	nft(argv, script, NULL, 0);
}
