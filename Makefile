# ardopb -- a rebuilt ARDOP HF data modem (see README.md).
#
# Build requirements (Debian/Ubuntu):
#   sudo apt install build-essential                  # ardopb + apps
#   sudo apt install libcmocka-dev                     # to run `make test-core`
#
# On Windows, build in an MSYS2 MINGW64 shell (see .github/workflows/test.yml
# for the exact package list). Binaries get a .exe suffix; check-pure,
# check-headers and check-standalone are Linux-hosted and refuse to run there.
#
# From Linux, cross-compiling catches Windows-only defects without waiting for
# CI -- notably the LLP64 `long` width, where a signed/unsigned mix involving
# uint32_t warns on Windows and is silently fine here:
#   sudo apt install gcc-mingw-w64-x86-64
#   make CC=x86_64-w64-mingw32-gcc
#
# Targets:
#   make               ardopb + the host-client apps (the default)
#   make ardopb        the modem
#   make apps          ardop-cat / ardop-chat
#   make test-core     the in-process unit/integration tests (needs cmocka)
#   make check-pure    assert core/ has no mutable globals and no allocation
#   make check-headers assert every core/ header compiles standalone
#   make check-standalone  assert core/ links with only libm
#   make golden-core / golden-shell / golden-tx   conformance vs the frozen corpus
#   make clean

.PHONY: all core apps app lib test-core check-pure check-headers check-standalone \
	golden-core golden-shell golden-tx golden-resample clean

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
#
# A cross-compile counts as Windows too:
#
#   make CC=x86_64-w64-mingw32-gcc
#
# which is worth having on a Linux workstation. `long` is 32 bits on Windows
# and 64 on Linux, so a signed/unsigned mix involving uint32_t warns on one and
# is silently fine on the other -- a class of defect that otherwise only ever
# surfaces in CI.
WINDOWS := $(if $(findstring mingw,$(CC)),1,$(if $(filter Windows_NT,$(OS)),1,))

ifeq ($(WINDOWS),1)
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
# SetupAPI enumerates HID interfaces for CM108 keying. hid.dll itself is loaded
# at run time, so it needs no import library. Both ship with Windows: this is a
# link addition, not a dependency an operator installs.
PTT_LDLIBS = -lsetupapi
else
EXE =
PLATFORM_CPPFLAGS =
# sys.c uses pthreads for the timed semaphore the audio callbacks post to.
PLATFORM_LDLIBS = -lpthread
STATIC =
# One audio backend on every platform. miniaudio dlopen()s ALSA, PulseAudio and
# JACK at run time rather than linking them, so there is no -lasound and no
# libasound2-dev build dependency -- a smaller footprint than the dedicated
# ALSA backend this replaced, not a larger one.
AUDIO_BACKEND_OBJS = $(MA_OBJS)
AUDIO_LDLIBS = -ldl
PTT_LDLIBS =
endif

# The miniaudio backend and everything it needs. ptt.o is here rather than in
# SHELL_OBJS because it talks to serial ports and rigctld -- it is device code.
MA_OBJS = shell/backend_ma.o shell/ma_impl.o shell/audio_devices.o \
	shell/ptt.o shell/ptt_cat.o shell/ptt_cm108.o shell/usbtopo.o \
	shell/serialports.o \
	shell/radios.o

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
	shell/capture.o shell/wavwriter.o shell/ring.o shell/resample.o \
	shell/net.o shell/sys.o shell/fault.o shell/settings.o shell/build.o

shell/%.o: shell/%.c
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

#
# The build identifier, for a fault report.
#
# Two problems, and the stamp file solves both. The identifier changes on every
# commit, so a -D on the global flags would rebuild the whole tree each time;
# and make cannot see that the value changed, so without a file it would rebuild
# nothing. The recipe therefore writes the value to a stamp only when it
# differs, and one object depends on the stamp.
#
# A release tarball has no git repository. `git describe` fails there and the
# identifier becomes "unknown", which is the honest answer.
#
# --match 'v*' is load-bearing, and a plain --tags is wrong here for two
# reasons:
#
#   1. `continuous` is a *moving* tag. The publish job runs `git tag -f
#      continuous` on every push to main, so a count of commits after it means
#      something different tomorrow, and two machines with different fetch
#      states print different strings for the same commit.
#   2. The tags 1.0.4.1.3 and 2.0.3.2.1 are inherited ardopcf release tags. A
#      build of this software must not report one of them as its own version.
#
# So only a `v*` tag counts. Until this project makes one, --always supplies the
# abbreviated commit, which is the whole of what is known.
#
BUILD_ID   := $(shell git describe --tags --always --dirty --match 'v*' 2>/dev/null || echo unknown)
BUILD_DATE := $(shell date -u +%Y-%m-%d 2>/dev/null || echo unknown)

