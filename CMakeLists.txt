cmake_minimum_required(VERSION 3.20)
project(OpenTune VERSION 0.1.0 LANGUAGES CXX)

# ---- Language and warnings -------------------------------------------------
# C++17 everywhere; warnings are errors so real-time-safety mistakes surface
# at compile time rather than as clicks in a headphone.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(opentune_warnings INTERFACE)
target_compile_options(opentune_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)

# ---- Targets ---------------------------------------------------------------
add_subdirectory(engine)
add_subdirectory(tools/autotune-cli)
add_subdirectory(tools/f0-dump)

option(OPENTUNE_BUILD_TESTS "Build the test suite" ON)
if(OPENTUNE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Formatting gate -------------------------------------------------------
# `cmake --build build --target format-check` fails if any of our sources is not
# clang-formatted. Vendored code is excluded. Run before every commit.
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE OPENTUNE_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/engine/*.cpp ${CMAKE_SOURCE_DIR}/engine/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp  ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tools/*.cpp  ${CMAKE_SOURCE_DIR}/tools/*.h
        ${CMAKE_SOURCE_DIR}/apps/*.cpp   ${CMAKE_SOURCE_DIR}/apps/*.h)
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${OPENTUNE_FORMAT_SOURCES}
        COMMENT "Checking clang-format compliance")
endif()
