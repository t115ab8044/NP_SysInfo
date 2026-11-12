# ===========================================================================
# Makefile - NP SysInfo midterm project (fork server + client + libutils.so)
#
#   make              debug build: -DENABLE_DEBUG, LOG_DEBUG compiled in
#   make release      LOG_DEBUG expands to nothing
#   make demo         one-command live demo, about 3 seconds
#   make test         concurrency, robustness and debug test programs
#   make test-capture tcpdump capture of one request, decoded (needs root / sudo)
#   make inspect      exported symbols of libutils.so and ldd linkage
#   make capture      live tcpdump of the server port on loopback (needs sudo)
#   make win-client   win_client.exe for the Real Test from Windows (needs mingw-w64)
#
# -Wl,-rpath,'$ORIGIN' makes each binary load the libutils.so sitting next to
# it, from any working directory.
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
HDRS    := utils.h

TEST_CFLAGS := -Wall -Wextra -pedantic -std=gnu11 -O2 -g
TEST_NAMES  := demo test_concurrent test_robustness test_debug test_capture
TEST_BINS   := $(addprefix tests/,$(TEST_NAMES))

WIN_CC      := x86_64-w64-mingw32-gcc

.PHONY: all release demo test test-concurrent test-robust test-debug test-capture \
        inspect capture win-client test-programs clean clean-build

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
	@$(CC) $(TEST_CFLAGS) -o $@ $< tests/testlib.c

test-programs: $(TEST_BINS)

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

inspect: all
	@echo "== Symbols exported by $(LIB) =="
	@nm -D --defined-only $(LIB) | grep -E ' [TBD] '
	@echo "== Dynamic linkage =="
	@ldd ./server ./client

capture:
	sudo tcpdump -i lo -n -x 'tcp port $(PORT)'

# Real Test: a Winsock client to run on Windows against the server in WSL2.
win-client: win_client.exe

win_client.exe: tests/win_client.c
	$(WIN_CC) -Wall -Wextra -pedantic -std=gnu11 -O2 -o $@ $< -lws2_32

# Used by the test programs: removes the build but keeps the running test binaries.
clean-build:
	rm -f $(LIB) $(BINS) .build_flags

clean: clean-build
	rm -f $(TEST_BINS) win_client.exe
