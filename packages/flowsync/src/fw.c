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
 *    on, and after every reload); here it is checked and, if it is missing
 *    or names another mark than ours, put right.
 *
 * The table makes the gateway depend on the programs: what conntrack no
 * longer sees, only they can accept. When they cannot be on the uplink, or
 * the rule cannot be put in, for a whole interval, the table is taken away
 * again and conntrack carries the flows on a symmetric path, as it did before
 * flowsync. It comes back as at the first start.
 *
 * Both are looked at when the ruleset changes (a netlink notification), when
 * the programs come or go, and when time has made a step due; every interval
 * only while something is wrong or there are no notifications. The rules are
 * read and written with the nft tool: two short runs per look.
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
#define RULE_FILE	"/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft"
#define SETTLE_MS	500

static int gen_fd = -1;
static bool dirty = true;
static uint64_t dirty_at;
static uint64_t works_since;	/* the datapath accepts its flows since; 0: it does not */
static uint64_t broken_since;	/* ... cannot since; 0: it can */
static uint64_t due;		/* a look that time alone makes necessary; 0: none */

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

static int own_table_remove(void)
{
	char *argv[] = { "nft", "delete", "table", "inet", OWN_TABLE, NULL };

	return nft(argv, NULL, NULL, 0);
}

/*
 * The next rule with our comment in a chain listing (nft -a), from p on:
 * returns where to go on and sets its handle and whether it names our mark;
 * NULL when there is none left.
 */
static const char *rule_next(const char *listing, const char *p, unsigned long *handle, bool *ours)
{
	const char *tag = strstr(p, RULE_TAG), *bol, *eol, *h, *m;
	char want[48];

	if (!tag)
		return NULL;
	for (bol = tag; bol > listing && bol[-1] != '\n'; bol--)
		;
	eol = strchr(tag, '\n');
	if (!eol)
		eol = tag + strlen(tag);
	h = strstr(tag, "# handle ");
	*handle = h && h < eol ? strtoul(h + 9, NULL, 10) : 0;
	/* as nft prints what the include and fw_rules_fix() say */
	snprintf(want, sizeof(want), "meta mark & 0x%08lx == 0x%08lx ", cfg.mark, cfg.mark);
	m = strstr(bol, want);
	*ours = m && m < tag;
	return eol;
}

/* the forward chain with handles; -1: there is no such chain */
static int fw_chain_list(char *buf, size_t len)
{
	char *argv[] = { "nft", "-a", "list", "chain", "inet", (char *)cfg.fw_table, "forward", NULL };

	return nft(argv, NULL, buf, len) ? -1 : 0;
}

/*
 * Rules with our comment and another mark go, and ours comes in if it is not
 * there, in one transaction. Its place is where the package's include puts
 * it: behind everything the chain does to every packet before it accepts any
 * (on the gateways the MSS clamp on SYNs, which a marked SYN/ACK must still
 * pass), right before the rule that accepts established flows; without such a
 * rule at the top. Returns 0 if nothing was to do, 1 if the chain was put
 * right (*replaced: a rule for another mark went), -1 if that failed.
 */
static int fw_rules_fix(const char *listing, bool *replaced)
{
	char *argv[] = { "nft", "-f", "-", NULL };
	char script[1024], pos[48] = "";
	const char *ct = strstr(listing, "ct state"), *h, *eol, *p = listing;
	unsigned long handle;
	size_t len = 0;
	bool ours, have = false;

	*replaced = false;
	script[0] = 0;
	while ((p = rule_next(listing, p, &handle, &ours)) != NULL) {
		if (ours) {
			have = true;
		} else if (handle && len < sizeof(script) - 128) {
			len += snprintf(script + len, sizeof(script) - len,
					"delete rule inet %s forward handle %lu\n", cfg.fw_table, handle);
			*replaced = true;
		}
	}
	if (have && !len)
		return 0;
	if (!have) {
		if (ct) {
			eol = strchr(ct, '\n');
			h = strstr(ct, "# handle ");
			if (h && (!eol || h < eol))
				snprintf(pos, sizeof(pos), "position %lu ", strtoul(h + 9, NULL, 10));
		}
		snprintf(script + len, sizeof(script) - len,
			 "insert rule inet %s forward %smeta nfproto ipv6 meta mark & 0x%08lx == "
			 "0x%08lx accept " RULE_TAG "\n", cfg.fw_table, pos, cfg.mark, cfg.mark);
	}
	return nft(argv, script, NULL, 0) ? -1 : 1;
}

