cmake_minimum_required(VERSION 3.22)
project(term_core_host_test CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/term_core
                 ${CMAKE_CURRENT_BINARY_DIR}/term_core_build)

enable_testing()

add_executable(term_core_tests
    tests/parser_test.cpp
    tests/screen_test.cpp
    tests/terminal_golden_test.cpp
    tests/wide_char_test.cpp
)
target_link_libraries(term_core_tests PRIVATE term_core gtest_main gmock)
target_include_directories(term_core_tests PRIVATE fakes)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(term_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GoogleTest)
gtest_discover_tests(term_core_tests)
