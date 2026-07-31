# ardopb -- a rebuilt ARDOP HF data modem (see README.md).
#
# Build requirements (Debian/Ubuntu):
#   sudo apt install build-essential libasound2-dev   # ardopb + apps
#   sudo apt install libcmocka-dev                     # to run `make test-core`
#
# Targets:
#   make               ardopb + the host-client apps (the default)
#   make ardopb        the modem
#   make apps          ardop-tx / ardop-rx / ardop-chat
#   make test-core     the in-process unit/integration tests (needs cmocka)
#   make check-pure    assert core/ has no mutable globals and no allocation
#   make check-headers assert every core/ header compiles standalone
#   make check-standalone  assert core/ links with only libm
#   make golden-core / golden-shell / golden-tx   conformance vs the frozen corpus
#   make clean

.PHONY: all core ardopb apps test-core check-pure check-headers check-standalone \
	golden-core golden-shell golden-tx clean

CC = gcc
CFLAGS = -g -MMD

# core/ is held to a strict standard: -Werror and no mutable global state
# (enforced by check-pure).  The shell/ and apps/ layers compose it under the
# same flags.
CORE_CFLAGS = -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes \
	-Wstrict-prototypes -Wshadow -Wconversion -Wcast-qual -Wwrite-strings
CORE_CPPFLAGS = -Icore

# --- core/ : the pure modem + protocol library -----------------------------
CORE_OBJS = \
	core/codec/crc.o \
	core/codec/dataframe.o \
	core/codec/frame.o \
	core/codec/locator.o \
	core/codec/packed6.o \
	core/codec/rs.o \
	core/codec/stationid.o \
	core/modem/busy.o \
	core/modem/demodulate.o \
	core/modem/fft.o \
	core/modem/goertzel.o \
	core/modem/modulate.o \
	core/modem/rxquality.o \
	core/link/session.o \
	core/link/quality.o \
	core/link/bandwidth.o \
	core/link/datamodes.o \
	core/link/frames.o \
	core/link/link.o

# The waveform templates are large const tables, not protocol logic, so they
# live outside CORE_OBJS (and outside check-pure) but link into everything.
TEMPLATES = core/modem/templates.o

core/%.o: core/%.c
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

# --- shell/ : the I/O-free runtime + the platform backends -----------------
SHELL_OBJS = shell/runtime.o shell/loop.o shell/host.o shell/telemetry.o

shell/%.o: shell/%.c
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

# The ALSA backend needs POSIX/GNU feature macros the strict -std=c11 withholds
# (struct timespec, alloca); the host transport and null backend need sockets /
# usleep.  These impure device files are the only place the macros appear.
shell/backend_alsa.o: shell/backend_alsa.c
	$(CC) -I. -D_GNU_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/host_tcp.o: shell/host_tcp.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/telemetry_tcp.o: shell/telemetry_tcp.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/backend_null.o: shell/backend_null.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

core: $(CORE_OBJS) $(SHELL_OBJS)

# --- ardopb : the modem ----------------------------------------------------
ARDOPB_OBJS = shell/main.o shell/backend_null.o shell/backend_alsa.o \
	shell/telemetry_tcp.o \
	shell/host_tcp.o

ardopb: $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(ARDOPB_OBJS)
	$(CC) $^ -o $@ -lasound -lm

# --- apps/ : host-client applications (plain TCP clients) ------------------
APPS = apps/ardop-tx apps/ardop-rx apps/ardop-chat

apps/%.o: apps/%.c
	$(CC) -Iapps $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

apps/ardop-tx:   apps/ardop_tx.o   apps/hostclient.o ; $(CC) $^ -o $@
apps/ardop-rx:   apps/ardop_rx.o   apps/hostclient.o ; $(CC) $^ -o $@
apps/ardop-chat: apps/ardop_chat.o apps/hostclient.o ; $(CC) $^ -o $@

apps: $(APPS)

all: ardopb apps

