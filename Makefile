# ardopb -- a rebuilt ARDOP HF data modem (see README.md).
#
# Build requirements (Debian/Ubuntu):
#   sudo apt install build-essential libasound2-dev   # ardopb + apps
#   sudo apt install libcmocka-dev                     # to run `make test-core`
#
# On Windows, build in an MSYS2 MINGW64 shell (see .github/workflows/test.yml
# for the exact package list). Binaries get a .exe suffix; check-pure,
# check-headers and check-standalone are Linux-hosted and refuse to run there.
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

.PHONY: all core apps test-core check-pure check-headers check-standalone \
	golden-core golden-shell golden-tx clean

# `all` is the default goal explicitly. Without this the first non-pattern rule
# in the file wins -- which was shell/backend_alsa.o, so a bare `make` built one
# object and stopped, and CI's "build" step proved nothing about ardopb or apps.
.DEFAULT_GOAL := all

CC = gcc
CFLAGS = -g -MMD $(PLATFORM_CPPFLAGS)

# --- host detection --------------------------------------------------------
#
# The Windows build is MSYS2 + MinGW-w64 (see .github/workflows/test.yml), so
# the strict flag set below is the same compiler's and needs no translation.
# Only three things actually differ: the executable suffix, the socket library,
# and which audio backend exists.
ifeq ($(OS),Windows_NT)
EXE = .exe
# MinGW's printf is msvcrt's unless asked otherwise, and msvcrt has no %zu --
# it prints the literal text "zu". shell/ and test/golden/ use %zu throughout.
PLATFORM_CPPFLAGS = -D__USE_MINGW_ANSI_STDIO=1
PLATFORM_LDLIBS = -lws2_32
# ardopb and the apps ship as single copyable files; only the Qt GUI needs DLLs.
STATIC = -static
# miniaudio only: there is no ALSA here. WASAPI needs ole32/avrt; winmm is the
# fallback backend. miniaudio loads them at run time, but the stubs are linked.
AUDIO_BACKEND_OBJS = $(MA_OBJS)
AUDIO_LDLIBS = -lole32 -lwinmm -lavrt
else
EXE =
PLATFORM_CPPFLAGS =
# sys.c uses pthreads for the timed semaphore the audio callbacks post to.
PLATFORM_LDLIBS = -lpthread
STATIC =
# Both backends are built on Linux and chosen at run time: ALSA stays the
# headless daemon's path (its dependency footprint is a feature), miniaudio is
# what the desktop app and every other platform use. miniaudio dlopen()s ALSA
# and PulseAudio, hence -ldl.
AUDIO_BACKEND_OBJS = shell/backend_alsa.o $(MA_OBJS)
AUDIO_LDLIBS = -lasound -ldl
endif

# The miniaudio backend and everything it needs. ptt.o is here rather than in
# SHELL_OBJS because it talks to serial ports and rigctld -- it is device code.
MA_OBJS = shell/backend_ma.o shell/ma_impl.o shell/audio_devices.o shell/ptt.o

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
SHELL_OBJS = shell/runtime.o shell/loop.o shell/host.o shell/telemetry.o \
	shell/ring.o shell/resample.o shell/net.o shell/sys.o

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
# net.c and sys.c are the two files that include an OS header, so they are the
# two that need the feature macros: getaddrinfo, sigaction, CLOCK_MONOTONIC.
shell/net.o: shell/net.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/sys.o: shell/sys.c
	$(CC) -I. -D_GNU_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/ptt.o: shell/ptt.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

# shell/ma_impl.c is the ONE translation unit in the tree not held to
# -Wall -Wextra -Werror. It contains a single #include of the vendored
# miniaudio -- ~93,000 lines that were not written to this project's bar and
# never will be. Containing it in one file is the price of not writing four
# native audio backends; see third_party/README.md. Everything we wrote against
# it (backend_ma.c, audio_devices.c) is held to the full bar as usual.
shell/ma_impl.o: shell/ma_impl.c third_party/miniaudio.h
	$(CC) -I. -std=gnu11 -w -g -MMD -c -o $@ $<
shell/backend_ma.o: shell/backend_ma.c
	$(CC) -I. -D_GNU_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
shell/audio_devices.o: shell/audio_devices.c
	$(CC) -I. -D_GNU_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

core: $(CORE_OBJS) $(SHELL_OBJS)

# --- ardopb : the modem ----------------------------------------------------
ARDOPB_OBJS = shell/main.o shell/backend_null.o \
	shell/telemetry_tcp.o \
	shell/host_tcp.o \
	$(AUDIO_BACKEND_OBJS)

ardopb$(EXE): $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(ARDOPB_OBJS)
	$(CC) $(STATIC) $^ -o $@ $(AUDIO_LDLIBS) $(PLATFORM_LDLIBS) -lm

# A convenience alias so `make ardopb` works where the real file is ardopb.exe.
# Guarded, because on Linux the two names are the same and it would be circular.
ifneq ($(EXE),)
.PHONY: ardopb
ardopb: ardopb$(EXE)
endif

# --- apps/ : host-client applications (plain TCP clients) ------------------
APPS = apps/ardop-tx$(EXE) apps/ardop-rx$(EXE) apps/ardop-chat$(EXE)

