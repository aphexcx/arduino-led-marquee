# Builds the UNMODIFIED sketch (../../LED_Marquee.ino and its headers) into an
# ELF + HEX for the emulator, with the repo's own arduino-cmake toolchain file,
# so the compiler, flags, Arduino core and link order are the ones the flashed
# build uses. Run it through ../build_firmware.sh, never by hand in the repo root.
#
# Why this wrapper exists instead of pointing cmake at ../../CMakeLists.txt:
#   1. That file sets LED_Marquee_PORT, which makes arduino-cmake generate the
#      avrdude upload targets. This one sets no port, so no upload target exists
#      in the emulator's build tree at all.
#   2. Current CMake (>= 3.25) cannot load the repo's toolchain file as is;
#      toolchain.cmake next to this file wraps it with a one-function shim.
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

generate_arduino_firmware(${CMAKE_PROJECT_NAME})
