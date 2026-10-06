// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
/*
 * The datapath: the tc programs on the uplink, their maps and the new-flow
 * events (bpf/flowsync.bpf.c, dp.h).
 *
 * The maps are pinned and the programs stay attached when the daemon exits:
 * flows on a symmetric path, and peers' flows until they expire, keep passing
 * while it is down or restarts. A restart loads the programs anew (with the
 * current configuration), reuses the maps and replaces the filters in place.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "flowsync.h"
#include "dp.h"

/* our filters on the uplink's clsact qdisc */
#define TC_PRIO		3780
#define TC_HANDLE	1
/* after a failed attach: a notification starts the next attempt this much
 * later */
#define ATTACH_RETRY_MS	1000

static struct bpf_object *obj;
static int map_fd[2] = { -1, -1 };
static int stats_fd = -1;
static int ctl_fd = -1;
static int prog_fd[2] = { -1, -1 };	/* ingress, egress */
static __u32 prog_id[2];
static struct ring_buffer *rb;
static unsigned int attached_ifindex;
static unsigned int last_ifindex;	/* the device we were attached to before it went */
static struct fs_cfg dcfg;

/* the remote entries of one receive pass, written with one system call */
#define ADD_MAX		(DRAIN_MAX * WIRE_MAX_RECORDS)
static struct fs_key add_key[ADD_MAX];
static struct fs_remote add_val[ADD_MAX];
static unsigned int add_n;

struct dp_walk {
	enum dp_map which;
	__u32 token;
	bool first;
	uint32_t now;
};

/* the programs' clock: bpf_ktime_get_coarse_ns() in seconds */
uint32_t dp_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	return ts.tv_sec;
}

static void key_of(struct fs_key *k, const struct flow *f)
{
	memset(k, 0, sizeof(*k));
	memcpy(k->c, &f->c, 16);
	memcpy(k->s, &f->s, 16);
	k->cport = htons(f->cport);
	k->sport = htons(f->sport);
	k->proto = f->proto;
}

static void flow_of(struct flow *f, const struct fs_key *k)
{
	memset(f, 0, sizeof(*f));
	memcpy(&f->c, k->c, 16);
	memcpy(&f->s, k->s, 16);
	f->cport = ntohs(k->cport);
	f->sport = ntohs(k->sport);
	f->proto = k->proto;
}

static void ent_local(struct dp_ent *e, const struct fs_local *v, uint32_t now)
{
	uint32_t ttl = fs_ttl(&dcfg, e->f.proto, v->flags);

	e->flags = v->flags;
	e->age = fs_age(now, v->seen);
	e->left = e->age > ttl ? 0 : ttl - e->age + 1;
}

static void ent_remote(struct dp_ent *e, const struct fs_remote *v, uint32_t now)
{
	e->flags = 0;
	e->age = 0;
	e->left = (int32_t)(v->expires - now) > 0 ? v->expires - now : 0;
}

/* set around calls whose failure is expected and handled (a qdisc that
 * exists, a filter that is gone): libbpf prints the kernel's message for
 * those too */
static bool quiet;

static int print_cb(enum libbpf_print_level level, const char *fmt, va_list ap)
{
	char buf[512];
	size_t len;

	if (level == LIBBPF_DEBUG || quiet)
		return 0;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	len = strlen(buf);
	if (len && buf[len - 1] == '\n')
		buf[len - 1] = 0;
	logmsg(level == LIBBPF_WARN ? LOG_WARNING : LOG_INFO, "%s", buf);
	return 0;
}

static void pin_path(char *buf, size_t len, const char *name)
{
	snprintf(buf, len, "%s/%s", cfg.pin_dir, name);
}

static void unpin_all(void)
{
	static const char *const names[] = { "fs_local", "fs_remote", "fs_stats", "fs_ctl" };
	char path[PATH_MAX];
	unsigned int i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		pin_path(path, sizeof(path), names[i]);
		unlink(path);
	}
	rmdir(cfg.pin_dir);
}

