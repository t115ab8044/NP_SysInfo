# ===========================================================================
# Makefile - NP SysInfo midterm project (fork server + client + libutils.so)
#
#   make              debug build: -DENABLE_DEBUG, LOG_DEBUG compiled in
#   make release      LOG_DEBUG expands to nothing (zero overhead)
#   make demo         one-command live demo, about 3 seconds
#   make test         run the concurrency, robustness and debug proof programs
#   make test-capture tcpdump proof of the wire format (needs root / sudo)
#   make test-real    Windows <-> WSL2 traffic, plus injected network faults
#   make audit        static analysis, sanitizers, load and hostile-peer tests
#   make inspect      exported symbols of libutils.so and ldd linkage
#   make capture      live hex dump of loopback traffic (needs sudo)
#
# -Wl,-rpath,'$ORIGIN' makes each binary load the libutils.so sitting next to
# it, from any working directory and never one found in the current directory.
# ===========================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -pedantic -std=gnu11 -O2 -g
LDFLAGS := -L. -Wl,-rpath,'$$ORIGIN'
LDLIBS  := -lutils

DEBUG ?= 1
ifeq ($(DEBUG),1)
CFLAGS  += -DENABLE_DEBUG
endif

PORT    ?= 9090
LIB     := libutils.so
BINS    := server client
HDRS    := protocol.h utils.h

# The test programs are always built with plain gcc, even for sanitized builds.
TEST_CC     := gcc
TEST_CFLAGS := -Wall -Wextra -pedantic -std=gnu11 -O2 -g
TEST_NAMES  := demo test_concurrent test_robustness test_debug test_capture test_real test_audit
TEST_BINS   := $(addprefix tests/,$(TEST_NAMES))

.PHONY: all release demo test test-concurrent test-robust test-debug test-capture \
        test-real audit inspect capture capture-pcap test-programs clean clean-build

all: $(LIB) $(BINS)

# Record CFLAGS so that switching DEBUG=1 <-> DEBUG=0 rebuilds the library
# and the executables together: they must agree on ENABLE_DEBUG.
.build_flags: FORCE
	@echo '$(CFLAGS)' | cmp -s - $@ || echo '$(CFLAGS)' > $@

FORCE:

$(LIB): utils.c $(HDRS) .build_flags
	$(CC) $(CFLAGS) -fPIC -shared -o $@ utils.c

$(BINS): %: %.c $(HDRS) $(LIB) .build_flags
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

release:
	$(MAKE) DEBUG=0 all

$(TEST_BINS): tests/%: tests/%.c tests/testlib.c tests/testlib.h
	@$(TEST_CC) $(TEST_CFLAGS) -o $@ $< tests/testlib.c

test-programs: $(TEST_BINS)

# One-command live demo (~3 s): .so linkage, sysinfo, 12 concurrent forks,
# a rejected garbage request, no zombies.
demo: all tests/demo
	@./tests/demo

test: test-concurrent test-robust test-debug

test-concurrent: all tests/test_concurrent
	@./tests/test_concurrent

test-robust: all tests/test_robustness
	@./tests/test_robustness

# Rebuilds the tree (release, then debug), so it runs last in `make test`.
test-debug: all tests/test_debug
	@./tests/test_debug

test-capture: all tests/test_capture
	@./tests/test_capture

test-real: all tests/test_real
	@./tests/test_real

audit: all tests/test_audit
	@./tests/test_audit

inspect: all
	@echo "== Symbols exported by $(LIB) =="
	@nm -D --defined-only $(LIB) | grep -E ' [TBD] '
	@echo "== Dynamic linkage =="
	@ldd ./server ./client

capture:
	sudo tcpdump -i lo -nn -X 'tcp port $(PORT)'

capture-pcap:
	sudo tcpdump -i lo -nn -w capture.pcap 'tcp port $(PORT)'

# Used by the test programs: removes the build but keeps the running test binaries.
clean-build:
	rm -f $(LIB) $(BINS) .build_flags

clean: clean-build
	rm -f $(TEST_BINS) tests/hostile_server tests/np_client_win.exe capture.pcap
