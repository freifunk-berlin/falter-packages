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

# integration tests in network namespaces (../test/fstest, see
# ../test/README.md): need iproute2, nft, python3 and util-linux, and root
# (loading BPF programs is not possible in a user namespace).
#
#   make itest                                  default matrix, quiet
#   make itest S="asym frag_in"                 some scenarios
#   make itest P="g3" S=reroute                 one combination, named as printed
#   make itest M=quick | M=full                 another matrix
#   make itest T=tcp                            scenarios by tag
#   make itest J=8 V=1 K=1                      jobs, every run printed, keep all logs
#
# Prints failures, the result table and the summary; exit status 1 on any
# failure. Logs of failed runs stay in the directory the summary names, the
# timestamped progress of every check is in LOG (tail -f it).
# check: the unit test and the default matrix.
M ?= default
S ?=
P ?=
T ?=
J ?=
V ?=
K ?=
LOG ?= /tmp/flowsync-test.log

itest: $(PROG) $(BPF_OBJ)
	@cd ../test && { python3 -m unittest -q fstest.selftest >/dev/null 2>&1 || \
		{ echo "fstest selftest failed: cd test && python3 -m unittest fstest.selftest"; exit 1; }; } && \
	python3 -m fstest --flowsync ../src/$(PROG) --bpf-object ../src/$(BPF_OBJ) --log $(LOG) \
		$(if $(P),--profile "$(P)",--matrix $(M)) $(if $(S),--scenarios $(S)) \
		$(if $(T),--tags $(T)) $(if $(J),--jobs $(J)) $(if $(K),--keep) \
		$(if $(V),-v,--quiet)

check: test itest

$(OBJS) $(TEST_OBJS): flowsync.h
dp.o tx.o status.o: dp.h

install: $(PROG) $(BPF_OBJ)
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(BPFDIR)
	install -m 755 $(PROG) $(DESTDIR)$(SBINDIR)/$(PROG)
	install -m 644 $(BPF_OBJ) $(DESTDIR)$(BPFDIR)/$(BPF_OBJ)

clean:
	rm -f $(PROG) $(TEST) $(OBJS) $(TEST_OBJS) $(BPF_OBJ)

.PHONY: all test itest check install clean
