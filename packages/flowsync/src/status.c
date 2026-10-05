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

struct counters cnt;
struct gauges gauge;
const char *status_path = STATUS_FILE;

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
			 "copies=%llu copies_live=%llu copies_offloaded=%llu owned=%llu",
			 (unsigned long long)gauge.refresh_ms,
			 (unsigned long long)gauge.loop_max_ms,
			 (unsigned long long)gauge.copies,
			 (unsigned long long)gauge.copies_live,
			 (unsigned long long)gauge.copies_offloaded,
			 (unsigned long long)gauge.owned);
	logmsg(LOG_INFO, "%s", buf);
}

void write_status(void)
{
	char tmp[PATH_MAX], abuf[INET6_ADDRSTRLEN];
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
		fprintf(f, " tx_errors %llu\n", (unsigned long long)peer_tx_errors[i]);
	}
	fprintf(f, "refresh_running %d\n", gauge.refresh_running);
	fprintf(f, "refresh_entries %llu\n", (unsigned long long)gauge.refresh_entries);
	fprintf(f, "refresh_ms %llu\n", (unsigned long long)gauge.refresh_ms);
	fprintf(f, "loop_max_ms %llu\n", (unsigned long long)gauge.loop_max_ms);
	fprintf(f, "copies %llu\n", (unsigned long long)gauge.copies);
	fprintf(f, "copies_live %llu\n", (unsigned long long)gauge.copies_live);
	fprintf(f, "copies_offloaded %llu\n", (unsigned long long)gauge.copies_offloaded);
	fprintf(f, "owned %llu\n", (unsigned long long)gauge.owned);
	fprintf(f, "max_copies %lu\n", cfg.max_copies);
	fprintf(f, "max_copies_per_client %lu\n", cfg.max_copies_client);
#define X(name) fprintf(f, "%s %llu\n", #name, (unsigned long long)cnt.name);
	COUNTERS(X)
#undef X
	if (fclose(f) == 0)
		rename(tmp, status_path);
	else
		unlink(tmp);
}

static void print_proc(const char *label, const char *path)
{
	char buf[64];
	FILE *f = fopen(path, "r");

	if (f && fgets(buf, sizeof(buf), f))
		printf("%s %s", label, buf);
	if (f)
		fclose(f);
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
		ret = 1;
	} else {
		if (fstat(fileno(f), &st) == 0)
			printf("updated %lds ago\n", (long)(time(NULL) - st.st_mtime));
		while (fgets(line, sizeof(line), f)) {
			/* EPERM means the (root) daemon exists, we just may not signal it */
			if (sscanf(line, "pid %d", &pid) == 1 && kill(pid, 0) && errno == ESRCH) {
				printf("pid %d not running (stale status file)\n", pid);
				ret = 1;
			}
			fputs(line, stdout);
		}
		fclose(f);
	}
	print_proc("conntrack_count", "/proc/sys/net/netfilter/nf_conntrack_count");
	print_proc("conntrack_max", "/proc/sys/net/netfilter/nf_conntrack_max");
	return ret;
}
