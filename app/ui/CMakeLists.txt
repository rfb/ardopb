cmake_minimum_required(VERSION 3.16)
project(ardop-station LANGUAGES C CXX)

#
# The station application: the modem embedded, with a window on it.
#
# A separate CMake project rather than a root-Makefile target, for gui/'s
# reason: the C build and the Qt build stay out of each other's way, and
# analysis/14 Decision 6 keeps the Makefile as the authority for the mechanical
# guarantees (check-pure, check-standalone) that only it can enforce.
#
#   make lib                                 # the C program, as one archive
#   cmake -S app/ui -B app/ui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build app/ui/build
#   ./app/ui/build/ardop-station
#
# Qt Widgets rather than Qt Quick, which analysis/16 chose. That decision was
# made when Android was in scope; with the port scoped to Linux and Windows,
# Widgets reuses gui/'s working instrument painting instead of reimplementing it.
# Recorded as an amendment to analysis/16 with the trigger for revisiting.
#

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

set(ARDOP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

#
# The C half arrives as one archive, built by the Makefile.
#
# Listing the objects here instead would be a second definition of a set that
# already exists, and analysis/15 recorded what happens next: it drifts. This way
# adding a file to any group in the Makefile is picked up here with no change at
# all.
#
add_custom_command(
    OUTPUT ${ARDOP_ROOT}/libardop.a
    COMMAND make lib
    WORKING_DIRECTORY ${ARDOP_ROOT}
    COMMENT "Building libardop.a (the C program)"
    VERBATIM)
add_custom_target(ardop_lib DEPENDS ${ARDOP_ROOT}/libardop.a)

add_executable(ardop-station
    main.cpp
    stationwindow.cpp
    devicespage.cpp
    modemthread.cpp
    spinesource.cpp

    # The instrument panel, promoted from gui/ rather than rewritten. These are
    # the same widgets the standalone remote panel uses, and they bind to
    # whichever source is present -- which is what makes --remote a mode of this
    # binary instead of a second program.
    ${ARDOP_ROOT}/gui/waterfallwidget.cpp
    ${ARDOP_ROOT}/gui/constellationwidget.cpp
    ${ARDOP_ROOT}/gui/gaugewidget.cpp
    ${ARDOP_ROOT}/gui/statuslamps.cpp
    ${ARDOP_ROOT}/gui/telemetryclient.cpp
)

add_dependencies(ardop-station ardop_lib)

target_include_directories(ardop-station PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ARDOP_ROOT}
    ${ARDOP_ROOT}/core
    ${ARDOP_ROOT}/gui
)

target_link_libraries(ardop-station PRIVATE
    ${ARDOP_ROOT}/libardop.a
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
)

if(WIN32)
    # WASAPI, the multimedia timer, and SetupAPI for CM108 enumeration. Matches
    # AUDIO_LDLIBS and PTT_LDLIBS in the Makefile.
    target_link_libraries(ardop-station PRIVATE ole32 winmm avrt setupapi ws2_32)
    set_target_properties(ardop-station PROPERTIES WIN32_EXECUTABLE ON)
else()
    # miniaudio dlopen()s ALSA, PulseAudio and JACK at run time, so there is no
    # audio library to link -- only the loader and the thread library.
    target_link_libraries(ardop-station PRIVATE dl pthread m)
endif()

# The C sources compiled here are held to the same bar as everywhere else.
# Guarded on the compiler, since these are GCC/Clang spellings and MSVC rejects
# them; MinGW takes them unchanged.
if(NOT MSVC)
    target_compile_options(ardop-station PRIVATE -Wall -Wextra)
endif()