# FORCE rather than marking the stamp itself .PHONY. A .PHONY target is always
# out of date, so shell/build.o would rebuild -- and everything would relink --
# on every single make. With FORCE the recipe still runs every time, but it only
# touches the stamp when the value differs, so the timestamp is what carries the
# information and one object rebuilds only on a new commit.
shell/build_id.stamp: FORCE
	@printf '%s' '$(BUILD_ID)' > $@.new
	@if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi

FORCE:
.PHONY: FORCE

shell/build.o: shell/build.c shell/build_id.stamp
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		-DARDOP_BUILD_ID='"$(BUILD_ID)"' \
		-DARDOP_BUILD_DATE='"$(BUILD_DATE)"' -c -o $@ $<

# The impure device files need POSIX/GNU feature macros the strict -std=c11
# withholds: sockets, usleep, struct timespec, CLOCK_MONOTONIC. These are the
# only place the macros appear.
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
# ptt_cm108.c reads sysfs (dirent) on Linux and SetupAPI on Windows; ptt_cat.c is
# pure and needs nothing, but shares the rule for symmetry.
shell/ptt_cm108.o: shell/ptt_cm108.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
# serialports.c walks sysfs on Linux and SetupAPI on Windows.
shell/serialports.o: shell/serialports.c
	$(CC) -I. -D_DEFAULT_SOURCE $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<
# usbtopo.c walks sysfs: dirent and readlink.
shell/usbtopo.o: shell/usbtopo.c
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
	$(CC) $(STATIC) $^ -o $@ $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm

# A convenience alias so `make ardopb` works where the real file is ardopb.exe.
# Guarded, because on Linux the two names are the same and it would be circular.
ifneq ($(EXE),)
.PHONY: ardopb
ardopb: ardopb$(EXE)
endif

# --- apps/ : host-client applications (plain TCP clients) ------------------
APPS = apps/ardop-cat$(EXE) apps/ardop-chat$(EXE) apps/ardop-pcap-dump$(EXE)

# -I. so the apps can reach shell/net.h and shell/sys.h. They remain plain host
# clients with no dependency on the modem's C API -- net/sys are a platform
# shim, and a second copy of a Winsock wrapper would be strictly worse than
# sharing this one.
# -Icore since apps/fecchat.c asks core/codec/frame.h how many bytes a frame
# of the chosen FEC mode holds.
apps/%.o: apps/%.c
	$(CC) -Iapps -I. -Icore $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

APP_OBJS = apps/hostclient.o shell/net.o shell/sys.o shell/build.o
APP_LINK = $(CC) $(STATIC) $^ -o $@ $(PLATFORM_LDLIBS)
# The one app that links a piece of the protocol: app/asp_wire.o, purely so
# that asp_looks_like_hello and the HELLO it looks for cannot drift apart.
apps/ardop-cat$(EXE):  apps/ardop_cat.o  app/asp_wire.o $(APP_OBJS) ; $(APP_LINK)
# core/codec/frame.o for the FEC profile: it needs to know how many bytes a
# frame of the chosen mode holds, which is the budget a TEXT_B has to fit in.
apps/ardop-chat$(EXE): apps/ardop_chat.o apps/fecchat.o app/asp_wire.o \
                       core/codec/frame.o $(APP_OBJS) ; $(APP_LINK)
# No sockets, so not $(APP_OBJS) -- just the pure pcap/record decode, the
# frame-type name table, and sys.o for ardop_wall_ms (linked in with
# capture.o's impure half even though this tool only reads, never writes).
apps/ardop-pcap-dump$(EXE): apps/ardop_pcap_dump.o core/codec/frame.o \
                            shell/capture.o shell/sys.o
	$(CC) $(STATIC) $^ -o $@ $(PLATFORM_LDLIBS)

apps: $(APPS)

