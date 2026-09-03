# The engine: pure DSP. Depends on the C++ standard library and vendored DSP
# code only (constitution IV). No platform, file-format, UI or threading headers.
add_library(opentune STATIC
    src/Version.cpp
)
target_include_directories(opentune PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(opentune PRIVATE opentune_warnings)
add_library(opentune::engine ALIAS opentune)
target_compile_definitions(opentune PRIVATE OPENTUNE_VERSION="${PROJECT_VERSION}")
