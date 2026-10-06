// SPDX-License-Identifier: GPL-2.0-only
/* logging, time, address and prefix helpers */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "flowsync.h"

static bool use_syslog;

/* syslog when run by procd/systemd (stderr is a pipe), timestamped console
 * lines when stderr is a terminal */
void log_open(bool daemon)
{
	if (isatty(STDERR_FILENO) || !daemon)
		return;
	use_syslog = true;
	openlog("flowsync", LOG_PID, LOG_DAEMON);
}

/* "udp [c]:p -> [s]:p"; buf needs about 100 bytes */
const char *flow_str(const struct flow *f, char *buf, size_t len)
{
	char c[INET6_ADDRSTRLEN], s[INET6_ADDRSTRLEN], p[8];

	inet_ntop(AF_INET6, &f->c, c, sizeof(c));
	inet_ntop(AF_INET6, &f->s, s, sizeof(s));
	snprintf(buf, len, "%s [%s]:%u -> [%s]:%u", proto_str(f->proto, p, sizeof(p)), c, f->cport,
		 s, f->sport);
	return buf;
}

/* one debug line per record, e.g. "event udp [c]:p -> [s]:p" */
void flow_dbg(const char *what, const struct flow *f)
{
	char buf[128];

	if (!cfg.debug)
		return;
	logmsg(LOG_DEBUG, "%s %s", what, flow_str(f, buf, sizeof(buf)));
}

static const char *prio_name(int prio)
{
	switch (prio) {
	case LOG_ERR:
		return "err";
	case LOG_WARNING:
		return "warn";
	case LOG_NOTICE:
		return "notice";
	case LOG_INFO:
		return "info";
	case LOG_DEBUG:
		return "debug";
	}
	return "?";
}

void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;
	struct tm tm;
	time_t t;

	if (prio == LOG_DEBUG && !cfg.debug)
		return;
	va_start(ap, fmt);
	if (use_syslog) {
		vsyslog(prio, fmt, ap);
	} else {
		t = time(NULL);
		localtime_r(&t, &tm);
		fprintf(stderr, "%02d:%02d:%02d [%s] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
			prio_name(prio));
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

uint64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* monotonic seconds, never 0 (0 marks an empty table slot) */
uint32_t now_s(void)
{
	return mono_ms() / 1000 + 1;
}

/* true at most once per LOG_INTERVAL_MS for the given slot */
bool log_ok(uint64_t *last)
{
	uint64_t now = mono_ms();

	if (*last && now - *last < LOG_INTERVAL_MS)
		return false;
	*last = now;
	return true;
}

int parse_addr(const char *s, struct in6_addr *a)
{
	struct in_addr a4;

	if (inet_pton(AF_INET6, s, a) == 1)
		return 0;
	if (inet_pton(AF_INET, s, &a4) == 1) {
		memset(a, 0, sizeof(*a));
		a->s6_addr[10] = 0xff;
		a->s6_addr[11] = 0xff;
		memcpy(&a->s6_addr[12], &a4, 4);
		return 0;
	}
	return -1;
}

const char *addr_str(const struct in6_addr *a, char *buf, size_t len)
{
	if (IN6_IS_ADDR_V4MAPPED(a))
		return inet_ntop(AF_INET, &a->s6_addr[12], buf, len);
	return inet_ntop(AF_INET6, a, buf, len);
}

int parse_prefix(const char *s, struct prefix *p)
{
	char buf[INET6_ADDRSTRLEN + 8], *slash, *end;
	unsigned long len = 128;
	unsigned int i;

	if (strlen(s) >= sizeof(buf))
		return -1;
	strcpy(buf, s);
	slash = strchr(buf, '/');
	if (slash) {
		*slash++ = 0;
		errno = 0;
		len = strtoul(slash, &end, 10);
		if (errno || end == slash || *end || len > 128)
			return -1;
	}
	if (inet_pton(AF_INET6, buf, &p->addr) != 1)
		return -1;
	p->len = len;
	for (i = 0; i < 16; i++) {
		if (len >= 8)
			len -= 8;
		else {
			p->addr.s6_addr[i] &= (uint8_t)(0xff << (8 - len));
			len = 0;
		}
	}
	return 0;
}

static bool prefix_match(const struct in6_addr *a, const struct prefix *p)
{
	unsigned int bytes = p->len / 8, bits = p->len % 8;

	if (memcmp(a->s6_addr, p->addr.s6_addr, bytes))
		return false;
	if (bits && ((a->s6_addr[bytes] ^ p->addr.s6_addr[bytes]) &
		     (uint8_t)(0xff << (8 - bits))))
		return false;
	return true;
}

bool in_list(const struct in6_addr *a, const struct prefix_list *l)
{
	unsigned int i;

	for (i = 0; i < l->n; i++)
		if (prefix_match(a, &l->p[i]))
			return true;
	return false;
}

int parse_port(const char *s, uint16_t *port)
{
	unsigned long v;
	char *end;

	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || *end || v > 65535)
		return -1;
	*port = v;
	return 0;
}
