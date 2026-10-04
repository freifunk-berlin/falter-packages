// SPDX-License-Identifier: GPL-2.0-only
/*
 * conntrack injection over netlink. Records are batched; the kernel processes
 * a batch synchronously in sendto() and, with NETLINK_CAP_ACK, reports only
 * the failures, each carrying the sequence number of its message. A small
 * ring maps sequence numbers back to the record so the outcome can be
 * attributed: an EEXIST on a create means the entry is not ours and must not
 * be refreshed, an ENOENT on a refresh means our copy is gone and is
 * re-created at once.
 *
 * Reversed entries. An EEXIST is usually a native entry of the same flow,
 * which must stay untouched. But a reply segment that reached this gateway
 * before the copy, and was let through by a stateless rule (the gateways
 * accept TCP segments with ACK at a limited rate), is picked up by the kernel
 * as a connection of its own in the reverse direction: server -> client,
 * unreplied, unmarked. It blocks the copy for as long as the server keeps
 * sending, and every one of its packets is "ct state new". So after an
 * EEXIST the entry is looked up (once per element_timeout and tuple); if it
 * is exactly that reversed, unreplied, unmarked entry, it is deleted by its
 * id (the kernel refuses if it changed meanwhile) and the copy is created in
 * the same batch (inject_replaced). Its packets were only passing a stateless
 * rule; on the copy they are established.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nf_conntrack_common.h>

#include "flowsync.h"

static struct mnl_socket *inj_nl;
static unsigned int inj_portid;
static uint32_t inj_seq;
static char inj_buf[INJECT_BATCH + 4096] __attribute__((aligned(8)));
static struct mnl_nlmsg_batch *inj_batch;
static unsigned int batch_msgs, batch_create, batch_refresh;
static uint32_t batch_first_seq;

static struct {
	uint32_t seq;
	struct rx_ent *e;
	struct flow f;
	uint8_t kind;
} ring[INJ_RING];

static struct flow retry[INJ_RETRY];
static unsigned int n_retry;

/* EEXIST tuples to look up, and reversed entries to replace */
static struct flow check[INJ_RETRY];
static unsigned int n_check;
static struct {
	struct flow f;		/* our flow */
	struct flow orig;	/* the reversed entry's original tuple */
	uint32_t id;
} replace[INJ_RETRY];
static unsigned int n_replace;

int inject_fd(void)
{
	return inj_nl ? mnl_socket_get_fd(inj_nl) : -1;
}

unsigned int inject_portid(void)
{
	return inj_portid;
}

