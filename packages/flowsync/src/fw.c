// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
/*
 * The nftables rules the datapath needs, and keeping them in place.
 *
 * 1. Our own table (ip6 flowsync): two notrack rules at prerouting.
 *    - A packet the ingress program marked is untracked: its flow is in the
 *      tables, the firewall accepts it on the mark.
 *    - A packet routed out of the uplink is untracked while the uplink's
 *      interface index is in the set `alive`. The element lives
 *      alive_timeout seconds; the daemon refreshes it every third of that,
 *      each time after seeing both programs on the uplink, and deletes it
 *      the moment it finds them gone. Without the daemon, or without the
 *      programs, the element expires and conntrack tracks the forwarded
 *      flows as it did before flowsync: replies of a flow it saw leave pass
 *      on `ct state established`. An expired element never rejects
 *      anything, it only tracks; so the switch is tight and nothing waits.
 * 2. One rule in the firewall's forward chain: accept what the ingress
 *    program marked. An accept in a table of our own would not do, the
 *    firewall's chain still sees the packet and rejects it. The package
 *    ships the rule as an fw4 include (it is there from the first ruleset
 *    on, and after every reload); here it is checked and, if it is missing
 *    or names another mark than ours, put right.
 *
 * Both are looked at when the ruleset changes (a netlink notification that
 * is not our own element refresh), when the programs come or go, and every
 * interval while something is wrong or there are no notifications. The
 * table and the rule are read and written with the nft tool; the element is
 * written over netlink by the daemon itself, without a fork, every few
 * seconds.
 */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flowsync.h"

#define OWN_TABLE	NFNL_TABLE
#define ALIVE_SET	NFNL_ALIVE_SET
#define RULE_TAG	"comment \"flowsync\""
#define RULE_FILE	"/usr/share/nftables.d/chain-pre/forward/10-flowsync.nft"
#define SETTLE_MS	500
#define ACK_WAIT_MS	200

static int gen_fd = -1;		/* ruleset change notifications */
static int alive_fd = -1;	/* our element writes */
static uint32_t alive_port;	/* that socket's port id: our own notifications */
static bool dirty = true;
static uint64_t dirty_at;
static uint64_t retry;		/* a failed write is tried again then; 0: none */
static uint64_t alive_at;	/* the element was last written */
static bool alive_set = true;	/* ... and not deleted since; an earlier run may have
				 * left one: taken away at the first look without programs */
static unsigned int alive_ifindex;	/* the index it was written for */

/* run nft; script goes to its stdin if given, its stdout into out (-1 if it
 * does not fit: a listing cut short would be acted on). Without out, nft's
 * stderr is kept and logged with a failure, so that "could not install" says
 * why. Returns its exit status, -1 if it could not be run. */
static int nft(char *const argv[], const char *script, char *out, size_t outlen)
{
	int in[2] = { -1, -1 }, outp[2] = { -1, -1 }, status, null;
	static char errbuf[512];
	bool truncated = false, errs = !out;
	sigset_t none;
	size_t len = 0;
	ssize_t n;
	pid_t pid;

	if (errs) {
		out = errbuf;
		outlen = sizeof(errbuf);
	}
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
		if (errs)
			dup2(outp[1], 2);
		else if (null >= 0)
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
		n = len + 1 < outlen ? read(outp[0], out + len, outlen - 1 - len) :
				       read(outp[0], skip, sizeof(skip));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (len + 1 < outlen) {
			len += n;
			out[len] = 0;
		} else {
			truncated = true;
		}
	}
	close(outp[0]);
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (errs && status) {
		while (len && (out[len - 1] == '\n' || out[len - 1] == ' '))
			out[--len] = 0;
		logmsg(LOG_WARNING, "nft %s: exit %d%s%s", argv[1], status, len ? ": " : "",
		       len ? out : "");
	}
	if (!errs && truncated) {
		static uint64_t last_log;

		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "nft %s: listing longer than %zu bytes, not used",
			       argv[1], outlen);
		return -1;
	}
	return status;
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

/* is the table there, with the rules this daemon's configuration calls for?
 * 0: yes; 1: there, but a rule differs (another mark, an older layout); -1:
 * no table */
static int own_table_state(void)
{
	char *argv[] = { "nft", "-n", "list", "table", "ip6", OWN_TABLE, NULL };
	static char buf[8192];
	char want[64];

	if (nft(argv, NULL, buf, sizeof(buf)))
		return -1;
	snprintf(want, sizeof(want), "meta mark & 0x%08lx == 0x%08lx notrack accept", cfg.mark,
		 cfg.mark);
	return strstr(buf, want) && strstr(buf, "fib daddr oif @" ALIVE_SET " notrack") ? 0 : 1;
}