static void set_cfg(void)
{
	dcfg.mark = cfg.mark;
	dcfg.t_udp = cfg.t_udp;
	dcfg.t_tcp_syn = cfg.t_tcp_syn;
	dcfg.t_tcp_est = cfg.t_tcp;
	dcfg.t_tcp_close = cfg.t_tcp_close;
	dcfg.t_other = cfg.t_other;
}

/*
 * A pinned map the object cannot take over, because it was made with another
 * size (max_flows, max_remote changed) or by another version of the object:
 * libbpf would refuse to load. It is unpinned and the load makes a new one;
 * what it held is lost (own flows come back with their next packet out, the
 * peers' with the resync). Only this map: every other failure to load leaves
 * the pins alone.
 */
static void drop_unusable_pin(const struct bpf_map *m)
{
	const char *path = bpf_map__pin_path(m);
	struct bpf_map_info info;
	__u32 ilen = sizeof(info);
	int fd;

	if (!path)
		return;
	fd = bpf_obj_get(path);
	if (fd < 0)
		return;
	memset(&info, 0, sizeof(info));
	if (!bpf_map_get_info_by_fd(fd, &info, &ilen) &&
	    (info.type != bpf_map__type(m) || info.key_size != bpf_map__key_size(m) ||
	     info.value_size != bpf_map__value_size(m) ||
	     info.max_entries != bpf_map__max_entries(m) ||
	     info.map_flags != bpf_map__map_flags(m))) {
		logmsg(LOG_WARNING, "%s was pinned with another size or layout (%u entries, now "
		       "%u): made anew, what it held is dropped", bpf_map__name(m),
		       info.max_entries, bpf_map__max_entries(m));
		unlink(path);
	}
	close(fd);
}

static int load(void)
{
	LIBBPF_OPTS(bpf_object_open_opts, opts, .pin_root_path = cfg.pin_dir);
	struct bpf_program *p;
	struct bpf_map *m;
	const char *name;
	size_t len;
	int err;

	obj = bpf_object__open_file(cfg.bpf_object, &opts);
	if (!obj) {
		logmsg(LOG_ERR, "%s: %s", cfg.bpf_object, strerror(errno));
		return -1;
	}
	bpf_object__for_each_map(m, obj) {
		name = bpf_map__name(m);
		len = strlen(name);
		if (!strcmp(name, "fs_local"))
			bpf_map__set_max_entries(m, cfg.max_flows);
		else if (!strcmp(name, "fs_remote"))
			bpf_map__set_max_entries(m, cfg.max_remote);
		else if (len >= 7 && !strcmp(name + len - 7, ".rodata") &&
			 bpf_map__set_initial_value(m, &dcfg, sizeof(dcfg))) {
			logmsg(LOG_ERR, "%s: configuration does not fit the object", cfg.bpf_object);
			goto fail;
		}
		drop_unusable_pin(m);
	}
	err = bpf_object__load(obj);
	if (err) {
		logmsg(LOG_ERR, "%s: load: %s", cfg.bpf_object, strerror(-err));
		goto fail;
	}
	map_fd[DP_LOCAL] = bpf_object__find_map_fd_by_name(obj, "fs_local");
	map_fd[DP_REMOTE] = bpf_object__find_map_fd_by_name(obj, "fs_remote");
	stats_fd = bpf_object__find_map_fd_by_name(obj, "fs_stats");
	ctl_fd = bpf_object__find_map_fd_by_name(obj, "fs_ctl");
	p = bpf_object__find_program_by_name(obj, "fs_ingress");
	prog_fd[0] = p ? bpf_program__fd(p) : -1;
	p = bpf_object__find_program_by_name(obj, "fs_egress");
	prog_fd[1] = p ? bpf_program__fd(p) : -1;
	if (map_fd[DP_LOCAL] < 0 || map_fd[DP_REMOTE] < 0 || stats_fd < 0 || ctl_fd < 0 ||
	    prog_fd[0] < 0 || prog_fd[1] < 0) {
		logmsg(LOG_ERR, "%s: maps or programs missing", cfg.bpf_object);
		goto fail;
	}
	return 0;
fail:
	bpf_object__close(obj);
	obj = NULL;
	return -1;
}

