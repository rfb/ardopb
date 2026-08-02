cmake_minimum_required(VERSION 3.16)
project(ardop-gui LANGUAGES C CXX)

#
# ardop-gui -- the instrument panel for a running ardopb.
#
# A separate CMake project rather than a root-Makefile target: the modem is C
# built by make, this is C++ built by Qt's toolchain, and keeping the two build
# systems from leaking into each other is the point. Nothing here links the
# modem -- only shell/telemetry.c, so the wire format has exactly one
# definition, shared by the writer and the reader.
#
# Build:
#   cmake -S gui -B gui/build && cmake --build gui/build
#   ./gui/build/ardop-gui --host 127.0.0.1:8517
#
# Needs Qt 6 (Widgets + Network, both in qtbase). On Debian/Ubuntu:
#   sudo apt install qt6-base-dev cmake
#

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

set(ARDOP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(ardop-gui
	main.cpp
	mainwindow.cpp
	telemetryclient.cpp

	# Listed so AUTOMOC generates its meta-object: it declares the panel's
	# five signals but has no .cpp of its own.
	panelsource.h
	waterfallwidget.cpp
	constellationwidget.cpp
	gaugewidget.cpp
	statuslamps.cpp
	# The format definition itself, shared with the daemon.
	${ARDOP_ROOT}/shell/telemetry.c
	# The frame-type table, so a decoded frame can be named ("4PSK.200.100")
	# rather than shown as a bare hex byte. It is a self-contained const
	# table with no dependencies beyond <string.h>, and it is normative --
	# transcribing the 121 names into the GUI instead would be a second copy
	# of protocol data waiting to drift.
	${ARDOP_ROOT}/core/codec/frame.c
)

target_include_directories(ardop-gui PRIVATE
	${ARDOP_ROOT}
	${ARDOP_ROOT}/core
)

target_link_libraries(ardop-gui PRIVATE
	Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# The shared C file is held to the same bar as the rest of core/.
#
# Guarded on the compiler rather than assumed: these are GCC/Clang spellings and
# MSVC rejects them outright. MinGW -- what the Windows CI job uses -- takes them
# unchanged, so the Windows build is held to exactly the same bar as Linux.
if(NOT MSVC)
	set_source_files_properties(${ARDOP_ROOT}/shell/telemetry.c PROPERTIES
		COMPILE_OPTIONS "-std=c11;-Wall;-Wextra;-Wconversion")
endif()

if(WIN32)
	# A GUI subsystem binary, so double-clicking it does not also open a
	# console window behind the panel. Diagnostics go to the telemetry
	# stream and the modem's own stderr, not to this process's.
	set_target_properties(ardop-gui PROPERTIES WIN32_EXECUTABLE ON)
endif()
