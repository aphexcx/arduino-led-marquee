# Toolchain file for the emulator's firmware build: the repo's own
# cmake/ArduinoToolchain.cmake, loaded unchanged, behind one compatibility shim.
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

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ArduinoToolchain.cmake")
