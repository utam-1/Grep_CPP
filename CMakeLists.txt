cmake_minimum_required(VERSION 3.10)
project(grep-engine VERSION 1.0.0 LANGUAGES C CXX)

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(GREP_BUILD_TESTS "Build grep engine tests" ON)
option(GREP_BUILD_CLI "Build grep command line utility" ON)

# Standards: keep C++ compatibility down to C++98 / C++03 / C++11
# If modern C++ is supported by the compiler, CMake will use it.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED OFF)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Grep Core Engine Library (usable in C & C++ projects)
add_library(grep_engine
    src/grep_engine.cpp
)

target_include_directories(grep_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# CLI Executable
if(GREP_BUILD_CLI)
    add_executable(grep_cli
        src/main.cpp
    )
    target_link_libraries(grep_cli PRIVATE grep_engine)
    set_target_properties(grep_cli PROPERTIES OUTPUT_NAME "grep")
endif()

# Tests
if(GREP_BUILD_TESTS)
    enable_testing()

    # C API Test (pure C90/C99)
    add_executable(test_c_api tests/test_c_api.c)
    target_link_libraries(test_c_api PRIVATE grep_engine)
    set_target_properties(test_c_api PROPERTIES C_STANDARD 99)
    add_test(NAME test_c_api COMMAND test_c_api)

    # C++ API Test (C++98/C++03/C++11+)
    add_executable(test_cpp_api tests/test_cpp_api.cpp)
    target_link_libraries(test_cpp_api PRIVATE grep_engine)
    add_test(NAME test_cpp_api COMMAND test_cpp_api)
endif()

# Installation rules
install(TARGETS grep_engine
    EXPORT grep_engine_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES include/grep_engine.h DESTINATION include)

if(GREP_BUILD_CLI)
    install(TARGETS grep_cli DESTINATION bin)
endif()
