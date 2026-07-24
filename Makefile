#	ardopcf Makefile
#		For Linux, the default build requires gcc, make, and development
#		libraries for ALSA.
#			sudo apt install build-essential libasound2-dev
#			make
#
#		Fow Windows, the default build requires installation of a MinGW build
#		environment.  The easily installable packages from https://winlibs.com
#		available for 32-bit or 64-bit builds are suggested.  These are used to
#		build the Windows releases.  Installing these in `C:\Program Files` is
#		not recommended since that may require admin privileges.  Other build
#		environments may also work but are not tested.
#			mingw32-make
#
#	`make test` which builds the executable and also runs some tests also
#	requires installation of cmocka, which is not required for the default build.
#		On Debian/Ubuntu this is easily installed with:
#			sudo apt install libcmocka-dev
#
#		Package managers for other Linux distributions are also likely to
#		provide easy installation of cmocka.
#
#		In the following description of how to install cmocka for Windows, a
#		winlibs MinGW installation is assumed to be located at `C:\winlibs`
#		If installed elsewhere, substitute the appropriate path.  Putting the
#		cmocka files into the winlibs install directory avoids the need for further
#		configuration.  This uses git (available from https://git-scm.com/downloads/win)
#		to download the cmocka source code.
#
#		git clone https://git.cryptomilk.org/projects/cmocka.git
#		cd cmocka
#		mkdir build
#		cd build
#		cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="C:\winlibs" ..
#		mingw32-make
#		mingw32-make install
#
#	To cross-compile for Windows on Linux,
#		sudo apt install mingw-w64
#		make CC_NATIVE=gcc CC=i686-w64-mingw32-gcc-posix WIN32=1

.PHONY: all buildtest test golden golden-regen core check-pure check-headers test-core

# list all object files and their directories
# keep sorted by filename
OBJS = \
	lib/rawhid/rawhid.o \
	lib/rockliff/rrs.o \
	lib/ws_server/ws_server.o \
	src/common/ARDOPC.o \
	src/common/ARDOPCommon.o \
	src/common/ardopSampleArrays.o \
	src/common/ARQ.o \
	src/common/BusyDetect.o \
	src/common/FEC.o \
	src/common/FFT.o \
	src/common/HostInterface.o \
	src/common/Locator.o \
	src/common/log_file.o \
	src/common/log.o \
	src/common/Modulate.o \
	src/common/Packed6.o \
	src/common/RXO.o \
	src/common/sdft.o \
	src/common/SoundInput.o \
	src/common/StationId.o \
	src/common/TCPHostInterface.o \
	src/common/txframe.o \
	src/common/wav.o \
	src/common/gen-webgui.html.o \
	src/common/gen-webgui.js.o \
	src/common/Webgui.o \
	src/common/noise.o \

# Linux-only object files
OBJS_LIN = \
	src/linux/ALSASound.o \
	src/linux/LinSerial.o \

# Windows-only object files
OBJS_WIN = \
	src/windows/Waveout.o \
	lib/hid/hid.o \

# user-facing executables, like ardopcf
OBJS_EXE = \
	src/common/ardopcf.o \

# unit test executables
TESTS = \
	test/ardop/test_ARDOPCommon \
	test/ardop/test_HostInterface \
	test/ardop/test_Locator \
	test/ardop/test_log \
	test/ardop/test_Packed6 \
	test/ardop/test_StationId \

# unit test common code
TEST_OBJS_COMMON = \
	test/ardop/setup.o \

# core/ -- the rebuilt pure layers.  See core/README.md.
# These are built to a stricter standard than the inherited tree: -Werror,
# and no mutable global state (enforced by `make check-pure`).  They are not
# yet linked into ardopcf.
CORE_OBJS = \
	core/codec/crc.o \
	core/codec/frame.o \
	core/codec/locator.o \
	core/codec/packed6.o \
	core/codec/rs.o \
	core/codec/stationid.o \
	core/modem/modulate.o \

CORE_TESTS = \
	test/core/test_crc \
	test/core/test_frame \
	test/core/test_locator \
	test/core/test_packed6 \
	test/core/test_rs \
	test/core/test_stationid \
	test/core/test_modulate \

# define newline for use with foreach to run tests
define newline


endef