# -I. so the apps can reach shell/net.h and shell/sys.h. They remain plain host
# clients with no dependency on the modem's C API -- net/sys are a platform
# shim, and a second copy of a Winsock wrapper would be strictly worse than
# sharing this one.
apps/%.o: apps/%.c
	$(CC) -Iapps -I. $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

APP_OBJS = apps/hostclient.o shell/net.o shell/sys.o
APP_LINK = $(CC) $(STATIC) $^ -o $@ $(PLATFORM_LDLIBS)
apps/ardop-tx$(EXE):   apps/ardop_tx.o   $(APP_OBJS) ; $(APP_LINK)
apps/ardop-rx$(EXE):   apps/ardop_rx.o   $(APP_OBJS) ; $(APP_LINK)
apps/ardop-chat$(EXE): apps/ardop_chat.o $(APP_OBJS) ; $(APP_LINK)

apps: $(APPS)

all: ardopb$(EXE) apps

# --- mechanical guarantees on core/ ----------------------------------------
#
# These three read ELF with objdump/nm and are Linux-hosted. They prove
# properties of source that every platform shares, so running them once on the
# Linux CI job proves them for the Windows build too (analysis/14 Decision 6).
#
# On Windows they refuse rather than pass. core/README.md is explicit that a
# rule which cannot be mechanically enforced is only guidance -- so a developer
# there must not be able to believe they ran the check when they did not.
ifeq ($(OS),Windows_NT)
check-pure check-headers check-standalone:
	@echo "$@: Linux-hosted (objdump/nm on ELF sections). It proves a property"; \
	echo "of shared source and is run once, on the Linux CI job. Not run here."; \
	exit 1
else

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

endif   # Windows_NT

# --- in-process tests ------------------------------------------------------
CORE_TESTS = \
	test/core/test_link$(EXE) \
	test/core/test_loopback$(EXE) \
	test/core/test_runtime$(EXE) \
	test/core/test_loop$(EXE) \
	test/core/test_host$(EXE) \
	test/core/test_telemetry$(EXE) \
	test/core/test_ring$(EXE) \
	test/core/test_resample$(EXE) \
	test/core/test_backend_ma$(EXE)

define newline


endef

test-core: $(CORE_TESTS)
	$(foreach test, $(CORE_TESTS), @echo $(test):$(newline)@$(test)$(newline))

test/core/setup.o: test/core/setup.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The ring's ordering guarantee needs two threads and a sanitizer, so it is not
# part of test-core. Linux-hosted for the same reason check-pure is: it proves a
# property of portable source, and MinGW has no ThreadSanitizer.
.PHONY: test-ring-tsan
test-ring-tsan: test/core/stress_ring.c shell/ring.c
	@$(CC) -std=c11 -Wall -Wextra -Werror -I. $(CORE_CPPFLAGS) -g -O1 \
		-fsanitize=thread $^ -o test/core/stress_ring -lpthread
	@test/core/stress_ring

# The tests link the core, the shell runtime and the templates; no old code.
test/core/test_%$(EXE): test/core/test_%.c $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) \
		test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) test/core/setup.o \
		-lcmocka $(PLATFORM_LDLIBS) -lm -o $@

# The miniaudio backend test needs the backend and its vendored implementation,
# which are not in SHELL_OBJS. It runs against miniaudio's synthetic null
# device, so it needs no sound card and passes identically in CI on both hosts.
test/core/test_backend_ma$(EXE): test/core/test_backend_ma.c $(CORE_OBJS) \
		$(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) \
		test/core/setup.o \
		-lcmocka $(AUDIO_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# --- golden-corpus conformance (see test/golden/README.md) -----------------
#
# The frozen corpus of ardopcf-generated TX audio and recordings is the
# regression net now that the reference implementation is gone: golden-core /
# golden-shell decode the recordings through the core and the assembled shell,
# and golden-tx checks the modulator's TX audio bit-for-bit.
test/golden/core_decode_wav$(EXE): test/golden/core_decode_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) -lm -o $@
golden-core: test/golden/core_decode_wav$(EXE)
	cd test/golden && ./test_golden_core.py

test/golden/shell_decode_wav$(EXE): test/golden/shell_decode_wav.c $(CORE_OBJS) \
		$(SHELL_OBJS) $(TEMPLATES)
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(SHELL_OBJS) $(TEMPLATES) -lm -o $@
golden-shell: test/golden/shell_decode_wav$(EXE)
	cd test/golden && GOLDEN_DECODE_BIN=./shell_decode_wav$(EXE) ./test_golden_core.py

test/golden/shell_tx_wav$(EXE): test/golden/shell_tx_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) -lm -o $@
golden-tx: test/golden/shell_tx_wav$(EXE)
	cd test/golden && ./test_golden_tx.py

# Pull in -MMD dependency files so a changed header rebuilds its dependents.
-include core/*/*.d shell/*.d apps/*.d test/core/*.d

clean:
	rm -f -- ardopb$(EXE) $(APPS) \
		$(CORE_OBJS) $(CORE_OBJS:.o=.d) \
		$(TEMPLATES) $(TEMPLATES:.o=.d) \
		shell/*.o shell/*.d apps/*.o apps/*.d \
		test/core/*.o test/core/*.d $(CORE_TESTS) \
		test/core/stress_ring$(EXE) \
		test/golden/core_decode_wav$(EXE) test/golden/shell_decode_wav$(EXE) \
		test/golden/shell_tx_wav$(EXE) test/golden/*.d
