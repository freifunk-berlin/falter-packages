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

# optimized unless the caller says otherwise; "override" so that CFLAGS given
# in the environment (OpenWrt's MAKE_VARS) or on the command line are
# extended, not replaced
CFLAGS ?= -O2 -g
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

# data path tests in network namespaces (../test/gwlab, see its README.md):
# the daemon on a fleet of gateways with real traffic over every gateway pair,
# judged by what the endpoints see. Need iproute2, tc, nft, python3 and
# util-linux, no root.
#
#   make itest                                   scenario steady, all fleets
#   make itest S=symasym                         another scenario (test/gwlab/scenarios)
#   make itest S=steady FLEET=strict             one fleet
#   make itest FLOW='tcp_talk:A>gw1>B>gw2'       only flows whose name contains this
#   make itest S=longlived SCALE=10              timers and durations divided by 10
#
# Exit status 1 if a flow failed; results and logs in the directory it names.
# check: the unit test and the scenario steady.
S ?= steady
FLEET ?=
FLOW ?=
SCALE ?=

itest: $(PROG)
	@cd ../test && python3 -m gwlab $(S) --impl flowsync --set bin=../src/$(PROG) \
		$(if $(FLEET),--fleet $(FLEET)) $(if $(FLOW),--flow '$(FLOW)') \
		$(if $(SCALE),--scale $(SCALE))

check: test itest

$(OBJS) $(TEST_OBJS): flowsync.h

install: $(PROG)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)

clean:
	rm -f $(PROG) $(TEST) $(OBJS) $(TEST_OBJS)

.PHONY: all test itest check install clean
