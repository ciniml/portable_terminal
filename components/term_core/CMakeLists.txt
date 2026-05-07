if(ESP_PLATFORM)
    idf_component_register(
        SRCS
            "src/parser.cpp"
            "src/screen.cpp"
            "src/terminal.cpp"
        INCLUDE_DIRS "include"
        REQUIRES )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
else()
    cmake_minimum_required(VERSION 3.22)
    add_library(term_core STATIC
        src/parser.cpp
        src/screen.cpp
        src/terminal.cpp)
    target_include_directories(term_core PUBLIC include)
    target_compile_features(term_core PUBLIC cxx_std_23)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(term_core PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
