# flowsync - builds on any Linux with libnetfilter_conntrack and libmnl

# machine-specific settings (e.g. CFLAGS/LDFLAGS for headers and libraries
# outside the system paths), not tracked
-include local.mk

PROG = flowsync
OBJS = main.o util.o policy.o config.o ctnl.o wire.o udp.o tx.o inject.o rx.o resync.o status.o

# unit test for everything that needs no kernel; needs only libmnl
TEST = test_flowsync
TEST_OBJS = test_flowsync.o util.o policy.o config.o status.o ctnl.o wire.o rx.o inject.o udp.o resync.o
TEST_RUNNER ?=

PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin

# "override" so that CFLAGS given on the command line (OpenWrt does that) are
# extended, not replaced
override CFLAGS += -std=gnu99 -Wall -Wextra \
	$(shell $(PKG_CONFIG) --cflags libnetfilter_conntrack libmnl 2>/dev/null)
LDLIBS ?= $(shell $(PKG_CONFIG) --libs libnetfilter_conntrack libmnl 2>/dev/null || \
	echo -lnetfilter_conntrack -lmnl)
LDLIBS_TEST ?= -lmnl

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(TEST): $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJS) $(LDLIBS_TEST)

test: $(TEST)
	$(TEST_RUNNER) ./$(TEST)

# integration tests in network namespaces (../test/fstest, see
# ../test/README.md): need iproute2, nft, python3 and util-linux, no root.
#
#   make itest                                  default matrix, quiet
#   make itest S="resync tcp_idle"              some scenarios
#   make itest P="g3 off=01 ack=-" S=tcp_idle   one combination, named as printed
#   make itest M=quick | M=full                 another matrix
#   make itest T=tcp                            scenarios by tag
#   make itest J=8 V=1 K=1                      jobs, every run printed, keep all logs
#
# Prints failures, the result table and the summary; exit status 1 on any
# failure. Logs of failed runs stay in the directory the summary names, the
# timestamped progress of every check is in LOG (tail -f it).
# check: the unit test and the default matrix.
CTQ = ../test/ctquery
CTQ_OBJS = ctnl.o util.o policy.o config.o status.o wire.o udp.o
M ?= default
S ?=
P ?=
T ?=
J ?=
V ?=
K ?=
LOG ?= /tmp/flowsync-test.log

$(CTQ): ../test/ctquery.c $(CTQ_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ ../test/ctquery.c $(CTQ_OBJS) $(LDLIBS_TEST)

itest: $(PROG) $(CTQ)
	@cd ../test && { python3 -m unittest -q fstest.selftest >/dev/null 2>&1 || \
		{ echo "fstest selftest failed: cd test && python3 -m unittest fstest.selftest"; exit 1; }; } && \
	python3 -m fstest --flowsync ../src/$(PROG) --ctquery ctquery --log $(LOG) \
		$(if $(P),--profile "$(P)",--matrix $(M)) $(if $(S),--scenarios $(S)) \
		$(if $(T),--tags $(T)) $(if $(J),--jobs $(J)) $(if $(K),--keep) \
		$(if $(V),-v,--quiet)

check: test itest

$(OBJS) $(TEST_OBJS): flowsync.h

install: $(PROG)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)

clean:
	rm -f $(PROG) $(TEST) $(CTQ) $(OBJS) $(TEST_OBJS)

.PHONY: all test itest check install clean