# --- app/ : the station application's embedding spine ----------------------
#
# analysis/14 workstream A. Two groups, because they have different futures.
#
# SPINE_OBJS is the spine itself: portable C11, no sockets, no feature-test
# macros, no UI. It is what the eventual CMake app build compiles too, which is
# why the TNC transport is held behind an ops table rather than linked here.
#
# SPINE_HARNESS is the phase-1 driver -- a script reader, an in-memory loopback
# and the host_tcp adapter. The shipping application will not link it.
#
# Not APP_OBJS: that name belongs to apps/ (the host-client programs) forty
# lines up.
# Three groups, because they have three futures.
#
# SPINE_OBJS is the seam: portable C11, no sockets, no feature-test macros, no
#   devices. app/%.o gets no -D_DEFAULT_SOURCE or -D_GNU_SOURCE, so a violation
#   fails to compile rather than being noticed in review.
# SPINE_DEVICE_OBJS owns the sound card and the keying line. Portable C11 too,
#   but it names the miniaudio backend and the PTT object, so a platform that
#   gets its audio elsewhere replaces this one file and keeps the seam.
# ASP_OBJS is the application protocol (analysis/17). Pure: no sockets, no
#   files, no devices, no allocation -- which is why it can be built and proved
#   before any of them exist, and why the harness can carry a second decoder to
#   check this one against.
# ASP_APP_OBJS is the only file that names both the protocol and the spine, so
#   neither has to know about the other -- app/asp.c still compiles with no
#   spine in sight and app/spine.c has never heard of a file transfer. Same
#   reason SPINE_DEVICE_OBJS is its own group.
# SPINE_TNC_OBJS hosts the TNC interface for guest clients. Its own group
#   because it is the one part of the seam that needs sockets, and because both
#   the harness and the shipping application link it -- the application is what
#   Pat and Winlink connect to, which is the point of hosting it at all.
# SPINE_HARNESS is the phase-1 driver. The shipping application will not link it.
SPINE_OBJS        = app/spine.o app/ring.o
ASP_OBJS          = app/asp_wire.o app/asp.o
ASP_APP_OBJS      = app/asp_app.o
SPINE_DEVICE_OBJS = app/devices.o
SPINE_TNC_OBJS    = app/tnc_host_tcp.o
SPINE_HARNESS     = app/main.o app/script.o app/loopback.o

app/%.o: app/%.c
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

app/ardop-spine$(EXE): $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(SPINE_OBJS) \
		$(ASP_OBJS) $(ASP_APP_OBJS) \
		$(SPINE_DEVICE_OBJS) $(SPINE_TNC_OBJS) $(SPINE_HARNESS) \
		shell/backend_null.o \
		shell/host_tcp.o $(AUDIO_BACKEND_OBJS)
	$(CC) $(STATIC) $^ -o $@ $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm

app: app/ardop-spine$(EXE)

# --- libardop.a : the whole C program, for the Qt build to link -------------
#
# The graphical application is built by CMake (analysis/14 Decision 6), and CMake
# needs to know which objects to link. Listing them there would be a second
# definition of a set that already exists here, and analysis/15 predicted exactly
# what happens next: it drifts.
#
# So the Makefile stays the authority and hands CMake one artifact. Adding a file
# to any group above is picked up by the Qt build with no CMake change at all.
ARDOP_LIB_OBJS = $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(SPINE_OBJS) \
	$(ASP_OBJS) $(ASP_APP_OBJS) $(SPINE_DEVICE_OBJS) $(SPINE_TNC_OBJS) \
	shell/host_tcp.o \
	shell/backend_null.o $(AUDIO_BACKEND_OBJS)

libardop.a: $(ARDOP_LIB_OBJS)
	$(AR) rcs $@ $^

.PHONY: lib
lib: libardop.a

all: ardopb$(EXE) apps app

# --- mechanical guarantees on core/ ----------------------------------------
#
# These three read ELF with objdump/nm and are Linux-hosted. They prove
# properties of source that every platform shares, so running them once on the
# Linux CI job proves them for the Windows build too (analysis/14 Decision 6).
#
# On Windows they refuse rather than pass. core/README.md is explicit that a
# rule which cannot be mechanically enforced is only guidance -- so a developer
# there must not be able to believe they ran the check when they did not.
ifeq ($(WINDOWS),1)
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

endif   # WINDOWS