static void fw_check(void)
{
	static char buf[65536];
	static uint64_t last_log;
	static bool inserted_before;
	uint64_t now = mono_ms(), wait = cfg.interval * 1000;
	/* can the datapath accept what conntrack no longer sees? */
	bool works = gauge.attached, ok = true, have, replaced;
	int r;

	dirty = false;
	due = 0;

	if (fw_chain_list(buf, sizeof(buf))) {
		/* no chain rejects anything either: nothing to fall back from */
		ok = false;
		if (log_ok(&last_log))
			logmsg(LOG_ERR, "no chain forward in table inet %s: nothing accepts the "
			       "marked packets (is the firewall running?)", cfg.fw_table);
	} else {
		r = fw_rules_fix(buf, &replaced);
		if (r < 0) {
			ok = works = false;
			if (log_ok(&last_log))
				logmsg(LOG_ERR, "could not add the accept rule to inet %s forward",
				       cfg.fw_table);
		} else if (r > 0) {
			cnt.fw_repaired++;
			if (replaced)
				logmsg(LOG_WARNING, "the accept rule in inet %s forward named another "
				       "mark: replaced by one for 0x%08lx. Every firewall reload "
				       "brings it back and rejects the replies until this is done "
				       "again: write the mark into " RULE_FILE, cfg.fw_table, cfg.mark);
			else
				logmsg(inserted_before ? LOG_WARNING : LOG_NOTICE,
				       "accept rule for mark 0x%08lx added to inet %s forward%s",
				       cfg.mark, cfg.fw_table, inserted_before ?
				       " again: something removed it (a firewall reload without the "
				       "package's include?)" : "");
			inserted_before = true;
		}
	}

	if (works) {
		broken_since = 0;
		if (!works_since)
			works_since = now;
	} else {
		works_since = 0;
		if (!broken_since)
			broken_since = now;
	}

	have = own_table_present();
	if (works && !have) {
		/* not before the programs are on the uplink, and learning for one
		 * interval: without them nothing would be accepted any more */
		if (now - works_since < wait) {
			due = works_since + wait;
		} else if (own_table_install()) {
			if (log_ok(&last_log))
				logmsg(LOG_ERR, "could not install table inet " OWN_TABLE
				       " (kmod-nft-fib?)");
		} else {
			have = true;
			logmsg(LOG_NOTICE, "table inet " OWN_TABLE " installed: forwarded IPv6 "
			       "bypasses conntrack");
		}
	} else if (!works && have) {
		/* not at once: netifd creating the uplink anew is over in a moment,
		 * and the flows would have to be learned again */
		if (now - broken_since < wait) {
			due = broken_since + wait;
		} else if (!own_table_remove()) {
			have = false;
			logmsg(LOG_ERR, "%s for %lu s: table inet " OWN_TABLE " removed, falling back "
			       "to conntrack (flows on a symmetric path only) until that is over",
			       gauge.attached ? "no accept rule in the firewall" :
			       "the programs are not on the uplink", cfg.interval);
		}
	}
	if (!works || !have)
		ok = false;
	/* what failed is tried again, without waiting for a change */
	if (!ok && !due)
		due = now + wait;
	gauge.fw_ok = ok;
	logmsg(LOG_DEBUG, "rules: looked, %s", ok ? "in place" : "not in place");
}

/*
 * The daemon cannot start. Programs of an earlier run may still be on the
 * uplink, but nobody would put them back when they go: forwarded IPv6 is
 * conntrack's again.
 */
void fw_fallback(void)
{
	if (own_table_present() && !own_table_remove())
		logmsg(LOG_ERR, "table inet " OWN_TABLE " removed: falling back to conntrack "
		       "(flows on a symmetric path only)");
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

/*
 * Main loop, every iteration: look once a change of the ruleset has settled,
 * when the programs came or went, and when a step is due. Our own changes
 * notify us too and cost one more look, which finds nothing to do: dropping
 * those notifications unseen would drop a foreign change that came with them.
 * Without notifications: every interval.
 */
void fw_tick(void)
{
	static uint64_t last;
	static bool was_attached;
	uint64_t now = mono_ms();

	if ((dirty && now - dirty_at >= SETTLE_MS) || gauge.attached != was_attached ||
	    (due && now >= due) || (gen_fd < 0 && now - last >= cfg.interval * 1000)) {
		last = now;
		was_attached = gauge.attached;
		fw_check();
	}
}

/* the detach command: our table and the accept rule go */
void fw_remove(void)
{
	char *argv[] = { "nft", "-f", "-", NULL };
	static char buf[65536];
	char script[1024];
	const char *p = buf;
	unsigned long handle;
	size_t len = 0;
	bool ours;

	own_table_remove();
	if (fw_chain_list(buf, sizeof(buf)))
		return;
	while ((p = rule_next(buf, p, &handle, &ours)) != NULL)
		if (handle && len < sizeof(script) - 128)
			len += snprintf(script + len, sizeof(script) - len,
					"delete rule inet %s forward handle %lu\n", cfg.fw_table, handle);
	if (len)
		nft(argv, script, NULL, 0);
}
