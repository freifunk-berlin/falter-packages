// SPDX-License-Identifier: GPL-2.0-only
/* counters, gauges, status file and the status command */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "flowsync.h"
#include "dp.h"

struct counters cnt;
struct gauges gauge;
const char *status_path = STATUS_FILE;

/* the tc programs' counters, by name */
#define DP_STATS(X) \
	X(out_pkts) X(out_new) X(out_skip) X(out_answer) \
	X(in_pkts) X(in_local) X(in_remote) X(in_bypass) X(in_miss) \
	X(in_skip) X(ev_lost) \
	X(in_nobypass_off) X(in_nobypass_other_mac) X(in_nobypass_first_frag) \
	X(in_nobypass_exthdr) \
	X(in_nobypass_tcp_syn) X(in_nobypass_hop_limit) X(in_nobypass_fib) X(in_nobypass_same_if) \
	X(in_nobypass_mtu) X(in_nobypass_hl_write_fail) \
	X(in_bypass_proto_tcp) X(in_bypass_proto_udp) X(in_bypass_proto_esp) X(in_bypass_proto_gre) \
	X(in_bypass_proto_ipip) X(in_bypass_proto_ip6ip6) X(in_bypass_proto_l2tp) \
	X(in_bypass_proto_sctp) X(in_bypass_proto_other)

/* fs_stats' in_skip_why and out_skip_why, in their order */
static const char *const skip_why[FS_SKIP_WHY] = { "icmpv6", "later_frag", "exthdr_chain", "other" };

void log_counters(void)
{
	char buf[2048];
	size_t len = 0;

#define X(name) \
	if (len < sizeof(buf)) \
		len += snprintf(buf + len, sizeof(buf) - len, "%s%s=%llu", \
				len ? " " : "", #name, (unsigned long long)cnt.name);
	COUNTERS(X)
#undef X
	if (len < sizeof(buf))
		snprintf(buf + len, sizeof(buf) - len, " refresh_ms=%llu loop_max_ms=%llu "
			 "local=%llu remote=%llu attached=%d fw_ok=%d alive=%d",
			 (unsigned long long)gauge.refresh_ms,
			 (unsigned long long)gauge.loop_max_ms,
			 (unsigned long long)gauge.local,
			 (unsigned long long)gauge.remote, gauge.attached, gauge.fw_ok,
			 gauge.alive);
	logmsg(LOG_INFO, "%s", buf);
}

void write_status(void)
{
	char tmp[PATH_MAX], abuf[INET6_ADDRSTRLEN], ebuf[16];
	struct fs_stats st;
	unsigned int i;
	uint32_t now;
	FILE *f;
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", status_path);
	/* created anew, never through whatever is at that name (a symlink in a
	 * writable directory would have root write anywhere) */
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	f = fd < 0 ? NULL : fdopen(fd, "w");
	if (!f) {
		static uint64_t last_log;

		if (fd >= 0)
			close(fd);
		if (log_ok(&last_log))
			logmsg(LOG_WARNING, "status file %s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(f, "pid %d\n", (int)getpid());
	/* peer <addr> rx <datagrams> age <seconds since the last one | never>.
	 * Every peer sends a heartbeat each interval: an age of several
	 * intervals means the peer or the path is down. */
	now = now_s();
	for (i = 0; i < cfg.n_peer; i++) {
		fprintf(f, "peer %s rx %llu age ", addr_str(&cfg.peer[i], abuf, sizeof(abuf)),
			(unsigned long long)peer_rx[i]);
		if (peer_last[i])
			fprintf(f, "%u", now - peer_last[i]);
		else
			fprintf(f, "never");
		fprintf(f, " tx_errors %llu", (unsigned long long)peer_tx_errors[i]);
		/* the last one: what it was, and how long ago */
		if (peer_tx_errno[i])
			fprintf(f, " last_error %s last_error_age %u",
				errno_name(peer_tx_errno[i], ebuf, sizeof(ebuf)),
				now - peer_tx_err_at[i]);
		fprintf(f, "\n");
	}
	fprintf(f, "attached %d\n", gauge.attached);
	fprintf(f, "jited %d\n", gauge.jited);
	fprintf(f, "fw_ok %d\n", gauge.fw_ok);
	fprintf(f, "alive %d\n", gauge.alive);
	fprintf(f, "bypass %d\n", gauge.bypass);
	fprintf(f, "refresh_running %d\n", gauge.refresh_running);
	fprintf(f, "refresh_entries %llu\n", (unsigned long long)gauge.refresh_entries);
	fprintf(f, "refresh_ms %llu\n", (unsigned long long)gauge.refresh_ms);
	fprintf(f, "loop_max_ms %llu\n", (unsigned long long)gauge.loop_max_ms);
	fprintf(f, "local %llu\n", (unsigned long long)gauge.local);
	fprintf(f, "remote %llu\n", (unsigned long long)gauge.remote);
	fprintf(f, "max_flows %lu\n", cfg.max_flows);
	fprintf(f, "max_remote %lu\n", cfg.max_remote);
#define X(name) fprintf(f, "%s %llu\n", #name, (unsigned long long)cnt.name);
	COUNTERS(X)
#undef X
	if (!dp_stats(&st)) {
#define X(name) fprintf(f, "dp_%s %llu\n", #name, (unsigned long long)st.name);
		DP_STATS(X)
#undef X
		for (i = 0; i < FS_SKIP_WHY; i++)
			fprintf(f, "dp_in_skip_%s %llu\n", skip_why[i],
				(unsigned long long)st.in_skip_why[i]);
		for (i = 0; i < FS_SKIP_WHY; i++)
			fprintf(f, "dp_out_skip_%s %llu\n", skip_why[i],
				(unsigned long long)st.out_skip_why[i]);
	}
	if (fclose(f) == 0)
		rename(tmp, status_path);
	else
		unlink(tmp);
}

int cmd_status(void)
{
	struct stat st;
	char line[256];
	FILE *f;
	int pid = 0, ret = 0;

	f = fopen(status_path, "r");
	if (!f) {
		printf("daemon not running (no %s)\n", status_path);
		return 1;
	}
	if (fstat(fileno(f), &st) == 0)
		printf("updated %lds ago\n", (long)(time(NULL) - st.st_mtime));
	while (fgets(line, sizeof(line), f)) {
		/* EPERM means the daemon exists, we just may not signal it */
		if (sscanf(line, "pid %d", &pid) == 1 && kill(pid, 0) && errno == ESRCH) {
			printf("pid %d not running (stale status file)\n", pid);
			ret = 1;
		}
		fputs(line, stdout);
	}
	fclose(f);
	return ret;
}