# --- in-process tests ------------------------------------------------------
CORE_TESTS = \
	test/core/test_link$(EXE) \
	test/core/test_loopback$(EXE) \
	test/core/test_runtime$(EXE) \
	test/core/test_loop$(EXE) \
	test/core/test_host$(EXE) \
	test/core/test_telemetry$(EXE) \
	test/core/test_capture$(EXE) \
	test/core/test_wavwriter$(EXE) \
	test/core/test_ring$(EXE) \
	test/core/test_resample$(EXE) \
	test/core/test_spine$(EXE) \
	test/core/test_audio_devices$(EXE) \
	test/core/test_settings$(EXE) \
	test/core/test_devices$(EXE) \
	test/core/test_ptt$(EXE) \
	test/core/test_usbtopo$(EXE) \
	test/core/test_backend_ma$(EXE) \
	test/core/test_asp_wire$(EXE) \
	test/core/test_fecchat$(EXE) \
	test/core/test_asp$(EXE)

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

# The same argument one layer out: analysis/14's "nothing outside the modem
# thread touches the runtime, for any reason, ever" is a claim about what does
# not happen, which no single-threaded test can support.
#
# Sources, not the pre-built objects, and that is the whole point. The sanitizer
# only sees races in instrumented code, so for core/ and shell/runtime.c's
# silence to be evidence rather than an artefact, they have to be compiled under
# it. Everything listed is portable C11 needing no feature-test macro -- the
# property analysis/14 asserts of this set -- which is what lets the full warning
# bar stay on. net.c, sys.c, ring.c and resample.c are absent because none is
# reachable from a loopback-backed, socket-free spine.
# shell/build.c is here because shell/host.c answers the VERSION command with
# ardop_build_id(). It compiles from source like the rest of this list, so it
# gets no -DARDOP_BUILD_ID and reports "unknown" -- which is correct: the
# sanitiser binary is a test harness, not a build anybody runs a station on.
TSAN_SRCS = $(CORE_OBJS:.o=.c) $(TEMPLATES:.o=.c) \
	shell/runtime.c shell/loop.c shell/host.c shell/telemetry.c \
	shell/build.c app/spine.c app/ring.c app/loopback.c

.PHONY: test-app-tsan
test-app-tsan: test/core/stress_spine.c $(TSAN_SRCS)
	@$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -O1 \
		-fsanitize=thread $^ -o test/core/stress_spine -lpthread -lm
	@test/core/stress_spine

# test_audio_devices needs the enumeration half of MA_OBJS: the resolution rule
# is pure, but the test also enumerates against miniaudio's synthetic backend so
# it exercises the real path on both hosts with no sound card.
test/core/test_audio_devices$(EXE): test/core/test_audio_devices.c $(CORE_OBJS) \
		$(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) \
		test/core/setup.o \
		-lcmocka $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# test_usbtopo needs the topology walk and the radio table, both in MA_OBJS.
test/core/test_usbtopo$(EXE): test/core/test_usbtopo.c $(CORE_OBJS) \
		$(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) \
		test/core/setup.o \
		-lcmocka $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# test_ptt needs the keying objects, which live in MA_OBJS.
test/core/test_ptt$(EXE): test/core/test_ptt.c $(CORE_OBJS) $(TEMPLATES) \
		$(SHELL_OBJS) $(MA_OBJS) test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(MA_OBJS) \
		test/core/setup.o \
		-lcmocka $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# test_devices links the manager, so it needs MA_OBJS as well -- the null device
# gives a real backend with a real device thread, which is what makes the
# recovery path testable in CI with no hardware.
test/core/test_devices$(EXE): test/core/test_devices.c $(CORE_OBJS) $(TEMPLATES) \
		$(SHELL_OBJS) $(SPINE_OBJS) $(SPINE_DEVICE_OBJS) $(MA_OBJS) \
		test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(SPINE_OBJS) \
		$(SPINE_DEVICE_OBJS) $(MA_OBJS) test/core/setup.o \
		-lcmocka $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# The spine test needs app/ as well. It lives in test/core/ rather than a
# test/app/ of its own because that directory is really "the in-process suite",
# and a second directory would need a duplicate of the generic rule below and a
# second CI step to run it.
# The FEC broadcast profile lives in apps/ (it is a property of the chat tool,
# not of the station application), but it is pure and belongs in this suite.
test/core/test_fecchat$(EXE): test/core/test_fecchat.c apps/fecchat.o \
		$(ASP_OBJS) core/codec/frame.o test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Iapps -Itest/core $(CFLAGS) \
		$< apps/fecchat.o $(ASP_OBJS) core/codec/frame.o \
		test/core/setup.o \
		-lcmocka $(PLATFORM_LDLIBS) -lm -o $@