# --- mechanical guarantees on core/ ----------------------------------------
#
# check-pure: core/ has no mutable global state and no allocation, so it can be
# driven from an embedded shell or another language.  The test is which ELF
# *section* a symbol lands in: .data/.bss/*COM* are genuinely writable; .rodata
# and .data.rel.ro* (a const table with relocations) are read-only once loaded.
check-pure: $(CORE_OBJS)
	@fail=0; \
	for o in $(CORE_OBJS); do \
		syms=`objdump -t $$o \
			| grep -E '[[:space:]](\.data|\.bss|\*COM\*)[[:space:]]' \
			|| true`; \
		if [ -n "$$syms" ]; then \
			echo "FAIL: $$o has mutable global state:"; \
			echo "$$syms" | sed 's/^/    /'; fail=1; \
		fi; \
		alloc=`nm --undefined-only $$o \
			| grep -E ' U (malloc|calloc|realloc|free|strdup)$$' || true`; \
		if [ -n "$$alloc" ]; then \
			echo "FAIL: $$o allocates:"; \
			echo "$$alloc" | sed 's/^/    /'; fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "check-pure: $(words $(CORE_OBJS)) object(s), no mutable globals, no allocation"

# check-headers: every core/ header compiles standalone.
check-headers:
	@for h in `find core -name '*.h'`; do \
		echo "#include \"$${h#core/}\"" > /tmp/ardop-hdr-$$$$.c; \
		echo "int main(void){return 0;}" >> /tmp/ardop-hdr-$$$$.c; \
		$(CC) $(CORE_CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only \
			/tmp/ardop-hdr-$$$$.c || exit 1; \
		rm -f /tmp/ardop-hdr-$$$$.c; \
	done; \
	echo "check-headers: all core headers compile standalone"

# check-standalone: the whole core links into a program with only libm.  A core
# object that grew a dependency on ALSA, sockets or anything else fails here.
check-standalone: $(CORE_OBJS) $(TEMPLATES)
	@tmp=`mktemp -d`; \
	printf 'int main(void){return 0;}\n' > $$tmp/m.c; \
	if $(CC) $$tmp/m.c $(CORE_OBJS) $(TEMPLATES) -lm -o $$tmp/standalone \
			2>$$tmp/err; then \
		echo "check-standalone: core links with only -lm"; rm -rf $$tmp; \
	else \
		echo "FAIL: core needs more than libm:"; cat $$tmp/err; \
		rm -rf $$tmp; exit 1; \
	fi

# --- in-process tests ------------------------------------------------------
CORE_TESTS = \
	test/core/test_link \
	test/core/test_loopback \
	test/core/test_runtime \
	test/core/test_loop \
	test/core/test_host \
	test/core/test_telemetry

define newline


endef

test-core: $(CORE_TESTS)
	$(foreach test, $(CORE_TESTS), @echo $(test):$(newline)@$(test)$(newline))

test/core/setup.o: test/core/setup.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The tests link the core, the shell runtime and the templates; no old code.
test/core/test_%: test/core/test_%.c $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) \
		test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) test/core/setup.o \
		-lcmocka -lm -o $@

# --- golden-corpus conformance (see test/golden/README.md) -----------------
#
# The frozen corpus of ardopcf-generated TX audio and recordings is the
# regression net now that the reference implementation is gone: golden-core /
# golden-shell decode the recordings through the core and the assembled shell,
# and golden-tx checks the modulator's TX audio bit-for-bit.
test/golden/core_decode_wav: test/golden/core_decode_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) -lm -o $@
golden-core: test/golden/core_decode_wav
	cd test/golden && ./test_golden_core.py

test/golden/shell_decode_wav: test/golden/shell_decode_wav.c $(CORE_OBJS) \
		$(SHELL_OBJS) $(TEMPLATES)
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(SHELL_OBJS) $(TEMPLATES) -lm -o $@
golden-shell: test/golden/shell_decode_wav
	cd test/golden && GOLDEN_DECODE_BIN=./shell_decode_wav ./test_golden_core.py

test/golden/shell_tx_wav: test/golden/shell_tx_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) -lm -o $@
golden-tx: test/golden/shell_tx_wav
	cd test/golden && ./test_golden_tx.py

# Pull in -MMD dependency files so a changed header rebuilds its dependents.
-include core/*/*.d shell/*.d apps/*.d test/core/*.d

clean:
	rm -f -- ardopb $(APPS) \
		$(CORE_OBJS) $(CORE_OBJS:.o=.d) \
		$(TEMPLATES) $(TEMPLATES:.o=.d) \
		shell/*.o shell/*.d apps/*.o apps/*.d \
		test/core/*.o test/core/*.d $(CORE_TESTS) \
		test/golden/core_decode_wav test/golden/shell_decode_wav \
		test/golden/shell_tx_wav test/golden/*.d