int inj_open(void)
{
	int fd, one = 1;

	inj_nl = mnl_socket_open(NETLINK_NETFILTER);
	if (!inj_nl || mnl_socket_bind(inj_nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		logmsg(LOG_ERR, "conntrack netlink socket: %s", strerror(errno));
		return -1;
	}
	inj_portid = mnl_socket_get_portid(inj_nl);
	fd = mnl_socket_get_fd(inj_nl);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* error reports without the echoed request */
	mnl_socket_setsockopt(inj_nl, NETLINK_CAP_ACK, &one, sizeof(one));
	set_rcvbuf(fd, "conntrack injection", 1 << 20);

	inj_batch = mnl_nlmsg_batch_start(inj_buf, INJECT_BATCH);
	if (!inj_batch) {
		logmsg(LOG_ERR, "out of memory");
		return -1;
	}
	return 0;
}

/*
 * Book the kernel's answer for one message. Successes are counted at send
 * time; this takes them back where needed and keeps the ownership flag of the
 * record honest.
 */
enum inj_acct inj_account(enum inj_kind kind, int err, struct rx_ent *e)
{
	if (kind == INJ_CREATE) {
		if (cnt.inject_created)
			cnt.inject_created--;
		if (e)
			e->own = false;
		if (err == -EEXIST) {
			/* an entry we did not create: native, or a copy from before a
			 * restart. Left untouched; the dump re-learns ownership. */
			cnt.inject_exists++;
			return ACCT_OK;
		}
		cnt.inject_errors++;
		return ACCT_ERROR;
	}
	/* refresh of our own copy */
	if (err == -EBUSY) {
		/* the copy is ASSURED: timeout refreshed, status left alone */
		return ACCT_OK;
	}
	if (cnt.inject_refreshed)
		cnt.inject_refreshed--;
	if (e)
		e->own = false;
	if (err == -ENOENT) {
		/* our copy is gone (expired, flushed, evicted): create it again */
		cnt.inject_gone++;
		return ACCT_RETRY;
	}
	cnt.inject_errors++;
	return ACCT_ERROR;
}

static void note_error(uint32_t seq, int err)
{
	static uint64_t last_log;
	struct rx_ent *e = NULL;
	enum inj_kind kind = INJ_CREATE;
	unsigned int i = seq & (INJ_RING - 1);
	uint32_t now;

	if (ring[i].seq == seq) {
		e = ring[i].e;
		kind = ring[i].kind;
		ring[i].seq = 0;
		/* the slot may have been taken by another tuple meanwhile */
		if (e && memcmp(&e->f, &ring[i].f, sizeof(e->f)))
			e = NULL;
	}
	/* a lookup found nothing or a delete found the entry changed: nothing to do */
	if (kind == INJ_CHECK || kind == INJ_DELETE)
		return;
	if (kind == INJ_CREATE && err == -EEXIST && e && n_check < INJ_RETRY) {
		now = now_s();
		if (!e->checked_at || now - e->checked_at >= cfg.element_timeout) {
			e->checked_at = now;
			check[n_check++] = e->f;
		}
	}
	switch (inj_account(kind, err, e)) {
	case ACCT_RETRY:
		if (e && n_retry < INJ_RETRY)
			retry[n_retry++] = e->f;
		break;
	case ACCT_ERROR:
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "inject: %s: %s", kind == INJ_CREATE ? "create" : "refresh",
			       strerror(-err));
		break;
	default:
		break;
	}
}

/* the answer to a lookup after EEXIST: is it the reversed pickup? */
static void note_entry(const struct nlmsghdr *nlh)
{
	unsigned int i = nlh->nlmsg_seq & (INJ_RING - 1);
	struct ct_entry c;
	struct flow rev;

	if (ring[i].seq != nlh->nlmsg_seq || ring[i].kind != INJ_CHECK)
		return;
	ring[i].seq = 0;
	if (ct_parse(nlh, &c) != 0 || !c.id)
		return;
	flow_reverse(&rev, &ring[i].f);
	if (memcmp(&c.f, &rev, sizeof(rev)) || (c.status & IPS_SEEN_REPLY) || is_copy(c.mark) ||
	    n_replace == INJ_RETRY)
		return;
	replace[n_replace].f = ring[i].f;
	replace[n_replace].orig = c.f;
	replace[n_replace].id = c.id;
	n_replace++;
}

/* collect error reports (without NLM_F_ACK the kernel only reports failures)
 * and the answers to lookups */
void inj_drain(void)
{
	static char buf[NL_BUF_SIZE] __attribute__((aligned(8)));
	static uint64_t last_log;
	const struct nlmsghdr *nlh;
	const struct nlmsgerr *err;
	ssize_t n;
	int len;

	for (;;) {
		n = mnl_socket_recvfrom(inj_nl, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN && errno != EWOULDBLOCK && log_ok(&last_log))
				logmsg(LOG_WARNING, "inject: reading errors: %s", strerror(errno));
			return;
		}
		len = n;
		for (nlh = (struct nlmsghdr *)buf; mnl_nlmsg_ok(nlh, len);
		     nlh = mnl_nlmsg_next(nlh, &len)) {
			if (nlh->nlmsg_type == ((NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_NEW)) {
				note_entry(nlh);
				continue;
			}
			if (nlh->nlmsg_type != NLMSG_ERROR)
				continue;
			err = mnl_nlmsg_get_payload(nlh);
			if (err->error)
				note_error(nlh->nlmsg_seq, err->error);
		}
	}
}