static void (*event_cb)(const struct flow *f);

static int on_event(void *ctx, void *data, size_t len)
{
	struct flow f;

	(void)ctx;
	if (len < sizeof(struct fs_key) || !event_cb)
		return 0;
	flow_of(&f, data);
	cnt.ev_recv++;
	event_cb(&f);
	return 0;
}

/*
 * load: the daemon. Loads the programs, creating the maps or taking the
 * pinned ones. A map pinned with another size (max_flows or max_remote
 * changed) cannot be reused: it is dropped and made anew, with what it held.
 * Without load (the flow commands): open the pinned maps of a running or
 * stopped daemon.
 */
int dp_open(bool load_progs)
{
	struct bpf_prog_info info;
	char path[PATH_MAX];
	__u32 ilen;
	int i;

	set_cfg();
	libbpf_set_print(print_cb);
	if (!load_progs) {
		pin_path(path, sizeof(path), "fs_local");
		map_fd[DP_LOCAL] = bpf_obj_get(path);
		pin_path(path, sizeof(path), "fs_remote");
		map_fd[DP_REMOTE] = bpf_obj_get(path);
		pin_path(path, sizeof(path), "fs_stats");
		stats_fd = bpf_obj_get(path);
		pin_path(path, sizeof(path), "fs_ctl");
		ctl_fd = bpf_obj_get(path);
		return map_fd[DP_LOCAL] < 0 || map_fd[DP_REMOTE] < 0 ? -1 : 0;
	}
	if (mkdir(cfg.pin_dir, 0755) && errno != EEXIST) {
		logmsg(LOG_ERR, "%s: %s (is the BPF file system mounted?)", cfg.pin_dir,
		       strerror(errno));
		return -1;
	}
	if (load()) {
		logmsg(LOG_ERR, "the programs on the uplink and the flow tables in %s stay as "
		       "they are", cfg.pin_dir);
		return -1;
	}
	for (i = 0; i < 2; i++) {
		memset(&info, 0, sizeof(info));
		ilen = sizeof(info);
		if (bpf_prog_get_info_by_fd(prog_fd[i], &info, &ilen)) {
			logmsg(LOG_ERR, "program info: %s", strerror(errno));
			return -1;
		}
		prog_id[i] = info.id;
	}
	rb = ring_buffer__new(bpf_object__find_map_fd_by_name(obj, "fs_events"), on_event, NULL,
			      NULL);
	if (!rb) {
		logmsg(LOG_ERR, "event ring: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static void tc_hook(struct bpf_tc_hook *hook, unsigned int ifindex, int dir)
{
	memset(hook, 0, sizeof(*hook));
	hook->sz = sizeof(*hook);
	hook->ifindex = ifindex;
	hook->attach_point = dir ? BPF_TC_EGRESS : BPF_TC_INGRESS;
}

/* say: log why it fails (the caller tries again and again). Returns 0, -1 if
 * it failed, -2 if it failed after it had put a filter in. */
static int attach(unsigned int ifindex, bool say)
{
	struct bpf_tc_hook hook;
	int dir, err;

	tc_hook(&hook, ifindex, 0);
	hook.attach_point = BPF_TC_INGRESS | BPF_TC_EGRESS;
	quiet = true;
	err = bpf_tc_hook_create(&hook);	/* the clsact qdisc */
	quiet = false;
	if (err && err != -EEXIST) {
		if (say)
			logmsg(LOG_ERR, "%s: clsact qdisc: %s", cfg.uplink, strerror(-err));
		return -1;
	}
	for (dir = 0; dir < 2; dir++) {
		LIBBPF_OPTS(bpf_tc_opts, opts, .handle = TC_HANDLE, .priority = TC_PRIO,
			    .prog_fd = prog_fd[dir], .flags = BPF_TC_F_REPLACE);

		tc_hook(&hook, ifindex, dir);
		quiet = !say;		/* libbpf prints the kernel's reason */
		err = bpf_tc_attach(&hook, &opts);
		quiet = false;
		if (err) {
			if (say)
				logmsg(LOG_ERR, "%s: attach %s: %s", cfg.uplink,
				       dir ? "egress" : "ingress", strerror(-err));
			return dir ? -2 : -1;
		}
	}
	return 0;
}

/* the bypass switch of the running programs; -1 if it cannot be set */
int dp_set_bypass(bool on)
{
	struct fs_ctl ctl = { .bypass = on };
	__u32 zero = 0;

	if (ctl_fd < 0 || bpf_map_update_elem(ctl_fd, &zero, &ctl, BPF_ANY))
		return -1;
	gauge.bypass = on;
	return 0;
}

int dp_get_bypass(void)
{
	struct fs_ctl ctl;
	__u32 zero = 0;

	if (ctl_fd < 0 || bpf_map_lookup_elem(ctl_fd, &zero, &ctl))
		return -1;
	return ctl.bypass != 0;
}

/* the bypass takes an Ethernet header off the packet and writes behind it */
static bool is_ethernet(const char *dev)
{
	struct ifreq ifr;
	bool eth = false;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return false;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", dev);
	if (!ioctl(fd, SIOCGIFHWADDR, &ifr))
		eth = ifr.ifr_hwaddr.sa_family == ARPHRD_ETHER;
	close(fd);
	return eth;
}

/* as configured, but never on a device the bypass cannot work on */
static void apply_bypass(void)
{
	bool on = cfg.bypass;

	if (on && !is_ethernet(cfg.uplink)) {
		logmsg(LOG_WARNING, "bypass: uplink %s is not an Ethernet device, bypass stays off",
		       cfg.uplink);
		on = false;
	}
	if (dp_set_bypass(on))
		logmsg(LOG_WARNING, "bypass: %s", strerror(errno));
	else if (on)
		logmsg(LOG_NOTICE, "bypass on: accepted TCP and UDP packets are forwarded from tc, "
		       "past netfilter");
}

/* are both filters on the device, with our programs? */
static bool attached(unsigned int ifindex)
{
	struct bpf_tc_hook hook;
	int dir, err;

	for (dir = 0; dir < 2; dir++) {
		LIBBPF_OPTS(bpf_tc_opts, opts, .handle = TC_HANDLE, .priority = TC_PRIO);

		tc_hook(&hook, ifindex, dir);
		quiet = true;
		err = bpf_tc_query(&hook, &opts);
		quiet = false;
		if (err || opts.prog_id != prog_id[dir])
			return false;
	}
	return true;
}

/*
 * At startup and every interval: the programs must be on the uplink. The
 * device may not exist yet, may have been created anew under its name (netifd
 * does that to a VLAN on every ifup; the new device has no filters), or
 * somebody removed the qdisc. Returns true when they were attached now.
 *
 * An attempt that fails half way (one filter in, the other refused) changes
 * the device's filters and so notifies us like anybody else's change. A
 * notification right after such a failure therefore starts no new attempt at
 * once, or the daemon would spin: the attempt is made ATTACH_RETRY_MS after
 * the failed one (dp_retry_due), and at every tick. Any other failure (the
 * device is just going away) is tried again with the next notification.
 */
static uint64_t failed_at;
static bool retry_wanted;

bool dp_retry_due(void)
{
	return retry_wanted && mono_ms() - failed_at >= ATTACH_RETRY_MS;
}

/* the uplink's index while the programs are on it, 0 otherwise */
unsigned int dp_ifindex(void)
{
	return attached_ifindex;
}

bool dp_tick(void)
{
	static uint64_t last_log;
	unsigned int ifindex = if_nametoindex(cfg.uplink);
	uint64_t now = mono_ms();
	int err;

	if (!ifindex) {
		if (gauge.attached || log_ok(&last_log))
			logmsg(LOG_WARNING, "uplink %s does not exist (yet): nothing attached",
			       cfg.uplink);
		gauge.attached = false;
		if (attached_ifindex)
			last_ifindex = attached_ifindex;
		attached_ifindex = 0;
		return false;
	}
	if (ifindex == attached_ifindex && attached(ifindex))
		return false;
	if (failed_at && now - failed_at < ATTACH_RETRY_MS) {
		retry_wanted = true;
		return false;
	}
	retry_wanted = false;
	err = attach(ifindex, gauge.attached || log_ok(&last_log));
	if (err) {
		gauge.attached = false;
		if (attached_ifindex)
			last_ifindex = attached_ifindex;
		attached_ifindex = 0;
		failed_at = err == -2 ? now : 0;
		return false;
	}
	failed_at = 0;
	if (attached_ifindex == ifindex)
		logmsg(LOG_WARNING, "attached to uplink %s (ifindex %u) again: the filters were "
		       "gone (something removed them: nothing was accepted on the mark meanwhile)",
		       cfg.uplink, ifindex);
	else
		logmsg(LOG_NOTICE, "attached to uplink %s (ifindex %u)%s", cfg.uplink, ifindex,
		       attached_ifindex || last_ifindex ? ": the device was created anew" : "");
	attached_ifindex = ifindex;
	gauge.attached = true;
	apply_bypass();
	cnt.dp_attached++;
	return true;
}

/*
 * Link notifications. A device that is created anew has no filters, and with
 * forwarded IPv6 untracked every reply through it is rejected until they are
 * back: the interval tick alone would leave the gateway closed for up to
 * interval seconds. So the main loop looks (dp_tick, udp_tick) whenever a
 * link comes, goes or changes, and whenever a qdisc or filter does (ours may
 * have been removed).
 */
static int link_fd = -1;

int dp_link_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_TC };

	link_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (link_fd < 0 || bind(link_fd, (struct sockaddr *)&sa, sizeof(sa))) {
		logmsg(LOG_WARNING, "link notifications: %s (a re-created uplink is noticed at "
		       "the next interval only)", strerror(errno));
		if (link_fd >= 0)
			close(link_fd);
		link_fd = -1;
	}
	return 0;
}

int dp_link_fd(void)
{
	return link_fd;
}

/* drain the notifications; true if there was any (or some were lost) */
bool dp_link_changed(void)
{
	char buf[8192];
	bool any = false;
	ssize_t n;

	for (;;) {
		n = recv(link_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n > 0 || (n < 0 && errno == ENOBUFS))
			any = true;
		else if (n < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	return any;
}

/* the detach command: filters off the uplink, maps unpinned */
int dp_detach(void)
{
	unsigned int ifindex = if_nametoindex(cfg.uplink);
	struct bpf_tc_hook hook;
	int dir, err, ret = 0;

	for (dir = 0; ifindex && dir < 2; dir++) {
		LIBBPF_OPTS(bpf_tc_opts, opts, .handle = TC_HANDLE, .priority = TC_PRIO);

		tc_hook(&hook, ifindex, dir);
		quiet = true;
		err = bpf_tc_detach(&hook, &opts);
		quiet = false;
		if (err && err != -ENOENT && err != -EINVAL) {
			fprintf(stderr, "%s: detach %s: %s\n", cfg.uplink,
				dir ? "egress" : "ingress", strerror(-err));
			ret = 1;
		}
	}
	unpin_all();
	return ret;
}

int dp_events_fd(void)
{
	return rb ? ring_buffer__epoll_fd(rb) : -1;
}

/* the flows whose first packet left since the last call */
void dp_handle_events(void (*cb)(const struct flow *f))
{
	event_cb = cb;
	if (rb)
		ring_buffer__consume(rb);
}

struct dp_walk *dp_walk_start(enum dp_map which)
{
	struct dp_walk *w = calloc(1, sizeof(*w));

	if (w) {
		w->which = which;
		w->first = true;
		w->now = dp_now();
	}
	return w;
}

void dp_walk_end(struct dp_walk *w)
{
	free(w);
}

/*
 * The next entries of a walk, at most max (and WALK_BATCH); *done when the
 * map is through. Entries added or deleted meanwhile may be missed or seen
 * twice, as with any walk of a live hash table. Returns the number of
 * entries, or -1.
 */
int dp_walk_next(struct dp_walk *w, struct dp_ent *out, unsigned int max, bool *done)
{
	static struct fs_key keys[WALK_BATCH];
	static union {
		struct fs_local l[WALK_BATCH];
		struct fs_remote r[WALK_BATCH];
	} vals;
	__u32 count = max < WALK_BATCH ? max : WALK_BATCH, i;
	int err;

	*done = false;
	err = bpf_map_lookup_batch(map_fd[w->which], w->first ? NULL : &w->token, &w->token, keys,
				   &vals, &count, NULL);
	w->first = false;
	if (err) {
		if (errno != ENOENT)
			return -1;
		*done = true;
	}
	for (i = 0; i < count; i++) {
		flow_of(&out[i].f, &keys[i]);
		if (w->which == DP_LOCAL)
			ent_local(&out[i], &vals.l[i], w->now);
		else
			ent_remote(&out[i], &vals.r[i], w->now);
	}
	return count;
}

int dp_delete(enum dp_map which, const struct flow *f)
{
	struct fs_key k;

	key_of(&k, f);
	return bpf_map_delete_elem(map_fd[which], &k);
}

int dp_get(enum dp_map which, const struct flow *f, struct dp_ent *out)
{
	union {
		struct fs_local l;
		struct fs_remote r;
	} v;
	struct fs_key k;

	key_of(&k, f);
	if (bpf_map_lookup_elem(map_fd[which], &k, &v))
		return -1;
	out->f = *f;
	if (which == DP_LOCAL)
		ent_local(out, &v.l, dp_now());
	else
		ent_remote(out, &v.r, dp_now());
	return 0;
}

/* a peer's flow, alive for element_timeout from now; written by
 * dp_remote_flush() */
void dp_remote_add(const struct flow *f)
{
	if (add_n == ADD_MAX)
		dp_remote_flush();
	key_of(&add_key[add_n], f);
	add_val[add_n].expires = dp_now() + cfg.element_timeout;
	add_n++;
}

/*
 * One system call for the lot. A full map refuses new flows (E2BIG) and still
 * takes the refreshes of those it has: the refused one is counted and the
 * rest is written.
 */
void dp_remote_flush(void)
{
	LIBBPF_OPTS(bpf_map_batch_opts, opts, .elem_flags = BPF_ANY);
	static uint64_t last_log;
	unsigned int done = 0;
	__u32 count;

	while (done < add_n) {
		count = add_n - done;
		if (!bpf_map_update_batch(map_fd[DP_REMOTE], add_key + done, add_val + done, &count,
					  &opts))
			break;
		if (errno == E2BIG) {
			cnt.rx_limited++;
		} else {
			cnt.rx_errors++;
			if (log_ok(&last_log))
				logmsg(LOG_WARNING, "remote map: %s", strerror(errno));
		}
		done += count + 1;	/* count were written, the next one failed */
	}
	add_n = 0;
}

/* the programs' counters, summed over the CPUs */
int dp_stats(struct fs_stats *sum)
{
	int ncpu = libbpf_num_possible_cpus(), i;
	struct fs_stats *v;
	__u32 zero = 0;
	unsigned int j;

	memset(sum, 0, sizeof(*sum));
	if (stats_fd < 0 || ncpu <= 0)
		return -1;
	v = calloc(ncpu, sizeof(*v));
	if (!v)
		return -1;
	if (bpf_map_lookup_elem(stats_fd, &zero, v)) {
		free(v);
		return -1;
	}
	for (i = 0; i < ncpu; i++)
		for (j = 0; j < sizeof(*sum) / sizeof(__u64); j++)
			((__u64 *)sum)[j] += ((__u64 *)&v[i])[j];
	free(v);
	return 0;
}
