# flowsync - builds on any Linux with libbpf and clang (for the tc programs)

# machine-specific settings (e.g. CFLAGS/LDFLAGS for headers and libraries
# outside the system paths), not tracked
-include local.mk

PROG = flowsync
OBJS = main.o util.o policy.o config.o wire.o udp.o dp.o fw.o tx.o rx.o resync.o status.o

# the tc programs, loaded by the daemon from /lib/bpf/flowsync.o (--bpf-object).
# On OpenWrt the package Makefile builds them with the BPF toolchain instead
# (include/bpf.mk); this rule is for development on the host.
BPF_OBJ = flowsync.o
CLANG ?= clang
BPF_CFLAGS ?= -O2 -g -Wall

# unit test for everything that needs no kernel
TEST = test_flowsync
TEST_OBJS = test_flowsync.o util.o policy.o config.o wire.o
TEST_RUNNER ?=

PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
BPFDIR ?= /lib/bpf

# optimized unless the caller says otherwise; "override" so that CFLAGS given
# in the environment (OpenWrt's MAKE_VARS) or on the command line are
# extended, not replaced
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu99 -Wall -Wextra \
	$(shell $(PKG_CONFIG) --cflags libbpf 2>/dev/null)
LDLIBS ?= $(shell $(PKG_CONFIG) --libs libbpf 2>/dev/null || echo -lbpf)

all: $(PROG) $(BPF_OBJ)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BPF_OBJ): bpf/flowsync.bpf.c dp.h
	$(CLANG) $(BPF_CFLAGS) -target bpf $(BPF_INCLUDES) -c $< -o $@

$(TEST): $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJS)

test: $(TEST)
	$(TEST_RUNNER) ./$(TEST)

# data path tests in network namespaces (../test/gwlab, see its README.md):
# the daemon on a fleet of gateways with real traffic over every gateway pair,
# judged by what the endpoints see. Need iproute2, tc, nft, python3 and
# util-linux, and real root (BPF programs cannot be loaded from a user
# namespace): as root they run here, otherwise in a throwaway VM on the
# host's kernel (virtme-ng; VNG=/path/to/vng if it is not in PATH).
#
#   make itest                                   scenario steady, all fleets
#   make itest S=symasym                         another scenario (test/gwlab/scenarios)
#   make itest S=steady FLEET=strict             one fleet
#   make itest FLOW='tcp_talk:A>gw1>B>gw2'       only flows whose name contains this
#   make itest S=longlived SCALE=10              timers and durations divided by 10
#   make itest SET='nfbypass=1'                  an option of the adapter (test/gwlab/impl/flowsync.py)
#
# Exit status 1 if a flow failed; results and logs in the directory it names
# (in the VM: OUT, default /tmp/gwlab-out).
# check: the unit test, dptest and the scenario steady.
S ?= steady
FLEET ?=
FLOW ?=
SCALE ?=
SET ?=
OUT ?= /tmp/gwlab-out
GWLAB_ARGS = $(S) --impl flowsync --set bin=$(CURDIR)/$(PROG) --set bpf_object=$(CURDIR)/$(BPF_OBJ) \
	$(foreach s,$(SET),--set $(s)) $(if $(FLEET),--fleet $(FLEET)) $(if $(FLOW),--flow '$(FLOW)') \
	$(if $(SCALE),--scale $(SCALE))

itest: $(PROG) $(BPF_OBJ)
	@cd ../test && if [ "$$(id -u)" = 0 ]; then python3 -m gwlab $(GWLAB_ARGS); \
		else gwlab/vm.sh $(OUT) $(GWLAB_ARGS); fi

# what is specific to this datapath, from inside a gateway (../test/dptest,
# see its README.md): flow tables, programs, rules, fragments, the bypass.
# Root or the VM as for itest.
#
#   make dptest                                  all scenarios
#   make dptest DS="frag_asym bypass"            some
#   make dptest DPTEST_BYPASS=1                  every daemon with --bypass
DS ?=
DPOUT ?= /tmp/dptest-out
DPTEST_ARGS = --flowsync $(CURDIR)/$(PROG) --bpf-object $(CURDIR)/$(BPF_OBJ) --quiet \
	$(if $(DS),--scenarios $(DS))

dptest: $(PROG) $(BPF_OBJ)
	@cd ../test && if [ "$$(id -u)" = 0 ]; then \
		DPTEST_BYPASS=$(DPTEST_BYPASS) python3 -m dptest $(DPTEST_ARGS); \
		else DPTEST_BYPASS=$(DPTEST_BYPASS) dptest/vm.sh $(DPOUT) $(DPTEST_ARGS); fi

check: test dptest itest

$(OBJS) $(TEST_OBJS): flowsync.h
dp.o tx.o status.o: dp.h

install: $(PROG) $(BPF_OBJ)
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(BPFDIR)
	install -m 755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(BPFDIR)/$(BPF_OBJ)

clean:
	rm -f $(PROG) $(TEST) $(OBJS) $(TEST_OBJS) $(BPF_OBJ)

.PHONY: all test itest dptest check install clean