static void inj_send(void)
{
	static uint64_t last_log;
	uint32_t seq;

	if (!batch_msgs)
		return;
	DBG("inject: batch of %u entries (%u create, %u refresh)", batch_msgs, batch_create,
	    batch_refresh);
	if (mnl_socket_sendto(inj_nl, mnl_nlmsg_batch_head(inj_batch),
			      mnl_nlmsg_batch_size(inj_batch)) < 0) {
		cnt.inject_errors += batch_msgs;
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "inject: send: %s", strerror(errno));
		/* nothing was created: take the tentative ownership back */
		for (seq = batch_first_seq; seq != batch_first_seq + batch_msgs; seq++) {
			unsigned int i = seq & (INJ_RING - 1);

			if (ring[i].seq == seq && ring[i].e && ring[i].kind == INJ_CREATE)
				ring[i].e->own = false;
		}
	} else {
		/* the kernel processes the batch synchronously in sendto(), the
		 * error reports are already queued */
		cnt.inject_created += batch_create;
		cnt.inject_refreshed += batch_refresh;
		inj_drain();
	}
	batch_msgs = batch_create = batch_refresh = 0;
}

static void inj_commit(struct nlmsghdr *nlh, const struct flow *f, struct rx_ent *e,
		       enum inj_kind kind)
{
	unsigned int i;

	nlh->nlmsg_seq = ++inj_seq;
	i = inj_seq & (INJ_RING - 1);
	ring[i].seq = inj_seq;
	ring[i].e = e;
	ring[i].f = *f;
	ring[i].kind = kind;
	if (!batch_msgs)
		batch_first_seq = inj_seq;

	if (mnl_nlmsg_batch_next(inj_batch)) {
		batch_msgs++;
		if (kind == INJ_CREATE)
			batch_create++;
		else if (kind == INJ_REFRESH)
			batch_refresh++;
	} else {
		/* batch full: send it, the overflowing message moves to the front */
		inj_send();
		mnl_nlmsg_batch_reset(inj_batch);
		batch_msgs = 1;
		batch_create = kind == INJ_CREATE;
		batch_refresh = kind == INJ_REFRESH;
		batch_first_seq = inj_seq;
	}
}

void inj_add(const struct flow *f, struct rx_ent *e, enum inj_kind kind)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(inj_batch));

	ct_build_new(nlh, f, kind == INJ_CREATE);
	inj_commit(nlh, f, e, kind);
}

static void inj_add_get(const struct flow *f)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(inj_batch));

	ct_build_get(nlh, f);
	inj_commit(nlh, f, NULL, INJ_CHECK);
}

static void inj_add_delete(const struct flow *orig, uint32_t id)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(mnl_nlmsg_batch_current(inj_batch));

	ct_build_delete(nlh, orig, id);
	inj_commit(nlh, orig, NULL, INJ_DELETE);
}

/* look up the EEXIST tuples, then replace the reversed entries among them */
static void inj_resolve(void)
{
	unsigned int i, n;

	if (!n_check)
		return;
	n = n_check;
	n_check = 0;
	for (i = 0; i < n; i++)
		inj_add_get(&check[i]);
	inj_send();
	mnl_nlmsg_batch_reset(inj_batch);
	n_check = 0;	/* creates below may report EEXIST again: next time */
	n = n_replace;
	n_replace = 0;
	for (i = 0; i < n; i++) {
		struct rx_ent *e = rx_find(&replace[i].f);

		inj_add_delete(&replace[i].orig, replace[i].id);
		if (e)
			e->own = true;	/* tentative, like any create */
		inj_add(&replace[i].f, e, INJ_CREATE);
		cnt.inject_replaced++;
	}
	inj_send();
	mnl_nlmsg_batch_reset(inj_batch);
	n_check = 0;
}

/* send what is queued, then re-create the copies the kernel reported gone */
void inj_flush(void)
{
	static struct flow again[INJ_RETRY];	/* a whole receive pass, ~87 KiB */
	unsigned int i, n;

	inj_send();
	mnl_nlmsg_batch_reset(inj_batch);
	if (!n_retry) {
		inj_resolve();
		return;
	}
	n = n_retry;
	memcpy(again, retry, n * sizeof(*again));
	n_retry = 0;
	for (i = 0; i < n; i++) {
		struct rx_ent *e = rx_find(&again[i]);

		if (e)
			e->own = true;	/* tentative, like any create */
		inj_add(&again[i], e, INJ_CREATE);
	}
	inj_send();
	mnl_nlmsg_batch_reset(inj_batch);
	n_retry = 0;	/* creates cannot ask for another round */
	inj_resolve();
}
