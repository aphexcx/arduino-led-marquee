cmake_minimum_required(VERSION 3.25.2)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/ArduinoToolchain.cmake)
set(PROJECT_NAME LED_Marquee)
project(${PROJECT_NAME})
set(${CMAKE_PROJECT_NAME}_SKETCH LED_Marquee.ino)
#target_link_libraries(${CMAKE_PROJECT_NAME}_SKETCH, Fonts.h)

#### Uncomment below additional settings as needed.
set(${CMAKE_PROJECT_NAME}_BOARD uno)
set(${CMAKE_PROJECT_NAME}_PORT /dev/tty.usbmodem4101)
# set(mega.build.mcu atmega2560)
# set(mega.upload.protocol wiring)
# set(mega.upload.speed 115200)

generate_arduino_firmware(${CMAKE_PROJECT_NAME})
