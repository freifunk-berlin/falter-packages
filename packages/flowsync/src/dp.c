// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
/*
 * The datapath: the tc programs on the uplink, their maps and the new-flow
 * events (bpf/flowsync.bpf.c, dp.h).
 *
 * The maps are pinned and the programs stay attached when the daemon exits:
 * flows on a symmetric path, and peers' flows until they expire, keep passing
 * while it is down or restarts. A restart loads the programs anew (with the
 * current configuration), reuses the maps and replaces the programs in place.
 *
 * The programs sit on the device's tcx hooks (BPF_TCX_INGRESS/EGRESS, kernel
 * 6.6), not on a qdisc: they run where a clsact qdisc's filters would, before
 * the device's tc filters, and leave the qdiscs alone. An ingress qdisc with
 * SQM's redirect to an IFB, a shaper as the root qdisc, or a clsact of
 * somebody else can be on the uplink at the same time, and may come and go
 * while the programs are attached.
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

/* the uplink's tcx hooks our programs go on, by direction (0 in, 1 out) */
static const enum bpf_attach_type tcx_type[2] = { BPF_TCX_INGRESS, BPF_TCX_EGRESS };
/* the programs' names in the object, which is how a previous daemon's are
 * recognised on the hook (bpf_prog_info.name) */
static const char *const prog_name[2] = { "fs_ingress", "fs_egress" };
static const char *const dir_name[2] = { "ingress", "egress" };
#define TCX_MAX		64	/* BPF_MPROG_MAX: room for every id a hook can hold (63) */
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

/* the maps, by their name in the object and in the pin directory */
static const struct {
	const char *name;
	int *fd;
} maps[] = {
	{ "fs_local", &map_fd[DP_LOCAL] },
	{ "fs_remote", &map_fd[DP_REMOTE] },
	{ "fs_stats", &stats_fd },
	{ "fs_ctl", &ctl_fd },
};
#define N_MAPS	(sizeof(maps) / sizeof(maps[0]))

/* the remote entries of one receive pass, written with one system call */
#define ADD_MAX		(DRAIN_MAX * WIRE_MAX_RECORDS)
static struct fs_key add_key[ADD_MAX];
static struct fs_remote add_val[ADD_MAX];
static unsigned int add_n;

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