/*
 * The table, created or replaced in one transaction (add on an existing
 * table is nothing, delete takes it away, the definition brings it back:
 * there is no moment without it). With the programs on the uplink the
 * element comes along, so that a replace never drops the heartbeat. The
 * second chain is never run: a conntrack expression anywhere in the ruleset
 * keeps the kernel's defragmentation hooked in, which the marks of
 * fragmented packets rely on (see bpf/flowsync.bpf.c).
 */
static int own_table_apply(void)
{
	char *argv[] = { "nft", "-f", "-", NULL };
	char script[1024], elem[96] = "";
	unsigned int ifindex = gauge.attached ? dp_ifindex() : 0;

	if (ifindex)
		snprintf(elem, sizeof(elem), "\t\telements = { %u timeout %lus expires %lus }\n",
			 ifindex, cfg.alive_timeout, cfg.alive_timeout);
	snprintf(script, sizeof(script),
		 "add table ip6 " OWN_TABLE "\n"
		 "delete table ip6 " OWN_TABLE "\n"
		 "table ip6 " OWN_TABLE " {\n"
		 "	set " ALIVE_SET " {\n"
		 "		type iface_index\n"
		 "		flags timeout\n"
		 "%s"
		 "	}\n"
		 "	chain prerouting {\n"
		 "		type filter hook prerouting priority raw; policy accept;\n"
		 "		meta mark & 0x%08lx == 0x%08lx notrack accept\n"
		 "		fib daddr oif @" ALIVE_SET " notrack\n"
		 "	}\n"
		 "	chain defrag {\n"
		 "		ct state untracked accept\n"
		 "	}\n"
		 "}\n", elem, cfg.mark, cfg.mark);
	if (nft(argv, script, NULL, 0))
		return -1;
	alive_set = ifindex != 0;
	if (ifindex) {
		alive_at = mono_ms();
		alive_ifindex = ifindex;
	}
	return 0;
}

static int own_table_remove(void)
{
	char *argv[] = { "nft", "delete", "table", "ip6", OWN_TABLE, NULL };

	return nft(argv, NULL, NULL, 0);
}

/* write the element (add) or take it away; 0, or -errno */
static int alive_write(bool add, unsigned int ifindex)
{
	static uint32_t seq = 1;
	char buf[512];
	struct nlmsghdr *h;
	struct nlmsgerr *e;
	struct pollfd pfd = { .fd = alive_fd, .events = POLLIN };
	size_t len;
	ssize_t n;
	int err = 0;

	if (alive_fd < 0)
		return -EBADF;
	uint32_t first = seq, last;

	len = nfnl_alive_msg(buf, sizeof(buf), add, ifindex, cfg.alive_timeout, seq, alive_port);
	last = seq + NFNL_ALIVE_MSGS - 1;
	seq += NFNL_ALIVE_MSGS;
	if (!len)
		return -EMSGSIZE;
	/* an ack that came after an earlier write timed out is not this one's */
	while (recv(alive_fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
	if (send(alive_fd, buf, len, 0) < 0)
		return -errno;
	/* the ack of the one message that asked for it, or an error of any
	 * message of this batch */
	for (;;) {
		if (poll(&pfd, 1, ACK_WAIT_MS) <= 0)
			return -ETIMEDOUT;
		n = recv(alive_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}
		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type != NLMSG_ERROR || h->nlmsg_seq < first ||
			    h->nlmsg_seq > last)
				continue;
			e = NLMSG_DATA(h);
			err = e->error;
			if (err || h->nlmsg_seq == first + NFNL_ALIVE_ACKED(add))
				return err;
		}
	}
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
	char *argv[] = { "nft", "-na", "list", "chain", "inet", (char *)cfg.fw_table, "forward", NULL };

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

/* the rule and the table: there, or put there; what fails is tried again
 * after an interval, without waiting for a change */
static void fw_check(void)
{
	static char buf[65536];
	static uint64_t last_log;
	static bool inserted_before, table_before;
	uint64_t now = mono_ms();
	bool ok = true, replaced;
	int r;

	dirty = false;
	retry = 0;

	if (fw_chain_list(buf, sizeof(buf))) {
		ok = false;
		if (log_ok(&last_log))
			logmsg(LOG_ERR, "no chain forward in table inet %s: nothing accepts the "
			       "marked packets (is the firewall running?)", cfg.fw_table);
	} else {
		r = fw_rules_fix(buf, &replaced);
		if (r < 0) {
			ok = false;
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

	r = own_table_state();
	if (r == 0) {
		table_before = true;
	} else {
		if (own_table_apply()) {
			ok = false;
			if (log_ok(&last_log))
				logmsg(LOG_ERR, "could not install table ip6 " OWN_TABLE
				       " (kmod-nft-fib?)");
		} else {
			cnt.fw_repaired++;
			if (r > 0)
				logmsg(LOG_WARNING, "table ip6 " OWN_TABLE " had rules for another "
				       "mark or an older layout (a previous run's): replaced by "
				       "the rules for mark 0x%08lx", cfg.mark);
			else
				logmsg(table_before ? LOG_WARNING : LOG_NOTICE,
				       "table ip6 " OWN_TABLE " %s: forwarded IPv6 bypasses "
				       "conntrack while the programs are on the uplink",
				       table_before ? "was gone, installed again (something "
				       "deleted it: conntrack tracked forwarded IPv6 meanwhile)" :
				       "installed");
			table_before = true;
		}
	}
	if (!ok)
		retry = now + cfg.interval * 1000;
	gauge.fw_ok = ok;
	logmsg(LOG_DEBUG, "rules: looked, %s", ok ? "in place" : "not in place");
}

/* ruleset change notifications, and the socket for the element */
int fw_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
				  .nl_groups = 1u << (NFNLGRP_NFTABLES - 1) };
	struct sockaddr_nl me = { .nl_family = AF_NETLINK };
	socklen_t mlen = sizeof(me);

	alive_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
	if (alive_fd < 0 || bind(alive_fd, (struct sockaddr *)&me, sizeof(me)) ||
	    getsockname(alive_fd, (struct sockaddr *)&me, &mlen)) {
		logmsg(LOG_ERR, "nftables socket: %s", strerror(errno));
		return -1;
	}
	alive_port = me.nl_pid;

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

/*
 * Something changed the ruleset. Our own element writes notify us too, three
 * times every few seconds; they carry our port id and are not a change to
 * look at. What the nft tool wrote for us is, like anybody's change: the
 * look finds nothing to do. An overrun is a change: something may be lost.
 */
void fw_handle(void)
{
	char buf[8192];
	struct nlmsghdr *h;
	bool foreign = false;
	ssize_t n;

	for (;;) {
		n = recv(gen_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == ENOBUFS) {
				foreign = true;
				continue;
			}
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n))
			if (h->nlmsg_pid != alive_port)
				foreign = true;
	}
	if (foreign && !dirty) {
		dirty = true;
		dirty_at = mono_ms();
	}
}

