# Builds the UNMODIFIED sketch (../../LED_Marquee.ino and its headers) into an
# ELF + HEX for the emulator, with the repo's own arduino-cmake toolchain file,
# so the flags and the link order are the ones the flashed build uses. The
# compiler and the Arduino core are the ones ../Makefile names (ARDUINO_SDK,
# fetched by ../fetch_toolchain.sh). Run it through the Makefile (make
# firmware), never by hand in the repo root.
#
# Why this wrapper exists instead of pointing cmake at ../../CMakeLists.txt:
#   1. That file sets LED_Marquee_PORT, which makes arduino-cmake generate the
#      avrdude upload targets. This one sets no port, so no upload target exists
#      in the emulator's build tree at all.
#   2. Current CMake (>= 3.25) cannot load the repo's toolchain file as is;
#      toolchain.cmake next to this file wraps it with a one-function shim.
#      It also hands the SDK on to CMake's try_compile test projects, so that
#      no configure goes looking for an Arduino IDE.
# Keep the three settings below in step with ../../CMakeLists.txt.
cmake_minimum_required(VERSION 3.10)

get_filename_component(SIGN_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain.cmake)
set(PROJECT_NAME LED_Marquee)
project(${PROJECT_NAME})

set(${CMAKE_PROJECT_NAME}_SKETCH ${SIGN_REPO}/LED_Marquee.ino)
set(${CMAKE_PROJECT_NAME}_BOARD uno)

# arduino-cmake looks for the sketch's libraries (SoftwareSerial/, and the
# empty "Fonts" one it derives from Fonts.h) next to the top-level
# CMakeLists.txt; this adds the repo root to that search.
link_directories(${SIGN_REPO})

# Extra compiler flags for a second image of the same sketch, for example
# -DCHOOSER_DEBUG_PANEL_B=1 (make firmware-debug). Empty for the image that
# gets flashed for a show.
if(SIGN_FLAGS)
    add_definitions(${SIGN_FLAGS})
endif()

generate_arduino_firmware(${CMAKE_PROJECT_NAME})
