# Toolchain file for the emulator's firmware build: the repo's own
# cmake/ArduinoToolchain.cmake, loaded unchanged, behind a compatibility shim,
# with the Arduino SDK always named by ../Makefile.
#
# The repo's toolchain file includes Platform/UnixPaths before project() has
# set CMake up. CMake 3.25 and later call the internal helper below from that
# module, and it is not defined that early (CMake also re-reads the toolchain
# file in every try_compile project, so the shim has to live here). The helper
# only records install prefixes, which a cross build for the AVR never uses.
if(NOT COMMAND _cmake_record_install_prefix)
    function(_cmake_record_install_prefix)
    endfunction()
endif()

# The SDK is the one ../Makefile passes as ARDUINO_SDK_PATH (the toolchain
# ../fetch_toolchain.sh fetched, or ARDUINO_SDK=). Without it, the repo's
# toolchain file goes looking for an Arduino IDE (/Applications/Arduino.app
# and the like). So a configure with no SDK named stops here, and the SDK is
# handed on to the test project CMake configures for every try_compile (the
# compiler checks of project()): such a project reads this file again but sees
# none of this build's cache.
if(NOT ARDUINO_SDK_PATH)
    message(FATAL_ERROR "ARDUINO_SDK_PATH is not set: build the firmware with make in emulator/")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARDUINO_SDK_PATH)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ArduinoToolchain.cmake")