/*
 * Main loop, every iteration: look once a change of the ruleset has settled,
 * when the programs came or went, and when a failed write is due again.
 * Without notifications: every interval.
 */
void fw_tick(void)
{
	static uint64_t last;
	static bool was_attached;
	uint64_t now = mono_ms();

	if ((dirty && now - dirty_at >= SETTLE_MS) || gauge.attached != was_attached ||
	    (retry && now >= retry) || (gen_fd < 0 && now - last >= cfg.interval * 1000)) {
		last = now;
		was_attached = gauge.attached;
		fw_check();
	}
}

/*
 * Main loop, every iteration: the heartbeat. While the rules are in place,
 * every third of alive_timeout: look at the uplink (dp_tick: cheap while the
 * programs are there) and, if both programs are on it, write the element.
 * The moment they are not, take the element away; the next refresh after
 * they are back puts it there again.
 */
void fw_alive_tick(void)
{
	static uint64_t last_log;
	uint64_t now = mono_ms(), refresh = cfg.alive_timeout * 1000 / 3;
	int err;

	if (gauge.fw_ok && (!alive_set || now - alive_at >= refresh)) {
		dp_tick();
		if (gauge.attached) {
			alive_ifindex = dp_ifindex();
			err = alive_write(true, alive_ifindex);
			if (err) {
				if (log_ok(&last_log))
					logmsg(LOG_WARNING, "alive element: %s", strerror(-err));
				/* not written: let it expire rather than pretend */
				alive_set = false;
			} else {
				alive_at = now;
				if (!alive_set)
					logmsg(LOG_NOTICE, "uplink %s (ifindex %u) alive: forwarded "
					       "IPv6 bypasses conntrack", cfg.uplink, dp_ifindex());
				alive_set = true;
			}
		}
	}
	if (alive_set && (!gauge.attached || !gauge.fw_ok)) {
		/* an element of an earlier run is the current device's, unless the
		 * device went with it: then there is nothing to delete */
		if (!alive_ifindex)
			alive_ifindex = if_nametoindex(cfg.uplink);
		err = alive_write(false, alive_ifindex);
		if (err && err != -ENOENT && log_ok(&last_log))
			logmsg(LOG_WARNING, "alive element: delete: %s", strerror(-err));
		alive_set = false;
		logmsg(LOG_NOTICE, "%s: alive element removed, conntrack tracks forwarded IPv6 "
		       "until the programs are back", gauge.attached ? "rules not in place" :
		       "the programs are not on the uplink");
	}
	gauge.alive = alive_set;
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
