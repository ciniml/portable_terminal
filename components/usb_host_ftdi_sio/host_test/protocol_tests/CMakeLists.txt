cmake_minimum_required(VERSION 3.16)
project(ftdi_protocol_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Find Catch2 (optional - can be installed via package manager)
# sudo apt-get install catch2 (Ubuntu/Debian)
# Or use FetchContent to download it
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(Catch2)

# Add source files to compile
add_executable(ftdi_protocol_tests
    test_ftdi_protocol.cpp
    ../../src/ftdi_host_protocol.c
)

target_include_directories(ftdi_protocol_tests PRIVATE
    ../../include
    ../../private_include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/esp_mock  # ESP-IDF mock headers for Linux
)

target_link_libraries(ftdi_protocol_tests PRIVATE Catch2::Catch2WithMain)

# Enable CTest
enable_testing()
add_test(NAME ftdi_protocol_tests COMMAND ftdi_protocol_tests)

# Compile options
target_compile_options(ftdi_protocol_tests PRIVATE -Wall -Wextra)