# Configuration:
CPPFLAGS += -Isrc -Ilib
CFLAGS = -g -MMD
LDLIBS = -lm -lpthread
LDFLAGS = -Xlinker -Map=output.map
CC = gcc
CC_NATIVE ?= $(CC)

# How to wrap a symbol with ld
LDWRAP := -Wl,--wrap=

# Path to txt2c executable; will be built if it does not already exist
TXT2C ?=

# Set WIN32 to non-empty to cross-compile on Linux.
# Leave empty for OS auto-detection
WIN32 ?= $(filter $(OS),Windows_NT)

ifneq ($(WIN32),)
OBJS += $(OBJS_WIN)
LDLIBS += -lwsock32 -lwinmm -lsetupapi -lws2_32
else
OBJS += $(OBJS_LIN)
LDLIBS += -lrt -lasound
endif

all: ardopcf

ardopcf: $(OBJS_EXE) $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LOADLIBES) $(LDLIBS)

# if txt2c is not provided, build it
ifeq ($(TXT2C),)
TXT2C := lib/txt2c/txt2c

# build txt2c directly and without our link libraries (none are required)
$(TXT2C): $(TXT2C).c
	$(CC_NATIVE) $^ -o $@

# mark build products for cleaning
CLEAN += $(TXT2C) $(TXT2C).exe
endif

# Use txt2c to convert webgui/FOO.xyz → FOO.xyz.c
#   The C symbol name will be FOO_xyz.
#   This is used to convert HTML and JavaScript to C sources.
#   The implicit rule will then compile them to FOO.xyz.o.
src/common/gen-%.c:: webgui/% | $(TXT2C)
	$(TXT2C) $< $@ $(subst .,_,$(notdir $<))

# `make buildtest` builds the test-case executables but does not run them
buildtest: $(TESTS)

# `make test` prints the name of each test file and then runs that test.
# running the test should indicate the tests run and whether they passed
# or failed.
test: buildtest
	$(foreach test, $(TESTS), @echo $(test):$(newline)@$(test)$(newline))

# --- core/ ------------------------------------------------------------------
#
# core/ is held to a stricter standard than the inherited tree.  The flags are
# separate from CFLAGS so that adding -Werror here cannot break the main build,
# and so the inherited tree's 177 warnings do not have to be fixed first.
CORE_CFLAGS = -std=c11 -Wall -Wextra -Werror -Wmissing-prototypes \
	-Wstrict-prototypes -Wshadow -Wconversion -Wcast-qual -Wwrite-strings
CORE_CPPFLAGS = -Icore -Isrc

core/%.o: core/%.c
	$(CC) $(CORE_CPPFLAGS) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

# `make core` builds the rebuilt layers.
core: $(CORE_OBJS)

# `make check-pure` enforces the rule that makes core/ testable: no mutable
# global state.  This is a link-time fact, not a code-review habit -- see
# core/README.md rule 1.
#
# The test is which *section* a symbol lands in, not what nm calls it.  nm
# reports a `static const` table containing pointers as `d`, which looks like
# mutable data but is not: in a position-independent build such a table goes to
# .data.rel.ro, which the loader maps read-only once relocations are applied.
# Keying on nm's letter would reject every const table with a string in it.
#
#   rejected:  .data  .bss  *COM*     genuinely writable for the whole run
#   allowed:   .rodata  .data.rel.ro*  .text
#
# It also checks that the core allocates nothing, so it can be driven from an
# embedded shell or another language without sharing an allocator.  That catches
# direct calls only, not transitive ones.
check-pure: $(CORE_OBJS)
	@fail=0; \
	for o in $(CORE_OBJS); do \
		syms=`objdump -t $$o \
			| grep -E '[[:space:]](\.data|\.bss|\*COM\*)[[:space:]]' \
			|| true`; \
		if [ -n "$$syms" ]; then \
			echo "FAIL: $$o has mutable global state:"; \
			echo "$$syms" | sed 's/^/    /'; \
			fail=1; \
		fi; \
		alloc=`nm --undefined-only $$o \
			| grep -E ' U (malloc|calloc|realloc|free|strdup)$$' || true`; \
		if [ -n "$$alloc" ]; then \
			echo "FAIL: $$o allocates:"; \
			echo "$$alloc" | sed 's/^/    /'; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "check-pure: $(words $(CORE_OBJS)) object(s), no mutable globals, no allocation"

