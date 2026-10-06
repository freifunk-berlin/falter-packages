/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The flow tables, shared by the tc programs (bpf/flowsync.bpf.c) and the
 * daemon (dp.c).
 *
 * fs_local   flows this gateway forwarded out of its uplink; written by the
 *            egress program only, one entry per flow, `seen` set by every
 *            packet. The daemon reads it (announcements) and deletes what
 *            has expired.
 * fs_remote  flows the peers announced; written by the daemon only, each with
 *            the time it expires at.
 *
 * The ingress program accepts (marks) a packet whose flow is alive in either
 * table, and with fs_ctl.bypass forwards it itself where it can. Nothing else
 * is shared: no entry is ever written by both sides.
 */
#ifndef FLOWSYNC_DP_H
#define FLOWSYNC_DP_H

#include <linux/types.h>

/* client: the address behind the gateways; server: the one outside. Ports in
 * network byte order: TCP, UDP and SCTP have both, every other protocol none
 * (0), its flows are per pair of addresses. */
struct fs_key {
	__u8 c[16];
	__u8 s[16];
	__be16 cport;
	__be16 sport;
	__u8 proto;
	__u8 pad[3];
};

/* TCP only: what the client's segments told us */
#define FS_F_EST	0x01	/* a segment other than a SYN: the handshake is past */
#define FS_F_CLOSING	0x02	/* the client sent FIN or RST */

struct fs_local {
	__u32 seen;		/* monotonic seconds of the last packet out */
	__u8 flags;
	__u8 pad[3];
};

struct fs_remote {
	__u32 expires;		/* monotonic seconds */
};

/* the programs' read-only configuration, set by the daemon before loading */
struct fs_cfg {
	__u32 mark;		/* set on accepted packets, cleared on all others */
	__u32 t_udp;		/* seconds after the last packet out */
	__u32 t_tcp_syn;	/* only SYNs so far */
	__u32 t_tcp_est;
	__u32 t_tcp_close;	/* after the client's FIN or RST */
	__u32 t_other;		/* every other protocol */
};

/* what the daemon may change while the programs run (fs_ctl, one entry) */
struct fs_ctl {
	__u32 bypass;		/* forward accepted TCP and UDP packets straight from tc */
};

/* per CPU */
struct fs_stats {
	__u64 out_pkts;		/* forwarded IPv6 packets out of the uplink, looked at */
	__u64 out_new;		/* of these, first of a flow */
	__u64 out_skip;		/* not parsed: ICMPv6, later fragments, header chain too long */
	__u64 in_pkts;		/* IPv6 packets in from the uplink, looked at */
	__u64 in_local;		/* accepted on a local flow */
	__u64 in_remote;	/* accepted on a peer's flow */
	__u64 in_bypass;	/* of the accepted ones, forwarded without the stack */
	__u64 in_miss;		/* no flow: left unmarked */
	__u64 in_skip;		/* not parsed, left unmarked (later fragments take the first one's mark) */
	__u64 ev_lost;		/* new flows whose event did not fit the ring */
};

#define FS_PROTO_TCP	6
#define FS_PROTO_UDP	17

/* how long a local flow lives after its last packet out */
static inline __u32 fs_ttl(const struct fs_cfg *c, __u8 proto, __u8 flags)
{
	if (proto == FS_PROTO_UDP)
		return c->t_udp;
	if (proto != FS_PROTO_TCP)
		return c->t_other;
	if (flags & FS_F_CLOSING)
		return c->t_tcp_close;
	return flags & FS_F_EST ? c->t_tcp_est : c->t_tcp_syn;
}

#endif