test/core/test_asp$(EXE) test/core/test_asp_wire$(EXE): \
		test/core/test_asp%$(EXE): test/core/test_asp%.c $(ASP_OBJS) \
		test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(ASP_OBJS) test/core/setup.o \
		-lcmocka $(PLATFORM_LDLIBS) -lm -o $@

test/core/test_spine$(EXE): test/core/test_spine.c $(CORE_OBJS) $(TEMPLATES) \
		$(SHELL_OBJS) $(SPINE_OBJS) app/loopback.o test/core/setup.o
	$(CC) $(CORE_CPPFLAGS) -I. -Itest/core $(CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(SHELL_OBJS) $(SPINE_OBJS) \
		app/loopback.o test/core/setup.o \
		-lcmocka $(PLATFORM_LDLIBS) -lm -o $@

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
		-lcmocka $(AUDIO_LDLIBS) $(PTT_LDLIBS) $(PLATFORM_LDLIBS) -lm -o $@

# --- golden-corpus conformance (see test/golden/README.md) -----------------
#
# The frozen corpus of ardopcf-generated TX audio and recordings is the
# regression net now that the reference implementation is gone: golden-core /
# golden-shell decode the recordings through the core and the assembled shell,
# and golden-tx checks the modulator's TX audio bit-for-bit.
test/golden/core_decode_wav$(EXE): test/golden/core_decode_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(PLATFORM_LDLIBS) -lm -o $@
golden-core: test/golden/core_decode_wav$(EXE)
	cd test/golden && ./test_golden_core.py

test/golden/shell_decode_wav$(EXE): test/golden/shell_decode_wav.c $(CORE_OBJS) \
		$(SHELL_OBJS) $(TEMPLATES)
	$(CC) -I. $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(SHELL_OBJS) $(TEMPLATES) $(PLATFORM_LDLIBS) \
		-lm -o $@
# The resampler against real modulated audio, not synthetic tones. See the
# harness comment: -alias adds an out-of-band interferer that naive decimation
# folds into the middle of the passband, so a decimator with no anti-alias filter
# passes golden-resample and fails golden-resample-alias.
test/golden/resample_decode_wav$(EXE): test/golden/resample_decode_wav.c \
		$(CORE_OBJS) $(SHELL_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) -I. $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(SHELL_OBJS) $(TEMPLATES) $(PLATFORM_LDLIBS) \
		-lm -o $@

.PHONY: golden-resample golden-resample-alias
golden-resample: test/golden/resample_decode_wav$(EXE)
	@for m in 2 4 8; do \
		echo "decimation m=$$m:"; \
		( cd test/golden && \
		  GOLDEN_DECODE_BIN="./resample_decode_wav$(EXE) -m $$m" \
			./test_golden_core.py ) || exit 1; \
	done

golden-resample-alias: test/golden/resample_decode_wav$(EXE)
	cd test/golden && \
		GOLDEN_DECODE_BIN="./resample_decode_wav$(EXE) -m 4 -alias" \
		./test_golden_core.py

golden-shell: test/golden/shell_decode_wav$(EXE)
	cd test/golden && GOLDEN_DECODE_BIN=./shell_decode_wav$(EXE) ./test_golden_core.py

test/golden/shell_tx_wav$(EXE): test/golden/shell_tx_wav.c $(CORE_OBJS) $(TEMPLATES)
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) \
		$< $(CORE_OBJS) $(TEMPLATES) $(PLATFORM_LDLIBS) -lm -o $@
golden-tx: test/golden/shell_tx_wav$(EXE)
	cd test/golden && ./test_golden_tx.py

# Pull in -MMD dependency files so a changed header rebuilds its dependents.
-include core/*/*.d shell/*.d apps/*.d app/*.d test/core/*.d

clean:
	rm -f -- ardopb$(EXE) $(APPS) app/ardop-spine$(EXE) libardop.a \
		$(CORE_OBJS) $(CORE_OBJS:.o=.d) \
		$(TEMPLATES) $(TEMPLATES:.o=.d) \
		shell/*.o shell/*.d apps/*.o apps/*.d app/*.o app/*.d \
		test/core/*.o test/core/*.d $(CORE_TESTS) \
		test/core/stress_ring$(EXE) test/core/stress_spine$(EXE) \
		test/golden/core_decode_wav$(EXE) test/golden/shell_decode_wav$(EXE) \
		test/golden/shell_tx_wav$(EXE) test/golden/resample_decode_wav$(EXE) \
		test/golden/*.d
