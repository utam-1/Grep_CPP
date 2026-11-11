cmake_minimum_required(VERSION 3.10)
project(grep-engine VERSION 1.0.0 LANGUAGES C CXX)

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(GREP_BUILD_TESTS "Build grep engine tests" ON)
option(GREP_BUILD_CLI "Build grep command line utility" ON)

# Standards: compatible down to C89/C99 and C++98/C++03/C++11+
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

if(NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED OFF)

# Grep Core Engine Library (usable in C & C++ projects)
add_library(grep_engine
    src/grep_engine.cpp
)

target_include_directories(grep_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(grep_engine PRIVATE -Wall -Wextra)
elseif(MSVC)
    target_compile_options(grep_engine PRIVATE /W4)
endif()

# CLI Executable
if(GREP_BUILD_CLI)
    add_executable(grep_cli
        src/main.cpp
    )
    target_link_libraries(grep_cli PRIVATE grep_engine)
    set_target_properties(grep_cli PROPERTIES OUTPUT_NAME "grep")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(grep_cli PRIVATE -Wall -Wextra)
    elseif(MSVC)
        target_compile_options(grep_cli PRIVATE /W4)
    endif()
endif()

# Tests
if(GREP_BUILD_TESTS)
    enable_testing()

    # C API Test (pure C89/C99)
    add_executable(test_c_api tests/test_c_api.c)
    target_link_libraries(test_c_api PRIVATE grep_engine)
    set_target_properties(test_c_api PROPERTIES C_STANDARD 99)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_c_api PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME test_c_api COMMAND test_c_api)

    # C++ API Test (C++98/C++03/C++11+)
    add_executable(test_cpp_api tests/test_cpp_api.cpp)
    target_link_libraries(test_cpp_api PRIVATE grep_engine)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_cpp_api PRIVATE -Wall -Wextra)
    endif()
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

install(EXPORT grep_engine_targets
    FILE grep_engineConfig.cmake
    NAMESPACE grep::
    DESTINATION lib/cmake/grep_engine
)

if(GREP_BUILD_CLI)
    install(TARGETS grep_cli DESTINATION bin)
endif()