static int print_cb(enum libbpf_print_level level, const char *fmt, va_list ap)
{
	char buf[512];
	size_t len;

	if (level == LIBBPF_DEBUG)
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
	char path[PATH_MAX];
	unsigned int i;

	for (i = 0; i < N_MAPS; i++) {
		pin_path(path, sizeof(path), maps[i].name);
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
	unsigned int i;
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
	for (i = 0; i < N_MAPS; i++)
		*maps[i].fd = bpf_object__find_map_fd_by_name(obj, maps[i].name);
	for (i = 0; i < 2; i++) {
		p = bpf_object__find_program_by_name(obj, prog_name[i]);
		prog_fd[i] = p ? bpf_program__fd(p) : -1;
	}
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

static int on_event(void *ctx, void *data, size_t len)
{
	struct flow f;

	(void)ctx;
	if (len < sizeof(struct fs_key))
		return 0;
	flow_of(&f, data);
	cnt.ev_recv++;
	tx_event(&f);
	return 0;
}

/*
 * load: the daemon. Loads the programs, creating the maps or taking the
 * pinned ones. A map pinned with another size (max_flows or max_remote
 * changed) cannot be reused: it is dropped and made anew, empty (see
 * drop_unusable_pin).
 * Without load (the flow commands): open the pinned maps of a running or
 * stopped daemon.
 */
int dp_open(bool load_progs)
{
	struct bpf_prog_info info;
	struct fs_ctl ctl = {};
	char path[PATH_MAX];
	__u32 ilen, zero = 0;
	int i;

	set_cfg();
	libbpf_set_print(print_cb);
	if (!load_progs) {
		for (i = 0; i < (int)N_MAPS; i++) {
			pin_path(path, sizeof(path), maps[i].name);
			*maps[i].fd = bpf_obj_get(path);
		}
		/* lifetimes as the programs count them, not as this
		 * invocation's options would (those only for maps that an
		 * older daemon left, which do not say) */
		if (ctl_fd >= 0 && !bpf_map_lookup_elem(ctl_fd, &zero, &ctl) && ctl.cfg.t_udp)
			dcfg = ctl.cfg;
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
		/* readable as root only (the field is zeroed for others): here,
		 * before the privileges go. MIPS boots with the JIT off and relies
		 * on base-files' sysctl to turn it on; interpreted, the programs
		 * cost several times more per packet */
		gauge.jited = info.jited_prog_len != 0;
		if (!gauge.jited)
			logmsg(LOG_WARNING, "%s is not JIT-compiled: the interpreter runs it per packet "
			       "(net.core.bpf_jit_enable=0?)", prog_name[i]);
	}
	if (bpf_map_lookup_elem(ctl_fd, &zero, &ctl))
		memset(&ctl, 0, sizeof(ctl));
	ctl.cfg = dcfg;
	if (bpf_map_update_elem(ctl_fd, &zero, &ctl, BPF_ANY)) {
		logmsg(LOG_ERR, "fs_ctl: %s", strerror(errno));
		return -1;
	}
	rb = ring_buffer__new(bpf_object__find_map_fd_by_name(obj, "fs_events"), on_event, NULL,
			      NULL);
	if (!rb) {
		logmsg(LOG_ERR, "event ring: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* the ids of the programs on one hook of the device, in their order. Returns
 * how many, -errno. A device nothing was ever attached to has no hook yet:
 * that is none, not an error. */
static int hook_progs(unsigned int ifindex, int dir, __u32 *ids)
{
	LIBBPF_OPTS(bpf_prog_query_opts, q, .prog_ids = ids, .prog_cnt = TCX_MAX);
	int err = bpf_prog_query_opts(ifindex, tcx_type[dir], &q);

	if (err == -ENOENT)
		return 0;
	if (err)
		return err;
	return q.prog_cnt > TCX_MAX ? TCX_MAX : (int)q.prog_cnt;
}

static bool has_id(const __u32 *ids, int n, __u32 id)
{
	int i;

	for (i = 0; i < n; i++)
		if (ids[i] == id)
			return true;
	return false;
}

/* a descriptor of the program with this id if it is ours by name (a previous
 * daemon's: the same object, loaded by another process), -1 otherwise. Needs
 * CAP_SYS_ADMIN (BPF_PROG_GET_FD_BY_ID), which the daemon has at startup, the
 * moment such a program can be there. */
static int ours_by_name(__u32 id, int dir)
{
	struct bpf_prog_info info;
	__u32 ilen = sizeof(info);
	int fd = bpf_prog_get_fd_by_id(id);

	if (fd < 0)
		return -1;
	memset(&info, 0, sizeof(info));
	if (bpf_prog_get_info_by_fd(fd, &info, &ilen) ||
	    strncmp(info.name, prog_name[dir], sizeof(info.name))) {
		close(fd);
		return -1;
	}
	return fd;
}

/* say: log why it fails (the caller tries again and again). Returns 0, -1 if
 * it failed, -2 if it failed after it had put the ingress program on.
 *
 * Per hook: our program is there, nothing to do; a previous daemon's is
 * there, it is replaced in place (no packet passes without a program);
 * otherwise ours is appended to whatever programs the hook holds. */
static int attach(unsigned int ifindex, bool say)
{
	__u32 ids[TCX_MAX];
	int dir, i, n, old, err;

	for (dir = 0; dir < 2; dir++) {
		LIBBPF_OPTS(bpf_prog_attach_opts, opts);

		n = hook_progs(ifindex, dir, ids);
		if (n < 0) {
			if (say)
				logmsg(LOG_ERR, "%s: programs on %s: %s", cfg.uplink,
				       dir_name[dir], strerror(-n));
			return dir ? -2 : -1;
		}
		if (has_id(ids, n, prog_id[dir]))
			continue;
		old = -1;
		for (i = 0; i < n && old < 0; i++)
			old = ours_by_name(ids[i], dir);
		if (old >= 0) {
			opts.flags = BPF_F_REPLACE;
			opts.replace_prog_fd = old;
		}
		err = bpf_prog_attach_opts(prog_fd[dir], ifindex, tcx_type[dir], &opts);
		if (old >= 0)
			close(old);
		if (err) {
			if (say)
				logmsg(LOG_ERR, "%s: attach %s: %s", cfg.uplink,
				       dir_name[dir], strerror(-err));
			return dir ? -2 : -1;
		}
	}
	return 0;
}

/* the bypass switch of the running programs; -1 if it cannot be set */
int dp_set_bypass(bool on)
{
	struct fs_ctl ctl;
	__u32 zero = 0;

	if (ctl_fd < 0 || bpf_map_lookup_elem(ctl_fd, &zero, &ctl))
		return -1;
	ctl.bypass = on;
	if (bpf_map_update_elem(ctl_fd, &zero, &ctl, BPF_ANY))
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

/* are our programs on both hooks of the device? */
static bool attached(unsigned int ifindex)
{
	__u32 ids[TCX_MAX];
	int dir, n;

	for (dir = 0; dir < 2; dir++) {
		n = hook_progs(ifindex, dir, ids);
		if (n < 0 || !has_id(ids, n, prog_id[dir]))
			return false;
	}
	return true;
}

/*
 * At startup and every interval: the programs must be on the uplink. The
 * device may not exist yet, may have been created anew under its name (netifd
 * does that to a VLAN on every ifup; the new device has no programs), or
 * somebody detached them. Returns true when they were attached now.
 *
 * An attempt that fails half way (the ingress program on, the egress one
 * refused) is not repeated with the next link notification but
 * ATTACH_RETRY_MS later (dp_retry_due), and at every tick: a hook that
 * refuses keeps refusing, and a device that changes while the uplink is
 * being set up would otherwise make the daemon try on every notification.
 * Any other failure (the device is just going away) is tried again with the
 * next notification.
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

static void detached(void)
{
	gauge.attached = false;
	if (attached_ifindex)
		last_ifindex = attached_ifindex;
	attached_ifindex = 0;
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
		detached();
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
		detached();
		failed_at = err == -2 ? now : 0;
		return false;
	}
	failed_at = 0;
	if (attached_ifindex == ifindex)
		logmsg(LOG_WARNING, "attached to uplink %s (ifindex %u) again: the programs were "
		       "gone (something detached them: nothing was accepted on the mark meanwhile)",
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
 * Link notifications. A device that is created anew has no programs, and with
 * forwarded IPv6 untracked every reply through it is rejected until they are
 * back: the interval tick alone would leave the gateway closed for up to
 * interval seconds. So the main loop looks (dp_tick, udp_tick) whenever a
 * link comes, goes or changes. Attaching and detaching tcx programs sends no
 * notification: somebody detaching ours is noticed at the next tick.
 */
static int link_fd = -1;

void dp_link_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };

	link_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (link_fd < 0 || bind(link_fd, (struct sockaddr *)&sa, sizeof(sa))) {
		logmsg(LOG_WARNING, "link notifications: %s (a re-created uplink is noticed at "
		       "the next interval only)", strerror(errno));
		if (link_fd >= 0)
			close(link_fd);
		link_fd = -1;
	}
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

/* the detach command: our programs (by name: this process loaded none) off
 * the uplink's hooks, other programs there untouched, maps unpinned */
int dp_detach(void)
{
	unsigned int ifindex = if_nametoindex(cfg.uplink);
	__u32 ids[TCX_MAX];
	int dir, i, n, fd, err, ret = 0;

	for (dir = 0; ifindex && dir < 2; dir++) {
		n = hook_progs(ifindex, dir, ids);
		if (n < 0) {
			fprintf(stderr, "%s: programs on %s: %s\n", cfg.uplink,
				dir_name[dir], strerror(-n));
			ret = 1;
			continue;
		}
		for (i = 0; i < n; i++) {
			fd = ours_by_name(ids[i], dir);
			if (fd < 0)
				continue;
			err = bpf_prog_detach_opts(fd, ifindex, tcx_type[dir], NULL);
			close(fd);
			if (err && err != -ENOENT) {
				fprintf(stderr, "%s: detach %s: %s\n", cfg.uplink,
					dir_name[dir], strerror(-err));
				ret = 1;
			}
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
void dp_handle_events(void)
{
	if (rb)
		ring_buffer__consume(rb);
}

void dp_walk_start(struct dp_walk *w, enum dp_map which)
{
	w->which = which;
	w->first = true;
	w->now = dp_now();
}

/*
 * The next entries of a walk, at most max (and WALK_BATCH); *done when the
 * map is through. Entries added or deleted meanwhile may be missed or seen
 * twice, as with any walk of a live hash table. Returns the number of
 * entries, or -1.
 */
/* the next batch of the walk, WALK_BATCH entries at most */
int dp_walk_next(struct dp_walk *w, struct dp_ent *out, bool *done)
{
	static struct fs_key keys[WALK_BATCH];
	static union {
		struct fs_local l[WALK_BATCH];
		struct fs_remote r[WALK_BATCH];
	} vals;
	__u32 count = WALK_BATCH, i;
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
	/* asked after every batch of events (tx.c): the buffer is kept */
	static struct fs_stats *v;
	static int ncpu;
	__u32 zero = 0;
	unsigned int j;
	int i;

	memset(sum, 0, sizeof(*sum));
	if (!v) {
		ncpu = libbpf_num_possible_cpus();
		if (ncpu > 0)
			v = calloc(ncpu, sizeof(*v));
	}
	if (stats_fd < 0 || !v || bpf_map_lookup_elem(stats_fd, &zero, v))
		return -1;
	for (i = 0; i < ncpu; i++)
		for (j = 0; j < sizeof(*sum) / sizeof(__u64); j++)
			((__u64 *)sum)[j] += ((__u64 *)&v[i])[j];
	return 0;
}