# Every core header must compile standalone -- a header that forgets an include
# fails here rather than mysteriously later.  See core/README.md rule 7.
check-headers:
	@for h in `find core -name '*.h'`; do \
		echo "#include \"$${h#core/}\"" > /tmp/ardop-hdr-$$$$.c; \
		echo "int main(void){return 0;}" >> /tmp/ardop-hdr-$$$$.c; \
		$(CC) $(CORE_CPPFLAGS) $(CORE_CFLAGS) -fsyntax-only \
			/tmp/ardop-hdr-$$$$.c || exit 1; \
		rm -f /tmp/ardop-hdr-$$$$.c; \
	done; \
	echo "check-headers: all core headers compile standalone"

# `make test-core` builds and runs the core unit tests.
test-core: $(CORE_TESTS)
	$(foreach test, $(CORE_TESTS), @echo $(test):$(newline)@$(test)$(newline))

# Core tests link the inherited objects too, because the equivalence tests use
# the old implementation as an oracle.  That dependency disappears when the old
# code does.
test/core/test_%: test/core/test_%.c $(CORE_OBJS) $(OBJS) $(TEST_OBJS_COMMON)
	$(CC) \
		$(CPPFLAGS) $(CORE_CPPFLAGS) -Itest/ardop \
		$(CFLAGS) \
		$(LDFLAGS) \
		$< \
		$(CORE_OBJS) $(OBJS) $(TEST_OBJS_COMMON) \
		$(LDLIBS) -lcmocka \
		-o $@

# `make golden` checks this build against the committed golden-vector corpus
# in test/golden: that the modulator is still bit-exact, and that every
# frozen recording still decodes to the bytes it was made from.  It needs
# only python3, no additional libraries.  See test/golden/README.md.
golden: all
	cd test/golden && ./test_golden.py

# `make golden-regen` rewrites that corpus from the current build, moving the
# baseline.  Review the resulting diff before committing: a changed tx_sha256
# means the modulated audio changed.
golden-regen: all
	cd test/golden && ./gen_golden.py

# rule to make test-case executables from their sources
test/ardop/test_%: test/ardop/test_%.c $(OBJS) $(TEST_OBJS_COMMON)
	$(CC) \
		$(CPPFLAGS) \
		$(CFLAGS) \
		$(LDFLAGS) \
		$(patsubst %,$(LDWRAP)%,$(WRAP)) \
		$< \
		$(OBJS) \
		$(TEST_OBJS_COMMON) \
		-o $@ \
		$(LOADLIBES) \
		$(LDLIBS) \
		-lcmocka

# linkage overrides for unit tests
#   for tests that need only a subset of production code,
#   set OBJS to the .o files you want
#
#   for tests that need mock functions injected,
#   set WRAP to a space-separated list of functions to mock
test/ardop/test_log: OBJS := \
	src/common/log_file.o \
	src/common/log.o
test/ardop/test_log: WRAP := fopen fclose fwrite fflush freopen

-include *.d

# 'make clean' deletes files produced by the build process.
# After using git checkout change branches, it is sometimes neccessary to run
# 'make clean' before running 'make' to produce a successful build.  Failure
# to run 'make clean' before using git checkout may sometimes leave build
# related files that must then be manually deleted.
CLEAN += \
	ardopcf \
	ardopcf.exe \
	$(OBJS) \
	$(OBJS:.o=.d) \
	$(OBJS_LIN) \
	$(OBJS_LIN:.o=.d) \
	$(OBJS_WIN) \
	$(OBJS_WIN:.o=.d) \
	$(OBJS_EXE) \
	$(OBJS_EXE:.o=.d) \
	$(TESTS) \
	$(TESTS:%=%.exe) \
	$(TESTS:%=%.d) \
	$(TEST_OBJS_COMMON) \
	$(TEST_OBJS_COMMON:.o=.d) \
	output.map \

ifeq ($(OS),Windows_NT)
# on Windows, del requires backslash paths
clean :
	del /Q /F $(subst /,\,$(CLEAN))
else
clean :
	rm -f -- $(CLEAN)
endif
